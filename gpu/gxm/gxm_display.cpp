/*
 * The Vita's display (ps2gpu::Display over GXM): the picture the host hands
 * over - the software GS's for now - drawn by the GPU onto the 960 x 544
 * screen, filtered, at the aspect asked for (4:3 pillarboxed, 16:9 filling
 * it), with the overlay (the speed line) over its top left at the screen's
 * own pixels; three buffers through GXM's display queue.
 *
 * Present runs on the thread that draws (the GS thread); SetAspect and
 * SetOverlay may come from another, under a lock. Input is not the
 * display's here: the host reads the Vita's buttons through the platform
 * layer (Pad says there is no controller of its own).
 */

#include <psp2/display.h>
#include <psp2/gxm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "gxm_device.h"
#include "ps2gpu/display.h"

namespace ps2gpu {

namespace {

using namespace gxm;

constexpr std::uint32_t kWidth = 960, kHeight = 544, kPitch = 960;
constexpr int kBuffers = 3;
constexpr std::uint32_t kTextureWidth = 1024, kTextureHeight = 576;   // the largest picture taken (the GS's: 640 x 512)
constexpr std::uint32_t kOverlayWidth = 1024, kOverlayHeight = 64;

const char kVertexSource[] =
    "void main(float2 aPosition, float2 aTexcoord,\n"
    "          out float4 vPosition : POSITION, out float2 vTexcoord : TEXCOORD0) {\n"
    "    vPosition = float4(aPosition, 0.5f, 1.0f);\n"
    "    vTexcoord = aTexcoord;\n"
    "}\n";

// The picture over the whole screen: outside [0, 1] (the pillarbox or
// letterbox) black, so one quad draws every pixel.
const char kPictureSource[] =
    "float4 main(float2 vTexcoord : TEXCOORD0, uniform sampler2D uTexture : TEXUNIT0,\n"
    "            uniform float2 uScale) : COLOR {\n"
    "    float4 c = tex2D(uTexture, vTexcoord * uScale);\n"
    "    float inside = step(0.0f, vTexcoord.x) * step(vTexcoord.x, 1.0f) *\n"
    "                   step(0.0f, vTexcoord.y) * step(vTexcoord.y, 1.0f);\n"
    "    return float4(c.rgb * inside, 1.0f);\n"
    "}\n";

const char kOverlaySource[] =
    "float4 main(float2 vTexcoord : TEXCOORD0, uniform sampler2D uTexture : TEXUNIT0) : COLOR {\n"
    "    return float4(tex2D(uTexture, vTexcoord).rgb, 1.0f);\n"
    "}\n";

struct Vertex {
    float x, y, u, v;
};

std::string Hex(int value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08X", unsigned(value));
    return text;
}

class GxmDisplay final : public Display {
public:
    bool Start(std::string* error);

    bool Present(const std::uint32_t* pixels, std::uint32_t width, std::uint32_t height) override;
    bool PresentImage(std::uint64_t image, int layout, std::uint32_t width, std::uint32_t height) override;
    bool Pump() override { return true; }
    bool Focused() const override { return true; }
    bool KeyDown(int) const override { return false; }
    std::string Describe() const override { return "GXM, the Vita's GPU (960 x 544)"; }
    Gpu* SharedGpu() override { return nullptr; }

    void SetAspect(float aspect) override {
        std::lock_guard<std::mutex> hold(lock_);
        aspect_ = aspect;
    }
    void SetOverlay(std::vector<std::uint32_t> pixels, std::uint32_t width, std::uint32_t height) override {
        std::lock_guard<std::mutex> hold(lock_);
        overlay_pixels_ = std::move(pixels);
        overlay_width_ = std::min(width, kOverlayWidth);
        overlay_height_ = std::min(height, kOverlayHeight);
        if (overlay_pixels_.size() < std::size_t(width) * height) overlay_width_ = overlay_height_ = 0;
        overlay_dirty_ = true;
    }
    double PresentsPerSecond() const override { return presents_per_second_.load(); }
    std::uint32_t PixelHeight() const override { return kHeight; }
    float WindowAspect() const override { return float(kWidth) / float(kHeight); }

private:
    SceGxmFragmentProgram* MakeFragment(SceGxmShaderPatcherId id, std::string* error);
    /* A texture onto the screen: the part of it scale_u, scale_v across and
       down, at the aspect asked for, the overlay over it; then queued for
       the display. */
    void Show(SceGxmTexture texture, float scale_u, float scale_v);

    Device* device_ = nullptr;
    SceGxmRenderTarget* target_ = nullptr;
    Memory buffers_memory_, depth_memory_, stencil_memory_, texture_memory_, overlay_memory_, vertex_memory_;
    void* buffers_[kBuffers] = {};
    SceGxmColorSurface surfaces_[kBuffers]{};
    SceGxmSyncObject* syncs_[kBuffers] = {};
    SceGxmDepthStencilSurface depth_{};
    int front_ = 0, back_ = 1;

    const SceGxmProgram* vertex_gxp_ = nullptr;
    const SceGxmProgram* picture_gxp_ = nullptr;
    SceGxmVertexProgram* vertex_program_ = nullptr;
    SceGxmFragmentProgram* picture_program_ = nullptr;
    SceGxmFragmentProgram* overlay_program_ = nullptr;
    const SceGxmProgramParameter* scale_ = nullptr;
    SceGxmTexture texture_{}, overlay_texture_{};
    Vertex* vertices_ = nullptr;
    std::uint16_t* indices_ = nullptr;

    std::mutex lock_;
    float aspect_ = 4.0f / 3.0f;
    std::vector<std::uint32_t> overlay_pixels_;
    std::uint32_t overlay_width_ = 0, overlay_height_ = 0;
    bool overlay_dirty_ = false;
    std::uint32_t overlay_shown_w_ = 0, overlay_shown_h_ = 0;    // what the overlay texture holds (Present's)

    std::deque<std::chrono::steady_clock::time_point> presented_;
    std::atomic<double> presents_per_second_{0.0};
    std::atomic<std::int32_t> pending_{0};      // queued for the screen, not yet shown (DisplayCallback)
};

SceGxmFragmentProgram* GxmDisplay::MakeFragment(SceGxmShaderPatcherId id, std::string* error) {
    SceGxmFragmentProgram* program = nullptr;
    const int r = sceGxmShaderPatcherCreateFragmentProgram(device_->patcher(), id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                           SCE_GXM_MULTISAMPLE_NONE, nullptr, vertex_gxp_, &program);
    if (r < 0) {
        *error = "sceGxmShaderPatcherCreateFragmentProgram " + Hex(r);
        return nullptr;
    }
    return program;
}

bool GxmDisplay::Start(std::string* error) {
    device_ = &TheDevice();
    if (!device_->Start(error)) return false;
    // The programs first: without libshacccg there is no display (the host
    // falls back to the CPU's picture), and nothing else need be made.
    SceGxmShaderPatcherId vertex_id = nullptr, picture_id = nullptr, overlay_id = nullptr;
    vertex_gxp_ = device_->Program("display_vertex", kVertexSource, false, error, &vertex_id);
    if (!vertex_gxp_) return false;
    picture_gxp_ = device_->Program("display_picture", kPictureSource, true, error, &picture_id);
    if (!picture_gxp_) return false;
    if (!device_->Program("display_overlay", kOverlaySource, true, error, &overlay_id)) return false;
    const SceGxmProgramParameter* position = sceGxmProgramFindParameterByName(vertex_gxp_, "aPosition");
    const SceGxmProgramParameter* texcoord = sceGxmProgramFindParameterByName(vertex_gxp_, "aTexcoord");
    scale_ = sceGxmProgramFindParameterByName(picture_gxp_, "uScale");
    if (!position || !texcoord || !scale_) {
        *error = "the display's programs lack their parameters";
        return false;
    }
    SceGxmVertexAttribute attributes[2]{};
    attributes[0].streamIndex = 0;
    attributes[0].offset = 0;
    attributes[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attributes[0].componentCount = 2;
    attributes[0].regIndex = std::uint16_t(sceGxmProgramParameterGetResourceIndex(position));
    attributes[1] = attributes[0];
    attributes[1].offset = 8;
    attributes[1].regIndex = std::uint16_t(sceGxmProgramParameterGetResourceIndex(texcoord));
    SceGxmVertexStream stream{};
    stream.stride = sizeof(Vertex);
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    int r = sceGxmShaderPatcherCreateVertexProgram(device_->patcher(), vertex_id, attributes, 2, &stream, 1,
                                                   &vertex_program_);
    if (r < 0) {
        *error = "sceGxmShaderPatcherCreateVertexProgram " + Hex(r);
        return false;
    }
    picture_program_ = MakeFragment(picture_id, error);
    overlay_program_ = MakeFragment(overlay_id, error);
    if (!picture_program_ || !overlay_program_) return false;

    SceGxmRenderTargetParams rt{};
    rt.flags = 0;
    rt.width = kWidth;
    rt.height = kHeight;
    rt.scenesPerFrame = 1;
    rt.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rt.multisampleLocations = 0;
    rt.driverMemBlock = -1;
    r = sceGxmCreateRenderTarget(&rt, &target_);
    if (r < 0) {
        *error = "sceGxmCreateRenderTarget " + Hex(r);
        return false;
    }
    const std::uint32_t buffer_bytes = kPitch * kHeight * 4;
    if (!Allocate(&buffers_memory_, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, buffer_bytes * kBuffers,
                  SCE_GXM_MEMORY_ATTRIB_RW, "gxm display buffers")) {
        *error = "no CDRAM for the display's buffers";
        return false;
    }
    std::memset(buffers_memory_.base, 0, buffers_memory_.size);
    for (int i = 0; i < kBuffers; ++i) {
        buffers_[i] = static_cast<std::uint8_t*>(buffers_memory_.base) + std::size_t(i) * buffer_bytes;
        r = sceGxmColorSurfaceInit(&surfaces_[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
                                   SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, kWidth, kHeight,
                                   kPitch, buffers_[i]);
        if (r < 0) {
            *error = "sceGxmColorSurfaceInit " + Hex(r);
            return false;
        }
        r = sceGxmSyncObjectCreate(&syncs_[i]);
        if (r < 0) {
            *error = "sceGxmSyncObjectCreate " + Hex(r);
            return false;
        }
    }
    // Depth and stencil, S8D24 and tiled: nothing tests them, but a scene
    // with none fails every draw (the GXM probe's first version).
    const std::uint32_t tiled_w = (kWidth + SCE_GXM_TILE_SIZEX - 1) & ~(SCE_GXM_TILE_SIZEX - 1);
    const std::uint32_t tiled_h = (kHeight + SCE_GXM_TILE_SIZEY - 1) & ~(SCE_GXM_TILE_SIZEY - 1);
    if (!Allocate(&depth_memory_, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * tiled_w * tiled_h,
                  SCE_GXM_MEMORY_ATTRIB_RW, "gxm display depth") ||
        !Allocate(&stencil_memory_, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * tiled_w * tiled_h,
                  SCE_GXM_MEMORY_ATTRIB_RW, "gxm display stencil")) {
        *error = "no memory for the display's depth surface";
        return false;
    }
    r = sceGxmDepthStencilSurfaceInit(&depth_, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                      tiled_w, depth_memory_.base, stencil_memory_.base);
    if (r < 0) {
        *error = "sceGxmDepthStencilSurfaceInit " + Hex(r);
        return false;
    }
    if (!Allocate(&texture_memory_, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, kTextureWidth * kTextureHeight * 4,
                  SCE_GXM_MEMORY_ATTRIB_READ, "gxm display picture") ||
        !Allocate(&overlay_memory_, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, kOverlayWidth * kOverlayHeight * 4,
                  SCE_GXM_MEMORY_ATTRIB_READ, "gxm display overlay") ||
        !Allocate(&vertex_memory_, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4096, SCE_GXM_MEMORY_ATTRIB_READ,
                  "gxm display vertices")) {
        *error = "no memory for the display's textures";
        return false;
    }
    vertices_ = static_cast<Vertex*>(vertex_memory_.base);          // 8 a buffer: the picture's, the overlay's
    indices_ = reinterpret_cast<std::uint16_t*>(vertices_ + 8 * kBuffers);
    for (std::uint16_t i = 0; i < 8; ++i) indices_[i] = i;
    std::fprintf(stderr, "[gxm] display: 960 x 544, %d buffers\n", kBuffers);
    return true;
}

bool GxmDisplay::Present(const std::uint32_t* pixels, std::uint32_t width, std::uint32_t height) {
    if (!pixels || width == 0 || height == 0 || width > kTextureWidth || height > kTextureHeight) return true;
    // What the GPU drew last is done before the picture's texture is written
    // over (a picture's quad: well under a millisecond).
    device_->CloseOpenScene();
    sceGxmFinish(device_->context());
    // The picture, its rows 8 texels apart (a linear texture's stride).
    const std::uint32_t stride = (width + 7) & ~7u;
    std::uint32_t* texels = static_cast<std::uint32_t*>(texture_memory_.base);
    for (std::uint32_t y = 0; y < height; ++y)
        std::memcpy(texels + std::size_t(y) * stride, pixels + std::size_t(y) * width, width * 4);
    sceGxmTextureInitLinear(&texture_, texels, SCE_GXM_TEXTURE_FORMAT_A8R8G8B8, stride, height, 0);
    Show(texture_, float(width) / float(stride), 1.0f);
    return true;
}

bool GxmDisplay::PresentImage(std::uint64_t image, int layout, std::uint32_t width, std::uint32_t height) {
    (void)layout;
    if (!image || !width || !height) return false;
    // A GS renderer's target (gxm_gs_renderer.cpp's DisplayTarget): its
    // pixels as a texture, drawn after what drew them - the GPU takes scenes
    // in order.
    device_->CloseOpenScene();
    SceGxmTexture texture = *reinterpret_cast<const SceGxmTexture*>(std::uintptr_t(image));
    const float tw = float(sceGxmTextureGetWidth(&texture)), th = float(sceGxmTextureGetHeight(&texture));
    Show(texture, std::min(1.0f, float(width) / tw), std::min(1.0f, float(height) / th));
    return true;
}

void GxmDisplay::Show(SceGxmTexture texture, float scale_u, float scale_v) {
    // Two pictures already wait for the screen: this one would only wait
    // behind them (the newest goes at the next present).
    if (pending_.load(std::memory_order_acquire) >= 2) return;
    sceGxmTextureSetMinFilter(&texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(&texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetUAddrMode(&texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    float aspect;
    {
        std::lock_guard<std::mutex> hold(lock_);
        aspect = aspect_;
        if (overlay_dirty_) {
            overlay_dirty_ = false;
            sceGxmFinish(device_->context());       // the last overlay drawn, done with
            overlay_shown_w_ = overlay_width_;
            overlay_shown_h_ = overlay_height_;
            if (overlay_shown_w_ && overlay_shown_h_) {
                const std::uint32_t overlay_stride = (overlay_shown_w_ + 7) & ~7u;
                std::uint32_t* out = static_cast<std::uint32_t*>(overlay_memory_.base);
                for (std::uint32_t y = 0; y < overlay_shown_h_; ++y)
                    std::memcpy(out + std::size_t(y) * overlay_stride, overlay_pixels_.data() + std::size_t(y) * overlay_width_,
                                overlay_shown_w_ * 4);
                sceGxmTextureInitLinear(&overlay_texture_, out, SCE_GXM_TEXTURE_FORMAT_A8R8G8B8, overlay_stride,
                                        overlay_shown_h_, 0);
                sceGxmTextureSetMinFilter(&overlay_texture_, SCE_GXM_TEXTURE_FILTER_POINT);
                sceGxmTextureSetMagFilter(&overlay_texture_, SCE_GXM_TEXTURE_FILTER_POINT);
            }
        }
    }

    // Where the picture goes: the screen's height at `aspect`, or its width
    // where that is wider (0: the whole screen). The quad covers the screen;
    // its texture coordinates run 0 to 1 across the picture's place.
    float shown_w = float(kWidth), shown_h = float(kHeight);
    if (aspect > 0.0f) {
        shown_w = kHeight * aspect;
        if (shown_w > kWidth) {
            shown_w = float(kWidth);
            shown_h = kWidth / aspect;
        }
    }
    const float left = (kWidth - shown_w) / 2, top = (kHeight - shown_h) / 2;
    const float u0 = -left / shown_w, u1 = (kWidth - left) / shown_w;
    const float v0 = -top / shown_h, v1 = (kHeight - top) / shown_h;
    // This buffer's vertices (the GPU may still read the others'). Clip
    // space, y up; the strip's order: top left, bottom left, top right,
    // bottom right.
    Vertex* v = vertices_ + 8 * back_;
    v[0] = {-1.0f, 1.0f, u0, v0};
    v[1] = {-1.0f, -1.0f, u0, v1};
    v[2] = {1.0f, 1.0f, u1, v0};
    v[3] = {1.0f, -1.0f, u1, v1};
    const bool overlay = overlay_shown_w_ && overlay_shown_h_;
    if (overlay) {
        const float x0 = -1.0f + 2.0f * 4 / kWidth, y0 = 1.0f - 2.0f * 2 / kHeight;
        const float x1 = x0 + 2.0f * overlay_shown_w_ / kWidth, y1 = y0 - 2.0f * overlay_shown_h_ / kHeight;
        const float u = float(overlay_shown_w_) / float((overlay_shown_w_ + 7) & ~7u);
        v[4] = {x0, y0, 0.0f, 0.0f};
        v[5] = {x0, y1, 0.0f, 1.0f};
        v[6] = {x1, y0, u, 0.0f};
        v[7] = {x1, y1, u, 1.0f};
    }

    SceGxmContext* context = device_->context();
    int r = sceGxmBeginScene(context, 0, target_, nullptr, nullptr, syncs_[back_], &surfaces_[back_], &depth_);
    if (r < 0) {
        std::fprintf(stderr, "[gxm] sceGxmBeginScene %s\n", Hex(r).c_str());
        return;
    }
    sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);
    sceGxmSetViewport(context, kWidth / 2.0f, kWidth / 2.0f, kHeight / 2.0f, -(kHeight / 2.0f), 0.5f, 0.5f);
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0, kWidth - 1, kHeight - 1);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetVertexProgram(context, vertex_program_);
    sceGxmSetFragmentProgram(context, picture_program_);
    void* uniforms = nullptr;
    sceGxmReserveFragmentDefaultUniformBuffer(context, &uniforms);
    const float scale[2] = {scale_u, scale_v};
    sceGxmSetUniformDataF(uniforms, scale_, 0, 2, scale);
    sceGxmSetFragmentTexture(context, 0, &texture);
    sceGxmSetVertexStream(context, 0, v);
    sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, indices_, 4);
    if (overlay) {
        sceGxmSetFragmentProgram(context, overlay_program_);
        sceGxmSetFragmentTexture(context, 0, &overlay_texture_);
        sceGxmSetVertexStream(context, 0, v);
        sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, indices_ + 4, 4);
    }
    sceGxmEndScene(context, nullptr, nullptr);
    sceGxmPadHeartbeat(&surfaces_[back_], syncs_[back_]);
    DisplayEntry entry{buffers_[back_], kWidth, kHeight, kPitch, &pending_};
    pending_.fetch_add(1, std::memory_order_acq_rel);
    sceGxmDisplayQueueAddEntry(syncs_[front_], syncs_[back_], &entry);
    front_ = back_;
    back_ = (back_ + 1) % kBuffers;

    // Pictures a second, over the last second.
    const auto now = std::chrono::steady_clock::now();
    presented_.push_back(now);
    while (!presented_.empty() && now - presented_.front() > std::chrono::seconds(1)) presented_.pop_front();
    presents_per_second_.store(double(presented_.size()));
}

}  // namespace

std::unique_ptr<Display> CreateGxmDisplay(std::string* error) {
    auto display = std::make_unique<GxmDisplay>();
    if (!display->Start(error)) return nullptr;
    return display;
}

}  // namespace ps2gpu
