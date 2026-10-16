/*
 * The GS renderer's tests on the Vita's GPU (ps2gxm_tests.vpk): the Vulkan
 * renderer's tests (engine/gpu/tests/renderer_tests.cpp) run on the GXM
 * renderer - each check on the screen as it comes, and in
 * ux0:data/ps2gxm_tests/results.txt, the file to send back. Needs
 * libshacccg, as the game's GPU renderer does.
 */

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include <cstdio>
#include <memory>
#include <string>

#include <psp2/gxm.h>

#include <cstring>

#include "../../gpu/gxm/gxm_device.h"
#include "ps2gpu/gs_renderer.h"
#include "ps2plat/platform.h"
#include "ps2plat/vita_console.h"

int RunRendererTests(ps2gpu::GsRenderer& renderer, bool say_passes);

/* vita-elf-create needs room for the module's own data after the code (2.4
   KB); this program's code ended 1.4 KB short of the 64 KB boundary its data
   starts on ("segment 1 overlaps"). Read-only padding in the code moves the
   boundary on. */
extern "C" {
__attribute__((used)) const char ps2gxm_tests_padding[8192] = {1};
}

namespace {

/* Depth across scenes, each way GXM keeps it: a depth surface's format,
   layout and memory; whether the CPU's writes to it reach the GPU (force
   load) and the GPU's reach the CPU (force store). The GS renderer's depth
   tests failed on the hardware in ways the tests alone could not tell
   apart. Per configuration: scene 1 writes Z 0.75 (or the GS-sized
   0x300000 / 2^32) over the left half; scene 2 draws green at 0.6 (or
   0x200000 / 2^32) over all of it with GEQUAL, over a CPU-written 0.5
   (0x100000 / 2^32) where scene 1 did not draw. Right: green on both
   sides' right, red kept on the left, and the CPU reading 0.75 / 0.6. */
void DepthProbe() {
    using namespace ps2gpu::gxm;
    Device& device = TheDevice();
    std::string error;
    if (!device.Start(&error)) {
        std::printf("depth probe: %s\n", error.c_str());
        return;
    }
    static const char kVertex[] =
        "void main(float3 aPosition, out float4 vPosition : POSITION) { vPosition = float4(aPosition, 1.0f); }\n";
    static const char kFragment[] = "float4 main(uniform float4 uColour) : COLOR { return uColour; }\n";
    SceGxmShaderPatcherId vid = nullptr, fid = nullptr;
    const SceGxmProgram* vgxp = device.Program("probe_vertex", kVertex, false, &error, &vid);
    const SceGxmProgram* fgxp = device.Program("probe_fragment", kFragment, true, &error, &fid);
    if (!vgxp || !fgxp) {
        std::printf("depth probe: %s\n", error.c_str());
        return;
    }
    SceGxmVertexAttribute attribute{};
    attribute.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attribute.componentCount = 3;
    attribute.regIndex = std::uint16_t(sceGxmProgramParameterGetResourceIndex(sceGxmProgramFindParameterByName(vgxp, "aPosition")));
    SceGxmVertexStream stream{};
    stream.stride = 12;
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    SceGxmVertexProgram* vp = nullptr;
    SceGxmFragmentProgram* fp = nullptr;
    if (sceGxmShaderPatcherCreateVertexProgram(device.patcher(), vid, &attribute, 1, &stream, 1, &vp) < 0 ||
        sceGxmShaderPatcherCreateFragmentProgram(device.patcher(), fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                 SCE_GXM_MULTISAMPLE_NONE, nullptr, vgxp, &fp) < 0) {
        std::printf("depth probe: no programs\n");
        return;
    }
    const SceGxmProgramParameter* colour = sceGxmProgramFindParameterByName(fgxp, "uColour");
    constexpr unsigned kSide = 64;
    SceGxmRenderTargetParams rtp{};
    rtp.width = kSide;
    rtp.height = kSide;
    rtp.scenesPerFrame = 1;
    rtp.driverMemBlock = -1;
    SceGxmRenderTarget* rt = nullptr;
    sceGxmCreateRenderTarget(&rtp, &rt);
    Memory target, vertices;
    Allocate(&target, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, kSide * kSide * 4, SCE_GXM_MEMORY_ATTRIB_RW, "probe target");
    Allocate(&vertices, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4096, SCE_GXM_MEMORY_ATTRIB_READ, "probe vertices");
    SceGxmColorSurface surface{};
    sceGxmColorSurfaceInit(&surface, SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
                           SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, kSide, kSide, kSide,
                           target.base);
    struct Config {
        const char* name;
        SceGxmDepthStencilFormat format;
        SceGxmDepthStencilSurfaceType type;
        SceKernelMemBlockType memory;
        bool tiny;
    };
    const Config configs[] = {
        {"DF32_S8 linear CDRAM", SCE_GXM_DEPTH_STENCIL_FORMAT_DF32_S8, SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, false},
        {"DF32_S8 linear CDRAM, GS-sized Z", SCE_GXM_DEPTH_STENCIL_FORMAT_DF32_S8, SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, true},
        {"DF32_S8 linear LPDDR", SCE_GXM_DEPTH_STENCIL_FORMAT_DF32_S8, SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, false},
        {"DF32 linear CDRAM", SCE_GXM_DEPTH_STENCIL_FORMAT_DF32, SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, false},
        {"DF32_S8 tiled CDRAM", SCE_GXM_DEPTH_STENCIL_FORMAT_DF32_S8, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, false},
        {"S8D24 tiled CDRAM", SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, false},
        {"S8D24 linear CDRAM", SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, false},
    };
    SceGxmContext* ctx = device.context();
    std::uint16_t* indices = reinterpret_cast<std::uint16_t*>(static_cast<std::uint8_t*>(vertices.base) + 2048);
    for (std::uint16_t i = 0; i < 6; ++i) indices[i] = i;
    float* v = static_cast<float*>(vertices.base);
    auto quad = [&](float* at, float x0, float x1, float z) {
        const float corners[6][2] = {{x0, -1}, {x1, -1}, {x0, 1}, {x1, -1}, {x1, 1}, {x0, 1}};
        for (int i = 0; i < 6; ++i) at[3 * i] = corners[i][0], at[3 * i + 1] = corners[i][1], at[3 * i + 2] = z;
    };
    for (const Config& c : configs) {
        Memory depth, stencil;
        if (!Allocate(&depth, c.memory, kSide * kSide * 4, SCE_GXM_MEMORY_ATTRIB_RW, "probe depth") ||
            !Allocate(&stencil, c.memory, kSide * kSide * 4, SCE_GXM_MEMORY_ATTRIB_RW, "probe stencil")) {
            std::printf("depth probe %s: no memory\n", c.name);
            continue;
        }
        const float far = c.tiny ? float(0x100000 / 4294967296.0) : 0.5f;
        const float left_z = c.tiny ? float(0x300000 / 4294967296.0) : 0.75f;
        const float green_z = c.tiny ? float(0x200000 / 4294967296.0) : 0.6f;
        float* d = static_cast<float*>(depth.base);
        for (unsigned i = 0; i < kSide * kSide; ++i) d[i] = far;           // the CPU's (as linear DF32 has it)
        std::memset(stencil.base, 0, kSide * kSide * 4);
        std::memset(target.base, 0, kSide * kSide * 4);
        SceGxmDepthStencilSurface ds{};
        sceGxmDepthStencilSurfaceInit(&ds, c.format, c.type, kSide, depth.base, stencil.base);
        sceGxmDepthStencilSurfaceSetForceLoadMode(&ds, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
        sceGxmDepthStencilSurfaceSetForceStoreMode(&ds, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
        const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
        for (int scene = 0; scene < 2; ++scene) {
            float* at = v + 18 * scene;
            if (scene == 0) quad(at, -1, 0, left_z);
            else quad(at, -1, 1, green_z);
            sceGxmBeginScene(ctx, 0, rt, nullptr, nullptr, nullptr, &surface, &ds);
            sceGxmSetViewportEnable(ctx, SCE_GXM_VIEWPORT_ENABLED);
            sceGxmSetViewport(ctx, kSide / 2.0f, kSide / 2.0f, kSide / 2.0f, kSide / 2.0f, 0.0f, 1.0f);
            sceGxmSetRegionClip(ctx, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0, kSide - 1, kSide - 1);
            sceGxmSetVertexProgram(ctx, vp);
            sceGxmSetFragmentProgram(ctx, fp);
            sceGxmSetFrontDepthFunc(ctx, scene == 0 ? SCE_GXM_DEPTH_FUNC_ALWAYS : SCE_GXM_DEPTH_FUNC_GREATER_EQUAL);
            sceGxmSetBackDepthFunc(ctx, scene == 0 ? SCE_GXM_DEPTH_FUNC_ALWAYS : SCE_GXM_DEPTH_FUNC_GREATER_EQUAL);
            sceGxmSetFrontDepthWriteEnable(ctx, SCE_GXM_DEPTH_WRITE_ENABLED);
            sceGxmSetBackDepthWriteEnable(ctx, SCE_GXM_DEPTH_WRITE_ENABLED);
            void* uniforms = nullptr;
            sceGxmReserveFragmentDefaultUniformBuffer(ctx, &uniforms);
            sceGxmSetUniformDataF(uniforms, colour, 0, 4, scene == 0 ? red : green);
            sceGxmSetVertexStream(ctx, 0, at);
            sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, 6);
            sceGxmEndScene(ctx, nullptr, nullptr);
            sceGxmFinish(ctx);
        }
        const std::uint32_t* px = static_cast<const std::uint32_t*>(target.base);
        const unsigned left = 32 * kSide + 8, right = 32 * kSide + 56;
        const bool linear_float = c.type == SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR &&
                                  c.format != SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24;
        std::printf("depth probe %-34s colour left %08X right %08X (want 000000FF-ish red left, green right)",
                    c.name, unsigned(px[left]), unsigned(px[right]));
        if (linear_float)
            std::printf("; Z left %.9g right %.9g (want %.9g, %.9g)", double(d[left]), double(d[right]),
                        double(left_z), double(green_z));
        else
            std::printf("; words left %08X right %08X", unsigned(reinterpret_cast<std::uint32_t*>(d)[left]),
                        unsigned(reinterpret_cast<std::uint32_t*>(d)[right]));
        std::printf("\n");
        Release(&depth);
        Release(&stencil);
    }
}

}  // namespace

extern "C" {
unsigned int _newlib_heap_size_user = 96u * 1024u * 1024u;
unsigned int sceUserMainThreadStackSize = 1u * 1024u * 1024u;     // not const: the loader reads it
}

int main() {
    namespace vita = ps2plat::vita;
    ps2plat::SetAppName("ps2gxm_tests");
    ps2plat::StartUp();
    vita::ConsoleShow();
    vita::ConsolePrint("ps2-port-engine: the GS renderer's tests on the Vita's GPU (GXM)\n\n", vita::kYellow);
    const std::string results = ps2plat::JoinPath(ps2plat::DataDirectory(), "results.txt");
    ps2plat::RedirectOutput(results);
    DepthProbe();
    std::string error;
    std::unique_ptr<ps2gpu::GsRenderer> renderer = ps2gpu::CreateGxmGsRenderer(&error);
    int failures = -1;
    if (!renderer) {
        std::printf("FAIL: no GXM renderer: %s\n", error.c_str());
    } else {
        failures = RunRendererTests(*renderer, true);
        std::printf("gxm renderer tests: %d failed\n", failures);
    }
    std::fflush(nullptr);
    // What was written, on the screen: the shaders' compiles, each check.
    if (std::FILE* f = std::fopen(results.c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            const std::string text(line);
            const bool failed = text.rfind("FAIL", 0) == 0, verdict = text.rfind("gxm renderer tests:", 0) == 0;
            if (text.rfind("[gxm]", 0) == 0) continue;
            vita::ConsolePrint(text, failed ? vita::kRed : verdict ? (failures ? vita::kRed : vita::kGreen) : vita::kWhite);
        }
        std::fclose(f);
    }
    vita::ConsolePrint("\nAll of it is in " + results + ". Press X to exit.\n", vita::kGrey);
    vita::ConsoleWaitButtons(SCE_CTRL_CROSS);
    ps2plat::QuickExit(failures == 0 ? 0 : 1);
}
