/*
 * GsRenderer on GXM (gs_renderer.h): the Vita's GS on its GPU, as the Vulkan
 * renderer is the PC's - the bridge (engine/host/src/gpu_bridge.cpp) drives
 * both the same way. Compiled into the host on the Vita (it reads GS local
 * memory with the host's gs::Memory).
 *
 * The first version, with the Vita's memory in mind - the CPU and the GPU
 * share it:
 *
 *  - Targets are RGBA8 colour surfaces in CDRAM, linear, one per (FBP, FBW,
 *    PSM), at the native resolution; depth buffers float depth (DF32, Z /
 *    2^32 as the Vulkan renderer keeps it) with an 8-bit stencil, linear,
 *    loaded and stored with every scene (GXM keeps depth on the chip
 *    otherwise), one per (ZBP, size).
 *  - A target switch ends the scene and begins another (a tile-based GPU's
 *    unit of work); colour comes back from memory at each (the surface's own
 *    background texture), depth by the force-load.
 *  - Blending is the fixed-function blender's. The GS's factor - the source
 *    alpha or FIX, 0x80 being 1.0 - goes out as the fragment's alpha, which
 *    the blender reads (SRC_ALPHA); where alpha is written too, a second
 *    pass writes it (colour masked off). The destination alpha as the factor
 *    (Ad) is read at half its strength (over 255, not 128), as the Vulkan
 *    renderer's blender does.
 *  - The alpha test discards in the fragment program; AFAIL's writes come
 *    in a second pass over the failing pixels, as on the PC.
 *  - GS local memory "on the GPU" is a copy the CPU keeps (gs::Memory):
 *    targets and depth buffers swizzled into it and out of it by the CPU,
 *    textures decoded from it by the CPU, after the GPU has finished
 *    (sceGxmFinish). Each frame ends with the GPU finished too (Kick).
 *    Simple and exact; fragment passes take over where it costs too much.
 *  - Programs are Cg compiled on the Vita (gxm_device.cpp): one vertex
 *    program, fragment programs by what a batch needs (Variant), each with
 *    the blends it is used with (the shader patcher bakes the blend in).
 *
 * The destination alpha test (DATE) as on the PC: the target's alpha bit
 * into the stencil by a pass at the start of a scene, the batch stencil-
 * tested; PABE as two passes, blended and not, by the source alpha.
 *
 * Not yet: an internal resolution above 1x, FBMSK within a channel, 16-bit targets' five bits a channel
 * when blending, PSMT8H through the CLUT, the region clamp and repeat modes
 * when sampling (decoded textures have them baked in), mipmaps, a texture
 * pack.
 */

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "gxm_device.h"
#include "ps2gpu/gs_renderer.h"
#include "ps2host/gs.h"

namespace ps2gpu {

namespace {

using namespace gxm;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

std::string Hex(int value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08X", unsigned(value));
    return text;
}

GpuWaitObserver g_wait_observer = nullptr;
std::uint64_t g_draws = 0, g_scenes = 0, g_waits = 0;      // RendererCounters', since the last call

std::uint64_t WaitClock() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}

// ---------------------------------------------------------------- a heap

/* Pieces of one big block of GPU memory (CDRAM): surfaces and textures,
   which the system would otherwise hand out 256 KB at a time. First fit,
   free pieces merged with their neighbours. */
class Heap {
public:
    bool Init(std::uint32_t size, SceKernelMemBlockType type, const char* name) {
        if (!Allocate(&block_, type, size, SCE_GXM_MEMORY_ATTRIB_RW, name)) return false;
        free_[0] = block_.size;
        return true;
    }
    void* Take(std::uint32_t bytes, std::uint32_t align = 256) {
        bytes = (bytes + 255u) & ~255u;
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            const std::uint32_t start = (it->first + align - 1) & ~(align - 1);
            const std::uint32_t end = it->first + it->second;
            if (start + bytes > end) continue;
            const std::uint32_t before = start - it->first, after = end - (start + bytes);
            const std::uint32_t at = it->first;
            free_.erase(it);
            if (before) free_[at] = before;
            if (after) free_[start + bytes] = after;
            used_[start] = bytes;
            in_use_ += bytes;
            return static_cast<u8*>(block_.base) + start;
        }
        return nullptr;
    }
    void Give(void* pointer) {
        if (!pointer) return;
        const std::uint32_t offset = std::uint32_t(static_cast<u8*>(pointer) - static_cast<u8*>(block_.base));
        auto used = used_.find(offset);
        if (used == used_.end()) return;
        std::uint32_t start = offset, size = used->second;
        in_use_ -= size;
        used_.erase(used);
        auto next = free_.lower_bound(start);
        if (next != free_.end() && next->first == start + size) {
            size += next->second;
            next = free_.erase(next);
        }
        if (next != free_.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start) {
                start = prev->first;
                size += prev->second;
                free_.erase(prev);
            }
        }
        free_[start] = size;
    }
    std::uint32_t in_use() const { return in_use_; }
    std::uint32_t size() const { return block_.size; }

private:
    Memory block_;
    std::map<std::uint32_t, std::uint32_t> free_;   // offset -> size
    std::unordered_map<std::uint32_t, std::uint32_t> used_;
    std::uint32_t in_use_ = 0;
};

// ---------------------------------------------------------------- the programs

/* What the GPU takes per vertex: the position in clip space (x, y from the
   GS's pixel position, z the GS's Z over 2^32), S, T, Q, the colour and F. */
struct GxmVertex {
    float x, y, z;
    float s, t, q;
    u8 r, g, b, a;
    u8 fog, pad[3];
};
static_assert(sizeof(GxmVertex) == 32, "a vertex is 32 bytes");

const char kVertexSource[] =
    "void main(float3 aPosition, float4 aColor, float3 aStq, float4 aFog,\n"
    "          out float4 vPosition : POSITION, out float4 vColor : COLOR0,\n"
    "          out float3 vStq : TEXCOORD0, out float vFog : TEXCOORD1) {\n"
    "    vPosition = float4(aPosition, 1.0f);\n"
    "    vColor = aColor;\n"
    "    vStq = aStq;\n"
    "    vFog = aFog.x;\n"
    "}\n";

/* A fragment program's kind, packed:
     bit 0      textured
     bits 1-2   TFX (MODULATE, DECAL, HIGHLIGHT, HIGHLIGHT2)
     bit 3      TCC
     bit 4      fog
     bits 5-8   the alpha test: 0 none, else ATST + 1 (NEVER .. NOTEQUAL)
     bit 9      ...the failing pixels drawn (AFAIL's pass), else the passing
     bits 10-11 the alpha written: 0 the GS's, 1 its blend factor As (As over
                0x80), 2 FIX (uParams.y)
     bits 12-13 a target sampled as a texture: 0 as it is (32-bit), 1 24-bit
                (TEXA's alpha), 2 16-bit (five bits a channel, the alpha
                bit's TEXA)
     bits 14-15 PABE's passes: 0 every pixel, 1 those with As >= 0x80 (blended),
                2 those below (written as they are)
   And on its own, kDateSetup | DATM: the destination alpha test's setup -
   the pixels of the target sampled where its alpha bit equals DATM kept
   (the stencil set there), the rest discarded.
*/
using Variant = u32;
constexpr Variant kTextured = 1u, kTcc = 1u << 3, kFog = 1u << 4, kFailing = 1u << 9;
constexpr int kTfxShift = 1, kAtstShift = 5, kAlphaOutShift = 10, kTexFormatShift = 12, kPabeShift = 14;
constexpr Variant kDateSetup = 1u << 20;

std::string FragmentSource(Variant v) {
    if (v & kDateSetup) {
        char text[512];
        std::snprintf(text, sizeof text,
                      "float4 main(float3 vStq : TEXCOORD0, uniform sampler2D uTexture : TEXUNIT0) : COLOR {\n"
                      "    float a = tex2D(uTexture, vStq.xy).a;\n"
                      "    if ((a >= (127.5f / 255.0f)) != %s) discard;\n"
                      "    return float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
                      "}\n",
                      (v & 1u) ? "true" : "false");
        return text;
    }
    char defines[320];
    std::snprintf(defines, sizeof defines,
                  "#define TEXTURED %u\n#define TFX %u\n#define TCC %u\n#define FOG %u\n#define ATST %d\n"
                  "#define FAILING %u\n#define ALPHA_OUT %u\n#define TEX_FORMAT %u\n#define PABE %u\n",
                  v & 1u, (v >> kTfxShift) & 3u, (v >> 3) & 1u, (v >> 4) & 1u, int((v >> kAtstShift) & 15u) - 1,
                  (v >> 9) & 1u, (v >> kAlphaOutShift) & 3u, (v >> kTexFormatShift) & 3u, (v >> kPabeShift) & 3u);
    // Colours in the GS's terms: a byte 0-255 is 0-1 here, and the GS's
    // 0x80 = 1.0 where it multiplies is 128/255.
    static const char body[] =
        "float4 main(float4 vColor : COLOR0, float3 vStq : TEXCOORD0, float vFog : TEXCOORD1,\n"
        "            uniform sampler2D uTexture : TEXUNIT0,\n"
        "            uniform float4 uParams,      // AREF, the FIX factor, TA0, TA1 (all over 255)\n"
        "            uniform float4 uFog,         // FOGCOL's R, G, B over 255; AEM\n"
        "            uniform float2 uTexScale) : COLOR {\n"
        "    float3 rgb = vColor.rgb;\n"
        "    float a = vColor.a;\n"
        "#if TEXTURED\n"
        "    float4 t = tex2D(uTexture, vStq.xy / vStq.z * uTexScale);\n"
        "#if TEX_FORMAT == 1\n"
        "    t.a = (uFog.w > 0.5f && dot(t.rgb, float3(1.0f, 1.0f, 1.0f)) == 0.0f) ? 0.0f : uParams.z;\n"
        "#elif TEX_FORMAT == 2\n"
        "    float bit = t.a >= (127.5f / 255.0f) ? 1.0f : 0.0f;\n"
        "    t.rgb = floor(t.rgb * (255.0f / 8.0f) + 0.001f) * (8.0f / 255.0f);\n"
        "    float black = dot(t.rgb, float3(1.0f, 1.0f, 1.0f)) == 0.0f ? 1.0f : 0.0f;\n"
        "    t.a = bit > 0.5f ? uParams.w : ((uFog.w > 0.5f && black > 0.5f) ? 0.0f : uParams.z);\n"
        "#endif\n"
        "    const float k = 255.0f / 128.0f;\n"
        "#if TFX == 0\n"
        "    rgb = min(t.rgb * rgb * k, 1.0f);\n"
        "#if TCC\n"
        "    a = min(t.a * a * k, 1.0f);\n"
        "#endif\n"
        "#elif TFX == 1\n"
        "    rgb = t.rgb;\n"
        "#if TCC\n"
        "    a = t.a;\n"
        "#endif\n"
        "#else\n"
        "    rgb = min(t.rgb * rgb * k + a, 1.0f);\n"
        "#if TCC && TFX == 2\n"
        "    a = min(t.a + a, 1.0f);\n"
        "#elif TCC\n"
        "    a = t.a;\n"
        "#endif\n"
        "#endif\n"
        "#endif\n"
        "#if FOG\n"
        "    rgb = (vFog * rgb + (1.0f - vFog) * uFog.rgb) * (255.0f / 256.0f);\n"
        "#endif\n"
        "#if ATST >= 0\n"
        "    float a8 = floor(a * 255.0f + 0.5f), ref = floor(uParams.x * 255.0f + 0.5f);\n"
        "    bool pass = true;\n"
        "#if ATST == 0\n"
        "    pass = false;\n"
        "#elif ATST == 2\n"
        "    pass = a8 < ref;\n"
        "#elif ATST == 3\n"
        "    pass = a8 <= ref;\n"
        "#elif ATST == 4\n"
        "    pass = a8 == ref;\n"
        "#elif ATST == 5\n"
        "    pass = a8 >= ref;\n"
        "#elif ATST == 6\n"
        "    pass = a8 > ref;\n"
        "#elif ATST == 7\n"
        "    pass = a8 != ref;\n"
        "#endif\n"
        "#if FAILING\n"
        "    if (pass) discard;\n"
        "#else\n"
        "    if (!pass) discard;\n"
        "#endif\n"
        "#endif\n"
        "#if PABE == 1\n"
        "    if (a < (127.5f / 255.0f)) discard;\n"
        "#elif PABE == 2\n"
        "    if (a >= (127.5f / 255.0f)) discard;\n"
        "#endif\n"
        "#if ALPHA_OUT == 1\n"
        "    a = min(a * (255.0f / 128.0f), 1.0f);\n"
        "#elif ALPHA_OUT == 2\n"
        "    a = uParams.y;\n"
        "#endif\n"
        "    return float4(rgb, a);\n"
        "}\n";
    return std::string(defines) + body;
}

// ---------------------------------------------------------------- what is drawn into

struct Target {
    u32 fbp = 0, fbw = 0, psm = 0;
    u32 width = 0, height = 0, stride = 0;      // pixels; the stride a multiple of 8 (a linear texture's)
    u32* pixels = nullptr;                      // RGBA8, R first
    SceGxmColorSurface surface{};
    SceGxmTexture texture{};                    // the same pixels, to sample (render to texture, the display)
};

struct Depth {
    u32 width = 0, height = 0, stride = 0;      // samples; the stride a multiple of 32
    float* depth = nullptr;                     // Z / 2^32
    u8* stencil = nullptr;
    SceGxmDepthStencilSurface surface{};
};

struct Texture {
    u32 width = 0, height = 0, stride = 0;
    u32* pixels = nullptr;
    SceGxmTexture texture{};
};

bool Is16Bit(u32 psm) { return psm == 0x02 || psm == 0x0A || psm == 0x32 || psm == 0x3A; }
bool Is24Bit(u32 psm) { return psm == 0x01 || psm == 0x31; }

/* FBMSK as the bits of the target's RGBA bytes it keeps (as the Vulkan
   renderer's KeptBits: FBMSK is in RGBA32 positions whatever the format). */
u32 KeptBits(u32 fbmsk, u32 psm) {
    if (Is16Bit(psm)) return fbmsk | ~0x80F8F8F8u;
    if (Is24Bit(psm)) return fbmsk | 0xFF000000u;
    return fbmsk;
}

/* The channels written whole or in part, as GXM's colour mask (A 1, R 2,
   G 4, B 8). */
u8 ColourMask(u32 kept) {
    u8 mask = 0;
    if ((kept & 0xFF) != 0xFF) mask |= SCE_GXM_COLOR_MASK_R;
    if (((kept >> 8) & 0xFF) != 0xFF) mask |= SCE_GXM_COLOR_MASK_G;
    if (((kept >> 16) & 0xFF) != 0xFF) mask |= SCE_GXM_COLOR_MASK_B;
    if (((kept >> 24) & 0xFF) != 0xFF) mask |= SCE_GXM_COLOR_MASK_A;
    return mask;
}

/* Cv = (A - B) * C + D as the blender's src * sf (op) dst * df (the Vulkan
   renderer's Blending): each of Cs and Cd ends with a coefficient 0, 1, C,
   -C, 1 - C or 1 + C (taken as 1). C is the fragment's alpha (the source's,
   or FIX, sent as the factor), or the destination's. Alpha is written as the
   fragment has it. */
SceGxmBlendInfo Blending(const GsDrawState& s, u8 mask) {
    SceGxmBlendInfo b{};
    b.colorMask = mask;
    b.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
    b.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    b.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    b.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
    const bool dest_alpha = s.c == BlendAlpha::Dest;
    const auto kC = dest_alpha ? SCE_GXM_BLEND_FACTOR_DST_ALPHA : SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    const auto kOneMinusC = dest_alpha ? SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    auto coefficient = [&](BlendColour who, bool* negative) {
        const int m = int(s.a == who) - int(s.b == who);
        const bool plus_one = s.d == who;
        *negative = false;
        if (m == 0) return plus_one ? SCE_GXM_BLEND_FACTOR_ONE : SCE_GXM_BLEND_FACTOR_ZERO;
        if (m == 1) return plus_one ? SCE_GXM_BLEND_FACTOR_ONE : kC;
        if (plus_one) return kOneMinusC;
        *negative = true;
        return kC;
    };
    bool src_negative, dst_negative;
    b.colorSrc = coefficient(BlendColour::Source, &src_negative);
    b.colorDst = coefficient(BlendColour::Dest, &dst_negative);
    if (!src_negative && dst_negative) b.colorFunc = SCE_GXM_BLEND_FUNC_SUBTRACT;
    else if (src_negative && !dst_negative) b.colorFunc = SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
    else if (src_negative && dst_negative) b.colorSrc = b.colorDst = SCE_GXM_BLEND_FACTOR_ZERO;
    return b;
}

/* A blend that writes nothing but the source (none, or the mask alone). */
SceGxmBlendInfo Plain(u8 mask) {
    SceGxmBlendInfo b{};
    b.colorMask = mask;
    b.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
    b.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
    b.colorSrc = b.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    b.colorDst = b.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
    return b;
}

u32 BlendKey(const SceGxmBlendInfo& b) {
    u32 key;
    std::memcpy(&key, &b, 4);
    return key;
}

bool SameBatch(const GsDrawState& x, const GsDrawState& y) {
    return x.fbp == y.fbp && x.fbw == y.fbw && x.psm == y.psm && x.fbmsk == y.fbmsk && x.scissor_x0 == y.scissor_x0 &&
           x.scissor_y0 == y.scissor_y0 && x.scissor_x1 == y.scissor_x1 && x.scissor_y1 == y.scissor_y1 &&
           x.textured == y.textured &&
           (!x.textured || (x.texture == y.texture && x.tfx == y.tfx && x.tcc == y.tcc && x.bilinear == y.bilinear &&
                            x.wms == y.wms && x.wmt == y.wmt && x.tw == y.tw && x.th == y.th &&
                            x.texture_target == y.texture_target && x.tex_fbp == y.tex_fbp &&
                            x.tex_fbw == y.tex_fbw && x.tex_target_psm == y.tex_target_psm &&
                            x.tex_format == y.tex_format && x.ta0 == y.ta0 && x.ta1 == y.ta1 && x.aem == y.aem)) &&
           x.alpha_test == y.alpha_test &&
           (!x.alpha_test || (x.atst == y.atst && x.aref == y.aref && x.afail == y.afail)) && x.blend == y.blend &&
           (!x.blend || (x.a == y.a && x.b == y.b && x.c == y.c && x.d == y.d && x.fix == y.fix && x.pabe == y.pabe)) &&
           x.date == y.date && (!x.date || x.datm == y.datm) &&
           x.depth == y.depth && (!x.depth || (x.zbp == y.zbp && x.ztst == y.ztst && x.depth_write == y.depth_write)) &&
           x.fog == y.fog && (!x.fog || x.fogcol == y.fogcol);
}

u32 To16(u32 c) {
    return ((c >> 3) & 31u) | (((c >> 11) & 31u) << 5) | (((c >> 19) & 31u) << 10) | ((c >> 31) << 15);
}

u32 From16(u32 c) {
    return ((c & 31u) << 3) | (((c >> 5) & 31u) << 11) | (((c >> 10) & 31u) << 19) | ((c & 0x8000u) ? 0x80000000u : 0u);
}

constexpr u32 kVertexRing = 4u << 20;           // 131,072 vertices a frame before the GPU is waited for
constexpr u32 kHeapSize = 96u << 20;            // CDRAM: targets, depth buffers, textures
constexpr u32 kMaxSide = 2048;

class GxmGsRenderer final : public GsRenderer {
public:
    bool Init(std::string* error);

    void UploadTexture(u64 key, const u32* pixels, u32 width, u32 height) override;
    void UploadDepth(u32 zbp, u32 width, u32 height, const u32* z) override;
    void UploadTarget(u32 fbp, u32 fbw, u32 psm, const u32* pixels, u32 width, u32 height) override;
    void ReleaseTexture(u64 key) override;
    bool HasTarget(u32 fbp, u32 fbw, u32 psm) const override { return targets_.count(Key(fbp, fbw, psm)) != 0; }
    void DrawTriangles(const GsDrawState& state, const GsVertex* vertices, std::size_t count) override;
    void Flush() override;
    void Kick() override;
    void MemoryWrite(u32 begin, u32 end, const u8* bytes) override;
    void MemoryRead(u32 begin, u32 end, u8* bytes) override;
    void MemoryPatch(const u32* words, std::size_t count) override;
    bool TargetToMemory(u32 fbp, u32 fbw, u32 psm, u32 cols, u32 rows, u32 first_row) override;
    void TargetFromMemory(u32 fbp, u32 fbw, u32 psm, u32 width, u32 height) override;
    void TargetRowsFromMemory(u32 fbp, u32 fbw, u32 psm, u32 width, u32 height, u32 first_row, u32 rows) override;
    bool DepthToMemory(u32 zbp, u32 width, u32 height, u32 fbw, u32 zpsm, u32 cols, u32 rows, u32 first_row) override;
    void DepthFromMemory(u32 zbp, u32 width, u32 height, u32 fbw, u32 zpsm) override;
    void DepthRowsFromMemory(u32 zbp, u32 width, u32 height, u32 fbw, u32 zpsm, u32 first_row, u32 rows) override;
    void TextureFromMemory(u64 key, const GsTextureDecode& decode) override;
    void ClutFromMemory(const GsClutLoad& load) override;
    void ClutRows(const u32* clut, u32 rows) override;
    bool ReadDepth(u32 zbp, u32 width, u32 height, std::vector<u32>* z) override;
    bool ReadTexture(u64 key, std::vector<u32>* pixels, u32* width, u32* height) override;
    bool DisplayTarget(u32 fbp, u32 fbw, u32 psm, u64* image, int* layout, u32* width, u32* height) override;
    bool ReadTarget(u32 fbp, u32 fbw, u32 psm, std::vector<u32>* pixels, u32* width, u32* height) override;

private:
    using TargetKey = std::tuple<u32, u32, u32>;
    using DepthKey = std::tuple<u32, u32, u32>;
    static TargetKey Key(u32 fbp, u32 fbw, u32 psm) { return TargetKey{fbp, fbw, psm}; }

    Target* TargetFor(u32 fbp, u32 fbw, u32 psm, u32 width, u32 height);
    Depth* DepthFor(u32 zbp, u32 width, u32 height);
    Depth* Dummy(u32 width, u32 height);
    SceGxmRenderTarget* RenderTarget(u32 width, u32 height);
    bool MakeTarget(Target& t, u32 width, u32 height);
    bool MakeDepth(Depth& d, u32 width, u32 height, bool kept);
    void OpenScene(Target* target, Depth* depth);
    void CloseScene();
    void Idle();                    // the scene ended and the GPU finished: the CPU may touch what it drew
    void Release(void* pointer) { released_.push_back(pointer); }
    SceGxmFragmentProgram* Fragment(Variant variant, const SceGxmBlendInfo& blend);
    struct Params {
        const SceGxmProgramParameter* params = nullptr;
        const SceGxmProgramParameter* fog = nullptr;
        const SceGxmProgramParameter* tex_scale = nullptr;
    };
    const SceGxmProgram* FragmentGxp(Variant variant, SceGxmShaderPatcherId* id, Params** params);
    /* Stencil in a pass: off, tested (the destination alpha test's: drawn
       where the setup set it), or set to 0 or 1 where drawn. */
    enum class Stencil { kOff, kTest, kClear, kSet };
    void Pass(Variant variant, const SceGxmBlendInfo& blend, const GsDrawState& s, const SceGxmTexture* texture,
              const float tex_scale[2], const void* vertices, u32 count, SceGxmDepthFunc depth_func, bool depth_write,
              Stencil stencil = Stencil::kOff);
    void DateSetup(Target* t, u32 datm);
    u32 TexelAt(const GsTextureDecode& d, int u, int v, int level) const;

    Device* device_ = nullptr;
    Heap heap_;
    Memory vertex_memory_, index_memory_;
    u32 vertex_used_ = 0;
    u16* indices_ = nullptr;
    SceGxmShaderPatcherId vertex_id_ = nullptr;
    const SceGxmProgram* vertex_gxp_ = nullptr;
    SceGxmVertexProgram* vertex_program_ = nullptr;
    std::map<Variant, std::tuple<const SceGxmProgram*, SceGxmShaderPatcherId, Params>> fragment_gxps_;
    std::map<std::pair<Variant, u32>, SceGxmFragmentProgram*> fragments_;

    std::map<TargetKey, Target> targets_;
    std::map<DepthKey, Depth> depths_;
    std::map<std::pair<u32, u32>, Depth> dummies_;
    std::map<std::pair<u32, u32>, SceGxmRenderTarget*> render_targets_;
    std::unordered_map<u64, Texture> textures_;
    std::vector<void*> released_;   // heap pieces the GPU may still read: given back at Idle

    Target* scene_target_ = nullptr;
    Depth* scene_depth_ = nullptr;
    bool scene_open_ = false;

    GsDrawState state_{};
    std::vector<GsVertex> pending_;

    ps2host::gs::Memory memory_;    // GS local memory "on the GPU"
    u32 clut_[256] = {};            // the GPU's CLUT (ClutFromMemory, ClutRows)
    u64 draws_ = 0, scenes_ = 0, waits_ = 0;
};

bool GxmGsRenderer::Init(std::string* error) {
    device_ = &TheDevice();
    if (!device_->Start(error)) return false;
    vertex_gxp_ = device_->Program("gs_vertex", kVertexSource, false, error, &vertex_id_);
    if (!vertex_gxp_) return false;
    const char* names[4] = {"aPosition", "aColor", "aStq", "aFog"};
    const SceGxmAttributeFormat formats[4] = {SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_U8N,
                                              SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_U8N};
    const u16 offsets[4] = {0, 24, 12, 28};
    const u8 components[4] = {3, 4, 3, 4};
    SceGxmVertexAttribute attributes[4]{};
    for (int i = 0; i < 4; ++i) {
        const SceGxmProgramParameter* p = sceGxmProgramFindParameterByName(vertex_gxp_, names[i]);
        if (!p) {
            *error = std::string("the GS vertex program has no ") + names[i];
            return false;
        }
        attributes[i].streamIndex = 0;
        attributes[i].offset = offsets[i];
        attributes[i].format = formats[i];
        attributes[i].componentCount = components[i];
        attributes[i].regIndex = u16(sceGxmProgramParameterGetResourceIndex(p));
    }
    SceGxmVertexStream stream{};
    stream.stride = sizeof(GxmVertex);
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    int r = sceGxmShaderPatcherCreateVertexProgram(device_->patcher(), vertex_id_, attributes, 4, &stream, 1,
                                                   &vertex_program_);
    if (r < 0) {
        *error = "sceGxmShaderPatcherCreateVertexProgram " + Hex(r);
        return false;
    }
    // The first fragment program now: no libshacccg, no renderer (the
    // software GS draws instead).
    Params* unused = nullptr;
    SceGxmShaderPatcherId id = nullptr;
    if (!FragmentGxp(0, &id, &unused)) {
        *error = "the GS fragment program could not be compiled";
        return false;
    }
    if (!heap_.Init(kHeapSize, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, "gxm gs heap")) {
        *error = "no CDRAM for the GS's targets and textures";
        return false;
    }
    if (!Allocate(&vertex_memory_, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, kVertexRing, SCE_GXM_MEMORY_ATTRIB_READ,
                  "gxm gs vertices") ||
        !Allocate(&index_memory_, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 65536 * 2, SCE_GXM_MEMORY_ATTRIB_READ,
                  "gxm gs indices")) {
        *error = "no memory for the GS's vertices";
        return false;
    }
    // Triangles as they come: index i is vertex i.
    indices_ = static_cast<u16*>(index_memory_.base);
    for (u32 i = 0; i < 65536; ++i) indices_[i] = u16(i);
    // The display's scene comes after what is drawn here.
    device_->SetSceneCloser([this] {
        Flush();
        CloseScene();
    });
    std::fprintf(stderr, "[gxm] GS renderer: %u MB of CDRAM for targets and textures\n", unsigned(kHeapSize >> 20));
    return true;
}

// ---------------------------------------------------------------- programs

const SceGxmProgram* GxmGsRenderer::FragmentGxp(Variant variant, SceGxmShaderPatcherId* id, Params** params) {
    auto found = fragment_gxps_.find(variant);
    if (found == fragment_gxps_.end()) {
        std::string error;
        char name[32];
        std::snprintf(name, sizeof name, "gs_fragment_%04x", unsigned(variant));
        SceGxmShaderPatcherId got = nullptr;
        const std::string source = FragmentSource(variant);
        const SceGxmProgram* gxp = device_->Program(name, source.c_str(), true, &error, &got);
        if (!gxp) {
            std::fprintf(stderr, "[gxm] %s\n", error.c_str());
            return nullptr;
        }
        Params p;
        p.params = sceGxmProgramFindParameterByName(gxp, "uParams");
        p.fog = sceGxmProgramFindParameterByName(gxp, "uFog");
        p.tex_scale = sceGxmProgramFindParameterByName(gxp, "uTexScale");
        found = fragment_gxps_.emplace(variant, std::make_tuple(gxp, got, p)).first;
    }
    *id = std::get<1>(found->second);
    *params = &std::get<2>(found->second);
    return std::get<0>(found->second);
}

SceGxmFragmentProgram* GxmGsRenderer::Fragment(Variant variant, const SceGxmBlendInfo& blend) {
    const auto key = std::make_pair(variant, BlendKey(blend));
    auto found = fragments_.find(key);
    if (found != fragments_.end()) return found->second;
    SceGxmShaderPatcherId id = nullptr;
    Params* params = nullptr;
    if (!FragmentGxp(variant, &id, &params)) return fragments_[key] = nullptr;
    SceGxmFragmentProgram* program = nullptr;
    const int r = sceGxmShaderPatcherCreateFragmentProgram(device_->patcher(), id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                           SCE_GXM_MULTISAMPLE_NONE, &blend, vertex_gxp_, &program);
    if (r < 0) {
        std::fprintf(stderr, "[gxm] sceGxmShaderPatcherCreateFragmentProgram %s\n", Hex(r).c_str());
        program = nullptr;
    }
    return fragments_[key] = program;
}

// ---------------------------------------------------------------- surfaces

SceGxmRenderTarget* GxmGsRenderer::RenderTarget(u32 width, u32 height) {
    const auto key = std::make_pair(width, height);
    auto found = render_targets_.find(key);
    if (found != render_targets_.end()) return found->second;
    SceGxmRenderTargetParams rt{};
    rt.flags = 0;
    rt.width = u16(width);
    rt.height = u16(height);
    rt.scenesPerFrame = 1;
    rt.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rt.multisampleLocations = 0;
    rt.driverMemBlock = -1;
    SceGxmRenderTarget* target = nullptr;
    const int r = sceGxmCreateRenderTarget(&rt, &target);
    if (r < 0) {
        std::fprintf(stderr, "[gxm] sceGxmCreateRenderTarget %ux%u: %s\n", unsigned(width), unsigned(height),
                     Hex(r).c_str());
        target = nullptr;
    }
    return render_targets_[key] = target;
}

bool GxmGsRenderer::MakeTarget(Target& t, u32 width, u32 height) {
    t.width = std::min(std::max(width, 1u), kMaxSide);
    t.height = std::min(std::max(height, 1u), kMaxSide);
    t.stride = (t.width + 7) & ~7u;
    t.pixels = static_cast<u32*>(heap_.Take(t.stride * t.height * 4));
    if (!t.pixels) {
        std::fprintf(stderr, "[gxm] no CDRAM for a %ux%u target (%u of %u MB in use)\n", unsigned(t.width),
                     unsigned(t.height), unsigned(heap_.in_use() >> 20), unsigned(heap_.size() >> 20));
        return false;
    }
    std::memset(t.pixels, 0, std::size_t(t.stride) * t.height * 4);     // local memory starts as zeros
    sceGxmColorSurfaceInit(&t.surface, SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
                           SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, t.width, t.height,
                           t.stride, t.pixels);
    sceGxmTextureInitLinear(&t.texture, t.pixels, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, t.width, t.height, 0);
    return true;
}

Target* GxmGsRenderer::TargetFor(u32 fbp, u32 fbw, u32 psm, u32 width, u32 height) {
    width = std::min(std::max(width, 1u), kMaxSide);
    height = std::min(std::max(height, 1u), kMaxSide);
    auto found = targets_.find(Key(fbp, fbw, psm));
    if (found != targets_.end()) {
        Target& t = found->second;
        if (width <= t.width && height <= t.height) return &t;
        // Larger than it was made: a new one, the old pixels kept.
        Idle();
        Target bigger;
        bigger.fbp = fbp, bigger.fbw = fbw, bigger.psm = psm;
        if (!MakeTarget(bigger, std::max(width, t.width), std::max(height, t.height))) return &t;
        for (u32 y = 0; y < t.height; ++y)
            std::memcpy(bigger.pixels + std::size_t(y) * bigger.stride, t.pixels + std::size_t(y) * t.stride, t.width * 4);
        heap_.Give(t.pixels);
        t = bigger;
        return &t;
    }
    Target t;
    t.fbp = fbp, t.fbw = fbw, t.psm = psm;
    if (!MakeTarget(t, width, height)) return nullptr;
    return &(targets_[Key(fbp, fbw, psm)] = t);
}

bool GxmGsRenderer::MakeDepth(Depth& d, u32 width, u32 height, bool kept) {
    d.width = width;
    d.height = height;
    d.stride = (width + SCE_GXM_TILE_SIZEX - 1) & ~(SCE_GXM_TILE_SIZEX - 1);
    const u32 rows = (height + SCE_GXM_TILE_SIZEY - 1) & ~(SCE_GXM_TILE_SIZEY - 1);
    d.depth = static_cast<float*>(heap_.Take(d.stride * rows * 4));
    d.stencil = static_cast<u8*>(heap_.Take(d.stride * rows));
    if (!d.depth || !d.stencil) {
        std::fprintf(stderr, "[gxm] no CDRAM for a %ux%u depth buffer\n", unsigned(width), unsigned(height));
        heap_.Give(d.depth);
        heap_.Give(d.stencil);
        d.depth = nullptr;
        d.stencil = nullptr;
        return false;
    }
    std::memset(d.depth, 0, std::size_t(d.stride) * rows * 4);         // zeros: the farthest Z
    std::memset(d.stencil, 0, std::size_t(d.stride) * rows);
    sceGxmDepthStencilSurfaceInit(&d.surface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32_S8, SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR,
                                  d.stride, d.depth, d.stencil);
    if (kept) {
        sceGxmDepthStencilSurfaceSetForceLoadMode(&d.surface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
        sceGxmDepthStencilSurfaceSetForceStoreMode(&d.surface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
    }
    return true;
}

Depth* GxmGsRenderer::DepthFor(u32 zbp, u32 width, u32 height) {
    width = std::min(std::max(width, 1u), kMaxSide);
    height = std::min(std::max(height, 1u), kMaxSide);
    auto found = depths_.find(DepthKey{zbp, width, height});
    if (found != depths_.end()) return &found->second;
    Depth d;
    if (!MakeDepth(d, width, height, true)) return nullptr;
    return &(depths_[DepthKey{zbp, width, height}] = d);
}

Depth* GxmGsRenderer::Dummy(u32 width, u32 height) {
    // A scene without the GS's depth still needs a depth surface (every draw
    // fails without one): its contents never loaded or kept.
    auto found = dummies_.find({width, height});
    if (found != dummies_.end()) return &found->second;
    Depth d;
    if (!MakeDepth(d, width, height, false)) return nullptr;
    return &(dummies_[{width, height}] = d);
}

void GxmGsRenderer::OpenScene(Target* target, Depth* depth) {
    if (scene_open_ && scene_target_ == target && (depth == nullptr || depth == scene_depth_)) return;
    CloseScene();
    if (!depth) depth = Dummy(target->width, target->height);
    SceGxmRenderTarget* rt = RenderTarget(target->width, target->height);
    if (!depth || !rt) return;
    const int r = sceGxmBeginScene(device_->context(), 0, rt, nullptr, nullptr, nullptr, &target->surface,
                                   &depth->surface);
    if (r < 0) {
        std::fprintf(stderr, "[gxm] sceGxmBeginScene %s\n", Hex(r).c_str());
        return;
    }
    scene_open_ = true;
    scene_target_ = target;
    scene_depth_ = depth;
    ++scenes_;
    ++g_scenes;
    // Clip space onto the target's pixels, rows down; Z as given.
    sceGxmSetViewportEnable(device_->context(), SCE_GXM_VIEWPORT_ENABLED);
    sceGxmSetViewport(device_->context(), target->width / 2.0f, target->width / 2.0f, target->height / 2.0f,
                      target->height / 2.0f, 0.0f, 1.0f);
    sceGxmSetCullMode(device_->context(), SCE_GXM_CULL_NONE);
}

void GxmGsRenderer::CloseScene() {
    if (!scene_open_) return;
    sceGxmEndScene(device_->context(), nullptr, nullptr);
    scene_open_ = false;
    scene_target_ = nullptr;
    scene_depth_ = nullptr;
}

void GxmGsRenderer::Idle() {
    Flush();
    CloseScene();
    const u64 from = WaitClock();
    sceGxmFinish(device_->context());
    if (g_wait_observer) g_wait_observer("the GPU", from, WaitClock());
    ++waits_;
    ++g_waits;
    for (void* p : released_) heap_.Give(p);
    released_.clear();
    vertex_used_ = 0;
}

// ---------------------------------------------------------------- drawing

void GxmGsRenderer::DrawTriangles(const GsDrawState& state, const GsVertex* v, std::size_t count) {
    if (!SameBatch(state, state_)) Flush();
    // A primitive sampling the target it draws into: what is pending first
    // (it may be what it reads).
    if (state.textured && state.texture_target && state.tex_fbp == state.fbp && state.tex_fbw == state.fbw) Flush();
    state_ = state;
    for (std::size_t i = 0; i + 2 < count; i += 3)
        for (int k = 0; k < 3; ++k) {
            // Flat shading takes the colour of the triangle's last vertex.
            GsVertex p = v[i + k];
            if (!state.gouraud) {
                const GsVertex& c = v[i + 2];
                p.r = c.r, p.g = c.g, p.b = c.b, p.a = c.a;
            }
            pending_.push_back(p);
        }
    if (pending_.size() >= 60000) Flush();
}

void GxmGsRenderer::Pass(Variant variant, const SceGxmBlendInfo& blend, const GsDrawState& s,
                         const SceGxmTexture* texture, const float tex_scale[2], const void* vertices, u32 count,
                         SceGxmDepthFunc depth_func, bool depth_write, Stencil stencil) {
    SceGxmFragmentProgram* program = Fragment(variant, blend);
    if (!program) return;
    SceGxmShaderPatcherId id = nullptr;
    Params* params = nullptr;
    FragmentGxp(variant, &id, &params);
    SceGxmContext* ctx = device_->context();
    sceGxmSetVertexProgram(ctx, vertex_program_);
    sceGxmSetFragmentProgram(ctx, program);
    sceGxmSetFrontDepthFunc(ctx, depth_func);
    sceGxmSetFrontDepthWriteEnable(ctx, depth_write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
    switch (stencil) {
        case Stencil::kOff:
            sceGxmSetFrontStencilFunc(ctx, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                                      SCE_GXM_STENCIL_OP_KEEP, 0xFF, 0xFF);
            break;
        case Stencil::kTest:
            sceGxmSetFrontStencilFunc(ctx, SCE_GXM_STENCIL_FUNC_EQUAL, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                                      SCE_GXM_STENCIL_OP_KEEP, 0xFF, 0x00);
            sceGxmSetFrontStencilRef(ctx, 1);
            break;
        case Stencil::kClear:
        case Stencil::kSet:
            sceGxmSetFrontStencilFunc(ctx, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_REPLACE,
                                      SCE_GXM_STENCIL_OP_REPLACE, SCE_GXM_STENCIL_OP_REPLACE, 0xFF, 0xFF);
            sceGxmSetFrontStencilRef(ctx, stencil == Stencil::kSet ? 1 : 0);
            break;
    }
    if (params->params || params->fog || params->tex_scale) {
        void* uniforms = nullptr;
        sceGxmReserveFragmentDefaultUniformBuffer(ctx, &uniforms);
        const float fix = std::min(1.0f, float(s.fix) / 128.0f);
        const float p[4] = {float(s.aref) / 255.0f, fix, float(s.ta0) / 255.0f, float(s.ta1) / 255.0f};
        const float fog[4] = {float(s.fogcol & 0xFF) / 255.0f, float((s.fogcol >> 8) & 0xFF) / 255.0f,
                              float((s.fogcol >> 16) & 0xFF) / 255.0f, s.aem ? 1.0f : 0.0f};
        if (params->params) sceGxmSetUniformDataF(uniforms, params->params, 0, 4, p);
        if (params->fog) sceGxmSetUniformDataF(uniforms, params->fog, 0, 4, fog);
        if (params->tex_scale) sceGxmSetUniformDataF(uniforms, params->tex_scale, 0, 2, tex_scale);
    }
    if (texture) sceGxmSetFragmentTexture(ctx, 0, texture);
    sceGxmSetVertexStream(ctx, 0, vertices);
    const int r = sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices_, count);
    if (r < 0) {
        static int reported = 0;
        if (reported++ < 8) std::fprintf(stderr, "[gxm] sceGxmDraw %s\n", Hex(r).c_str());
    }
    ++draws_;
    ++g_draws;
}

void GxmGsRenderer::Flush() {
    if (pending_.empty()) return;
    const GsDrawState& s = state_;
    std::vector<GsVertex> batch;
    batch.swap(pending_);
    // ZTST NEVER: nothing is drawn.
    if (s.depth && s.ztst == 0) return;
    Target* t = TargetFor(s.fbp, s.fbw, s.psm, s.width, s.height);
    if (!t) return;
    Depth* d = s.depth ? DepthFor(s.zbp, t->width, t->height) : nullptr;
    // The texture: a decoded one, or a target sampled as it is.
    const SceGxmTexture* texture = nullptr;
    SceGxmTexture sampled{};
    float tex_scale[2] = {1.0f, 1.0f};
    u32 tex_format = 0;
    if (s.textured) {
        if (s.texture_target) {
            auto found = targets_.find(Key(s.tex_fbp, s.tex_fbw, s.tex_target_psm));
            if (found != targets_.end()) {
                sampled = found->second.texture;
                // S and T are over TEX0's size; the target is its own size.
                tex_scale[0] = float(s.tw ? s.tw : found->second.width) / float(found->second.width);
                tex_scale[1] = float(s.th ? s.th : found->second.height) / float(found->second.height);
                tex_format = s.tex_format == 1 ? 1 : s.tex_format == 2 ? 2 : 0;
                texture = &sampled;
            }
        } else {
            auto found = textures_.find(s.texture);
            if (found != textures_.end()) {
                sampled = found->second.texture;
                texture = &sampled;
            }
        }
        if (texture) {
            const auto filter = s.bilinear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
            sceGxmTextureSetMinFilter(&sampled, filter);
            sceGxmTextureSetMagFilter(&sampled, filter);
            // REPEAT and CLAMP as the sampler has them; the region modes are
            // in a decoded texture already (clamped beyond).
            sceGxmTextureSetUAddrMode(&sampled, s.wms == 0 && !s.texture_target ? SCE_GXM_TEXTURE_ADDR_REPEAT
                                                                                 : SCE_GXM_TEXTURE_ADDR_CLAMP);
            sceGxmTextureSetVAddrMode(&sampled, s.wmt == 0 && !s.texture_target ? SCE_GXM_TEXTURE_ADDR_REPEAT
                                                                                 : SCE_GXM_TEXTURE_ADDR_CLAMP);
        }
    }
    // The vertices, in clip space: the GS samples a pixel at its whole
    // coordinates, the GPU at its centre - half a pixel on.
    const u32 count = u32(batch.size());
    if (vertex_used_ + count * sizeof(GxmVertex) > kVertexRing) Idle();
    GxmVertex* out = reinterpret_cast<GxmVertex*>(static_cast<u8*>(vertex_memory_.base) + vertex_used_);
    vertex_used_ += count * sizeof(GxmVertex);
    const float sx = 2.0f / float(t->width), sy = 2.0f / float(t->height);
    for (u32 i = 0; i < count; ++i) {
        const GsVertex& p = batch[i];
        GxmVertex& o = out[i];
        o.x = (p.x + 0.5f) * sx - 1.0f;
        o.y = (p.y + 0.5f) * sy - 1.0f;
        o.z = float(double(p.z) / 4294967296.0);
        o.s = p.s, o.t = p.t, o.q = p.q != 0.0f ? p.q : 1.0f;
        o.r = p.r, o.g = p.g, o.b = p.b, o.a = p.a;
        o.fog = p.fog;
        o.pad[0] = o.pad[1] = o.pad[2] = 0;
    }
    OpenScene(t, d);
    if (!scene_open_) return;
    // The destination alpha test: the target's alpha bit into the stencil
    // first, from a new scene (what it samples is the target's memory as
    // the scene began: everything drawn before it).
    if (s.date) {
        CloseScene();
        OpenScene(t, d);
        if (!scene_open_) return;
        DateSetup(t, s.datm);
    }
    const Stencil stencil = s.date ? Stencil::kTest : Stencil::kOff;
    // The scissor, inclusive, within the target.
    const int x0 = std::max(0, s.scissor_x0), y0 = std::max(0, s.scissor_y0);
    const int x1 = std::min(int(t->width) - 1, s.scissor_x1), y1 = std::min(int(t->height) - 1, s.scissor_y1);
    if (x1 < x0 || y1 < y0) return;
    sceGxmSetRegionClip(device_->context(), SCE_GXM_REGION_CLIP_OUTSIDE, u32(x0), u32(y0), u32(x1), u32(y1));
    // Depth: the GS keeps the larger Z.
    SceGxmDepthFunc depth_func = SCE_GXM_DEPTH_FUNC_ALWAYS;
    bool depth_write = false;
    if (s.depth) {
        depth_func = s.ztst == 2 ? SCE_GXM_DEPTH_FUNC_GREATER_EQUAL
                   : s.ztst == 3 ? SCE_GXM_DEPTH_FUNC_GREATER
                                 : SCE_GXM_DEPTH_FUNC_ALWAYS;
        depth_write = s.depth_write;
    }

    Variant base = 0;
    if (texture) {
        base |= kTextured | (u32(s.tfx) << kTfxShift) | (s.tcc ? kTcc : 0u) | (tex_format << kTexFormatShift);
    }
    if (s.fog) base |= kFog;
    const u8 mask = ColourMask(KeptBits(s.fbmsk, s.psm));
    // The blend's factor from the fragment's alpha: where alpha is written,
    // a second pass writes it.
    const bool factor_out = s.blend && s.c != BlendAlpha::Dest;
    const u32 alpha_out = !factor_out ? 0u : s.c == BlendAlpha::Fix ? 2u : 1u;
    auto blended = [&](Variant v, u8 colour_mask, bool test_depth_write) {
        const u8 colour = colour_mask & u8(SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B);
        if (factor_out && (colour_mask & SCE_GXM_COLOR_MASK_A)) {
            if (colour)
                Pass(v | (alpha_out << kAlphaOutShift), Blending(s, colour), s, texture, tex_scale, out, count,
                     depth_func, false, stencil);
            Pass(v, Plain(SCE_GXM_COLOR_MASK_A), s, texture, tex_scale, out, count, depth_func, test_depth_write,
                 stencil);
        } else {
            Pass(v | (alpha_out << kAlphaOutShift), Blending(s, colour_mask), s, texture, tex_scale, out, count,
                 depth_func, test_depth_write, stencil);
        }
    };
    auto draw = [&](Variant v, u8 colour_mask, bool test_depth_write) {
        if (!colour_mask && !test_depth_write) return;
        if (s.blend && colour_mask) {
            if (!s.pabe) return blended(v, colour_mask, test_depth_write);
            // PABE: blended where As >= 0x80, written as it is below.
            blended(v | (1u << kPabeShift), colour_mask, test_depth_write);
            Pass(v | (2u << kPabeShift), Plain(colour_mask), s, texture, tex_scale, out, count, depth_func,
                 test_depth_write, stencil);
            return;
        }
        Pass(v, Plain(colour_mask), s, texture, tex_scale, out, count, depth_func, test_depth_write, stencil);
    };
    if (s.alpha_test && s.atst != 1) {
        // The passing pixels with everything; the failing ones with what
        // AFAIL keeps: FB_ONLY colour, ZB_ONLY depth, RGB_ONLY colour without
        // alpha (KEEP nothing).
        const Variant tested = base | ((s.atst + 1) << kAtstShift);
        if (s.atst != 0) draw(tested, mask, depth_write);
        switch (s.afail) {
            case 1: draw(tested | kFailing, mask, false); break;
            case 2: draw(tested | kFailing, 0, depth_write); break;
            case 3: draw(tested | kFailing, mask & u8(~SCE_GXM_COLOR_MASK_A), false); break;
            default: break;
        }
    } else {
        draw(base, mask, depth_write);
    }
}

void GxmGsRenderer::DateSetup(Target* t, u32 datm) {
    // Over the whole target: the stencil cleared, then set where the alpha
    // bit is DATM. Clip space corners, texture coordinates over the target:
    // each pixel reads its own texel.
    if (vertex_used_ + 6 * sizeof(GxmVertex) > kVertexRing) return;
    GxmVertex* v = reinterpret_cast<GxmVertex*>(static_cast<u8*>(vertex_memory_.base) + vertex_used_);
    vertex_used_ += 6 * sizeof(GxmVertex);
    const float corners[6][4] = {{-1, -1, 0, 0}, {1, -1, 1, 0}, {-1, 1, 0, 1}, {1, -1, 1, 0}, {1, 1, 1, 1}, {-1, 1, 0, 1}};
    for (int i = 0; i < 6; ++i) v[i] = GxmVertex{corners[i][0], corners[i][1], 0.0f, corners[i][2], corners[i][3], 1.0f,
                                                 0, 0, 0, 0, 0xFF, {0, 0, 0}};
    sceGxmSetRegionClip(device_->context(), SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0, t->width - 1, t->height - 1);
    SceGxmTexture own = t->texture;
    sceGxmTextureSetMinFilter(&own, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&own, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(&own, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&own, SCE_GXM_TEXTURE_ADDR_CLAMP);
    const float one[2] = {1.0f, 1.0f};
    GsDrawState none;
    Pass(0, Plain(0), none, nullptr, one, v, 6, SCE_GXM_DEPTH_FUNC_ALWAYS, false, Stencil::kClear);
    Pass(kDateSetup | (datm & 1u), Plain(0), none, &own, one, v, 6, SCE_GXM_DEPTH_FUNC_ALWAYS, false, Stencil::kSet);
}

void GxmGsRenderer::Kick() {
    // The frame's end: what is recorded goes to the GPU, and is finished
    // before the next (the CPU's copy of what it drew stays simple).
    Idle();
}

// ---------------------------------------------------------------- uploads

void GxmGsRenderer::UploadTexture(u64 key, const u32* pixels, u32 width, u32 height) {
    if (!width || !height) return;
    Flush();
    Texture& t = textures_[key];
    // New memory each time: what is recorded may still read the old.
    if (t.pixels) Release(t.pixels);
    t.width = std::min(width, kMaxSide);
    t.height = std::min(height, kMaxSide);
    t.stride = (t.width + 7) & ~7u;
    t.pixels = static_cast<u32*>(heap_.Take(t.stride * t.height * 4));
    if (!t.pixels) {
        Idle();         // what was released comes back
        t.pixels = static_cast<u32*>(heap_.Take(t.stride * t.height * 4));
        if (!t.pixels) {
            std::fprintf(stderr, "[gxm] no CDRAM for a %ux%u texture\n", unsigned(width), unsigned(height));
            textures_.erase(key);
            return;
        }
    }
    for (u32 y = 0; y < t.height; ++y)
        std::memcpy(t.pixels + std::size_t(y) * t.stride, pixels + std::size_t(y) * width, t.width * 4);
    sceGxmTextureInitLinear(&t.texture, t.pixels, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, t.width, t.height, 0);
}

void GxmGsRenderer::ReleaseTexture(u64 key) {
    auto found = textures_.find(key);
    if (found == textures_.end()) return;
    Flush();            // what is pending may sample it
    Release(found->second.pixels);
    textures_.erase(found);
}

bool GxmGsRenderer::ReadTexture(u64 key, std::vector<u32>* pixels, u32* width, u32* height) {
    auto found = textures_.find(key);
    if (found == textures_.end()) return false;
    const Texture& t = found->second;
    pixels->resize(std::size_t(t.width) * t.height);
    for (u32 y = 0; y < t.height; ++y)
        std::memcpy(pixels->data() + std::size_t(y) * t.width, t.pixels + std::size_t(y) * t.stride, t.width * 4);
    *width = t.width;
    *height = t.height;
    return true;
}

void GxmGsRenderer::UploadTarget(u32 fbp, u32 fbw, u32 psm, const u32* pixels, u32 width, u32 height) {
    Idle();
    Target* t = TargetFor(fbp, fbw, psm, width, height);
    if (!t) return;
    const u32 w = std::min(width, t->width), h = std::min(height, t->height);
    for (u32 y = 0; y < h; ++y) std::memcpy(t->pixels + std::size_t(y) * t->stride, pixels + std::size_t(y) * width, w * 4);
}

void GxmGsRenderer::UploadDepth(u32 zbp, u32 width, u32 height, const u32* z) {
    Idle();
    Depth* d = DepthFor(zbp, width, height);
    if (!d) return;
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x)
            d->depth[std::size_t(y) * d->stride + x] = float(double(z[std::size_t(y) * width + x]) / 4294967296.0);
}

bool GxmGsRenderer::ReadTarget(u32 fbp, u32 fbw, u32 psm, std::vector<u32>* pixels, u32* width, u32* height) {
    auto found = targets_.find(Key(fbp, fbw, psm));
    if (found == targets_.end()) return false;
    Idle();
    const Target& t = found->second;
    pixels->resize(std::size_t(t.width) * t.height);
    for (u32 y = 0; y < t.height; ++y)
        std::memcpy(pixels->data() + std::size_t(y) * t.width, t.pixels + std::size_t(y) * t.stride, t.width * 4);
    *width = t.width;
    *height = t.height;
    return true;
}

bool GxmGsRenderer::ReadDepth(u32 zbp, u32 width, u32 height, std::vector<u32>* z) {
    auto found = depths_.find(DepthKey{zbp, width, height});
    if (found == depths_.end()) return false;
    Idle();
    const Depth& d = found->second;
    z->resize(std::size_t(width) * height);
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x) {
            const double value = double(d.depth[std::size_t(y) * d.stride + x]) * 4294967296.0;
            (*z)[std::size_t(y) * width + x] = u32(std::min(std::max(value, 0.0), 4294967040.0));
        }
    return true;
}

bool GxmGsRenderer::DisplayTarget(u32 fbp, u32 fbw, u32 psm, u64* image, int* layout, u32* width, u32* height) {
    auto found = targets_.find(Key(fbp, fbw, psm));
    if (found == targets_.end()) return false;
    // What is recorded goes first: the display's scene comes after it, and
    // the GPU takes scenes in order.
    Flush();
    CloseScene();
    *image = u64(reinterpret_cast<std::uintptr_t>(&found->second.texture));
    *layout = 0;
    *width = found->second.width;
    *height = found->second.height;
    return true;
}

// ---------------------------------------------------------------- local memory

void GxmGsRenderer::MemoryWrite(u32 begin, u32 end, const u8* bytes) {
    if (end <= begin) return;
    std::memcpy(memory_.data() + std::size_t(begin) * 256, bytes, std::size_t(end - begin) * 256);
}

void GxmGsRenderer::MemoryRead(u32 begin, u32 end, u8* bytes) {
    if (end <= begin) return;
    std::memcpy(bytes, memory_.data() + std::size_t(begin) * 256, std::size_t(end - begin) * 256);
}

void GxmGsRenderer::MemoryPatch(const u32* words, std::size_t count) {
    u32* mem = reinterpret_cast<u32*>(memory_.data());
    for (std::size_t i = 0; i < count; ++i) {
        const u32 index = words[3 * i] & ((1u << 20) - 1), value = words[3 * i + 1], mask = words[3 * i + 2];
        mem[index] = (mem[index] & ~mask) | (value & mask);
    }
}

bool GxmGsRenderer::TargetToMemory(u32 fbp, u32 fbw, u32 psm, u32 cols, u32 rows, u32 first_row) {
    auto found = targets_.find(Key(fbp, fbw, psm));
    if (found == targets_.end()) return false;
    Idle();
    const Target& t = found->second;
    cols = std::min(cols, t.width);
    rows = std::min(rows, t.height);
    std::vector<u32> run(cols);
    const bool sixteen = Is16Bit(psm), twenty_four = Is24Bit(psm);
    for (u32 y = first_row; y < rows; ++y) {
        const u32* in = t.pixels + std::size_t(y) * t.stride;
        if (twenty_four) {
            // The top byte is not the pixel's: kept as memory has it.
            for (u32 x = 0; x < cols; ++x) {
                const u32 old = memory_.Read(fbp * 32, fbw, x, y, 0x00);
                memory_.Write(fbp * 32, fbw, x, y, 0x00, (old & 0xFF000000u) | (in[x] & 0xFFFFFFu));
            }
            continue;
        }
        for (u32 x = 0; x < cols; ++x) run[x] = sixteen ? To16(in[x]) : in[x];
        memory_.WriteRun(fbp * 32, fbw, 0, y, psm, run.data(), cols);
    }
    return true;
}

void GxmGsRenderer::TargetRowsFromMemory(u32 fbp, u32 fbw, u32 psm, u32 width, u32 height, u32 first_row, u32 rows) {
    Idle();
    Target* t = TargetFor(fbp, fbw, psm, width, height);
    if (!t) return;
    const bool sixteen = Is16Bit(psm), twenty_four = Is24Bit(psm);
    const u32 last = std::min(t->height, first_row + rows);
    for (u32 y = first_row; y < last; ++y) {
        u32* out = t->pixels + std::size_t(y) * t->stride;
        for (u32 x = 0; x < t->width; ++x) {
            const u32 w = memory_.Read(fbp * 32, fbw, x, y, psm);
            out[x] = sixteen ? From16(w) : twenty_four ? (w & 0xFFFFFFu) : w;
        }
    }
}

void GxmGsRenderer::TargetFromMemory(u32 fbp, u32 fbw, u32 psm, u32 width, u32 height) {
    TargetRowsFromMemory(fbp, fbw, psm, width, height, 0, kMaxSide);
}

bool GxmGsRenderer::DepthToMemory(u32 zbp, u32 width, u32 height, u32 fbw, u32 zpsm, u32 cols, u32 rows,
                                  u32 first_row) {
    auto found = depths_.find(DepthKey{zbp, width, height});
    if (found == depths_.end()) return false;
    Idle();
    const Depth& d = found->second;
    cols = std::min(cols, d.width);
    rows = std::min(rows, d.height);
    const bool sixteen = Is16Bit(zpsm), twenty_four = Is24Bit(zpsm);
    std::vector<u32> run(cols);
    for (u32 y = first_row; y < rows; ++y) {
        const float* in = d.depth + std::size_t(y) * d.stride;
        for (u32 x = 0; x < cols; ++x) {
            const double value = double(in[x]) * 4294967296.0;
            const u32 z = u32(std::min(std::max(value, 0.0), 4294967040.0));
            run[x] = sixteen ? (z & 0xFFFFu) : z;
        }
        if (twenty_four) {
            for (u32 x = 0; x < cols; ++x) {
                const u32 old = memory_.Read(zbp * 32, fbw, x, y, 0x30);
                memory_.Write(zbp * 32, fbw, x, y, 0x30, (old & 0xFF000000u) | (run[x] & 0xFFFFFFu));
            }
            continue;
        }
        memory_.WriteRun(zbp * 32, fbw, 0, y, zpsm, run.data(), cols);
    }
    return true;
}

void GxmGsRenderer::DepthRowsFromMemory(u32 zbp, u32 width, u32 height, u32 fbw, u32 zpsm, u32 first_row, u32 rows) {
    Idle();
    Depth* d = DepthFor(zbp, width, height);
    if (!d) return;
    const bool twenty_four = Is24Bit(zpsm);
    const u32 last = std::min(d->height, first_row + rows);
    for (u32 y = first_row; y < last; ++y) {
        float* out = d->depth + std::size_t(y) * d->stride;
        for (u32 x = 0; x < d->width; ++x) {
            u32 z = memory_.Read(zbp * 32, fbw, x, y, zpsm);
            if (twenty_four) z &= 0xFFFFFFu;
            out[x] = float(double(z) / 4294967296.0);
        }
    }
}

void GxmGsRenderer::DepthFromMemory(u32 zbp, u32 width, u32 height, u32 fbw, u32 zpsm) {
    DepthRowsFromMemory(zbp, width, height, fbw, zpsm, 0, kMaxSide);
}

// ---------------------------------------------------------------- textures and CLUTs from memory

int Wrap(int i, u32 size, u32 mode, u32 lo, u32 hi) {
    if (mode == 0) return int(u32(i) & (size - 1));
    if (mode == 1) return std::clamp(i, 0, int(size) - 1);
    if (mode == 2) return std::clamp(i, int(lo), int(hi));
    return int((u32(i) & lo) | hi);
}

/* A texel as the host's software GS reads one (TexelAt): wrapped by CLAMP's
   mode, read in its format, expanded by TEXA or the CLUT. */
u32 GxmGsRenderer::TexelAt(const GsTextureDecode& d, int iu, int iv, int n) const {
    using namespace ps2host::gs;
    u32 raw;
    if (n == 0) {
        const int u = Wrap(iu, d.tw, d.wms, d.minu, d.maxu), v = Wrap(iv, d.th, d.wmt, d.minv, d.maxv);
        raw = memory_.Read(d.tbp, d.tbw, u32(u) & 2047, u32(v) & 2047, d.psm);
    } else {
        const u32 w = std::max(1u, d.tw >> n), h = std::max(1u, d.th >> n);
        const int u = std::clamp(Wrap(iu, w, d.wms, d.minu >> n, d.maxu >> n), 0, int(w) - 1);
        const int v = std::clamp(Wrap(iv, h, d.wmt, d.minv >> n, d.maxv >> n), 0, int(h) - 1);
        raw = memory_.Read(d.level_tbp[n], std::max(1u, d.level_tbw[n]), u32(u), u32(v), d.psm);
    }
    const u32 ta0 = d.texa_lo & 0xFF;
    const bool aem = (d.texa_lo & 0x8000u) != 0;
    const u32* clut = d.clut ? d.clut : clut_;
    switch (d.psm) {
        case PSMCT32: case PSMZ32: return raw;
        case PSMCT24: case PSMZ24: {
            const u32 rgb = raw & 0xFFFFFF;
            return rgb | ((aem && !rgb) ? 0u : ta0) << 24;
        }
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: {
            const u32 a = (raw & 0x8000u) ? d.ta1 : ((aem && !(raw & 0x7FFFu)) ? 0u : ta0);
            return ((raw & 31u) << 3) | (((raw >> 5) & 31u) << 11) | (((raw >> 10) & 31u) << 19) | (a << 24);
        }
        case PSMT8: case PSMT8H: return clut[raw & 0xFF];
        case PSMT4: case PSMT4HL: case PSMT4HH: return clut[(d.csa * 16 + (raw & 15)) & 0xFF];
        default: return raw;
    }
}

void GxmGsRenderer::TextureFromMemory(u64 key, const GsTextureDecode& d) {
    // Mipmap levels below level 0 at the image's left edge, as the GPU's
    // decode lays them out (TW wide, as tall as all of them).
    const u32 levels = std::max(1u, std::min(d.levels, 7u));
    u32 height = 0;
    for (u32 n = 0; n < levels; ++n) height += std::max(1u, d.th >> n);
    std::vector<u32> pixels(std::size_t(d.tw) * height, 0);
    u32 row = 0;
    for (u32 n = 0; n < levels; ++n) {
        const u32 w = std::max(1u, d.tw >> n), h = std::max(1u, d.th >> n);
        for (u32 v = 0; v < h; ++v)
            for (u32 u = 0; u < w; ++u) pixels[std::size_t(row + v) * d.tw + u] = TexelAt(d, int(u), int(v), int(n));
        row += h;
    }
    UploadTexture(key, pixels.data(), d.tw, height);
}

void GxmGsRenderer::ClutFromMemory(const GsClutLoad& load) {
    using namespace ps2host::gs;
    // The CLUT is the CPU's here: decodes read it as they happen, so what is
    // recorded has its textures already.
    const u32 count = load.eight ? 256 : 16;
    const u32 ta0 = load.texa_lo & 0xFF;
    const bool aem = (load.texa_lo & 0x8000u) != 0;
    for (u32 i = 0; i < count; ++i) {
        u32 x, y;
        if (load.eight) {
            const u32 p = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
            x = p % 16, y = p / 16;
        } else {
            x = i % 8, y = i / 8;
        }
        const u32 raw = memory_.Read(load.cbp, 1, x, y, load.cpsm == 0 ? u32(PSMCT32) : load.cpsm);
        u32 colour;
        if (load.cpsm == PSMCT32) {
            colour = raw;
        } else if (load.cpsm == PSMCT24) {
            const u32 rgb = raw & 0xFFFFFF;
            colour = rgb | ((aem && !rgb) ? 0u : ta0) << 24;
        } else {
            const u32 a = (raw & 0x8000u) ? load.ta1 : ((aem && !(raw & 0x7FFFu)) ? 0u : ta0);
            colour = ((raw & 31u) << 3) | (((raw >> 5) & 31u) << 11) | (((raw >> 10) & 31u) << 19) | (a << 24);
        }
        clut_[load.eight ? i : (load.csa * 16 + i) % 256] = colour;
    }
}

void GxmGsRenderer::ClutRows(const u32* clut, u32 rows) {
    rows &= 0xFFFF;
    if (!rows) return;
    for (u32 r = 0; r < 16; ++r)
        if (rows >> r & 1) std::memcpy(clut_ + r * 16, clut + r * 16, 64);
}

}  // namespace

// What the bridge asks of the GPU's side, the Vulkan renderer's otherwise.
void SetGpuWaitObserver(GpuWaitObserver observer) { g_wait_observer = observer; }
std::uint64_t GpuBusyNanoseconds() { return 0; }
std::string GpuProfileReport() { return ""; }
std::string RendererCounters() {
    char text[128];
    std::snprintf(text, sizeof text, "%llu draws, %llu scenes, %llu waits for the GPU", (unsigned long long)g_draws,
                  (unsigned long long)g_scenes, (unsigned long long)g_waits);
    g_draws = g_scenes = g_waits = 0;
    return text;
}
std::string SubmitStats() { return "GXM: the GPU finishes each frame before the next"; }

std::unique_ptr<GsRenderer> CreateGxmGsRenderer(std::string* error) {
    auto renderer = std::make_unique<GxmGsRenderer>();
    if (!renderer->Init(error)) return nullptr;
    return renderer;
}

}  // namespace ps2gpu
