/*
 * The Vita's GPU, probed for what the GS renderer will rest on (GXM; the
 * engine's ps2gpu backend for the Vita is to come):
 *
 *   1. libshacccg, the system's Cg compiler, loaded from where players
 *      install it (ur0:data/, ur0:data/external/): its version, and how long
 *      a shader takes to compile at run time.
 *   2. A scene drawn into a target in CPU-visible memory and read back: a
 *      constant colour, from literals and from a uniform (the controls: GXM,
 *      the shader patcher, the read-back), with a depth/stencil surface - the
 *      first version's disabled one made every draw fail (0x805B001F).
 *   3. How the compiler spells 8-bit vectors, and fragment programs that
 *      read the pixel they replace - the GS's blend, FBMSK, DATE done in the
 *      shader - in each spelling it might take (the first version found
 *      FRAGCOLOR known but not on a plain varying, and __regformat not
 *      before a function); for each, whether it compiles, links, and leaves
 *      the expected bytes.
 *   4. Integer arithmetic in a fragment program (GS memory's swizzle).
 *
 * Results on the screen and in ux0:data/ps2plat_gxm_probe/result.txt.
 */

#include <psp2/ctrl.h>
#include <psp2/gxm.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/shacccg.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ps2plat/platform.h"
#include "ps2plat/vita_console.h"

extern "C" {
unsigned int _newlib_heap_size_user = 32u * 1024u * 1024u;
}

namespace {

namespace vita = ps2plat::vita;

std::FILE* g_out = nullptr;

void Say(const std::string& text, std::uint32_t colour = vita::kWhite) {
    vita::ConsolePrint(text, colour);
    if (g_out) {
        std::fputs(text.c_str(), g_out);
        std::fflush(g_out);
    }
}

std::string Hex(int value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08X", unsigned(value));
    return text;
}

// ------------------------------------------------------------ GPU memory

/* A block the GPU can read and write: LPDDR uncached (the CPU sees the GPU's
   writes without cache maintenance - the read-back), 4 KB granularity. */
void* GpuAlloc(SceKernelMemBlockType type, unsigned size, SceGxmMemoryAttribFlags attributes, SceUID* uid) {
    const unsigned granularity = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256u * 1024u : 4096u;
    size = (size + granularity - 1) & ~(granularity - 1);
    *uid = sceKernelAllocMemBlock("gxm probe", type, size, nullptr);
    if (*uid < 0) return nullptr;
    void* base = nullptr;
    if (sceKernelGetMemBlockBase(*uid, &base) < 0) return nullptr;
    if (sceGxmMapMemory(base, size, attributes) < 0) return nullptr;
    return base;
}

void* UsseAlloc(bool fragment, unsigned size, unsigned* offset) {
    size = (size + 4095u) & ~4095u;
    const SceUID uid = sceKernelAllocMemBlock("gxm probe usse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, nullptr);
    if (uid < 0) return nullptr;
    void* base = nullptr;
    sceKernelGetMemBlockBase(uid, &base);
    const int r = fragment ? sceGxmMapFragmentUsseMemory(base, size, offset) : sceGxmMapVertexUsseMemory(base, size, offset);
    return r < 0 ? nullptr : base;
}

void* PatcherHostAlloc(void*, unsigned int size) { return std::malloc(size); }
void PatcherHostFree(void*, void* mem) { std::free(mem); }
void DisplayCallback(const void*) {}

// ------------------------------------------------------------ the compiler

bool g_compiler = false;

bool LoadCompiler() {
    for (const char* path : {"ur0:data/libshacccg.suprx", "ur0:data/external/libshacccg.suprx"}) {
        const SceUID id = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
        if (id >= 0) {
            Say(std::string("libshacccg: loaded from ") + path + "\n", vita::kGreen);
            sceShaccCgSetDefaultAllocator(std::malloc, std::free);
            const char* version = sceShaccCgGetVersionString();
            Say(std::string("  version: ") + (version ? version : "(none)") + "\n");
            return true;
        }
        Say(std::string("libshacccg: not at ") + path + " (" + Hex(id) + ")\n", vita::kGrey);
    }
    return false;
}

struct Compiled {
    SceGxmProgram* program = nullptr;
    std::string error;
    double ms = 0;
};

const char* g_source_text = nullptr;

SceShaccCgSourceFile* OpenSource(const char* name, const SceShaccCgSourceLocation*, const SceShaccCgCompileOptions*,
                                 const char**) {
    static SceShaccCgSourceFile file;
    file.fileName = name;
    file.text = g_source_text;
    file.size = SceUInt32(std::strlen(g_source_text));
    return &file;
}

Compiled Compile(const char* source, bool fragment) {
    Compiled out;
    SceShaccCgCompileOptions options;
    sceShaccCgInitializeCompileOptions(&options);
    options.mainSourceFile = "probe.cg";
    options.targetProfile = fragment ? SCE_SHACCCG_PROFILE_FP : SCE_SHACCCG_PROFILE_VP;
    options.entryFunctionName = "main";
    options.macroDefinitions = nullptr;
    options.locale = SCE_SHACCCG_ENGLISH;
    options.useFx = 1;
    options.optimizationLevel = 3;
    options.useFastmath = 1;
    options.useFastint = 1;
    SceShaccCgCallbackList callbacks;
    sceShaccCgInitializeCallbackList(&callbacks, SCE_SHACCCG_TRIVIAL);
    callbacks.openFile = OpenSource;
    g_source_text = source;
    const SceUInt64 began = sceKernelGetProcessTimeWide();
    const SceShaccCgCompileOutput* result = sceShaccCgCompileProgram(&options, &callbacks, 0);
    out.ms = double(sceKernelGetProcessTimeWide() - began) / 1000.0;
    if (!result) {
        out.error = "the compiler returned nothing";
        return out;
    }
    for (int i = 0; i < result->diagnosticCount; ++i) {
        const SceShaccCgDiagnosticMessage& d = result->diagnostics[i];
        if (d.level == SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR && out.error.empty())
            out.error = d.message ? d.message : "an error";
    }
    if (result->programData && result->programSize) {
        out.program = static_cast<SceGxmProgram*>(std::malloc(result->programSize));
        std::memcpy(out.program, result->programData, result->programSize);
        if (sceGxmProgramCheck(out.program) < 0) {
            out.error = "sceGxmProgramCheck refused the program";
            std::free(out.program);
            out.program = nullptr;
        }
    } else if (out.error.empty()) {
        out.error = "no program and no error";
    }
    sceShaccCgDestroyCompileOutput(result);
    return out;
}

// ------------------------------------------------------------ the GPU

constexpr unsigned kSide = 64;

struct Gpu {
    SceGxmContext* context = nullptr;
    SceGxmShaderPatcher* patcher = nullptr;
    SceGxmRenderTarget* target = nullptr;
    SceGxmColorSurface surface{};
    SceGxmDepthStencilSurface depth{};
    std::uint32_t* pixels = nullptr;
    float* vertices = nullptr;
    std::uint16_t* indices = nullptr;
};

bool Start(Gpu& g) {
    SceGxmInitializeParams init{};
    init.flags = 0;
    init.displayQueueMaxPendingCount = 1;
    init.displayQueueCallback = DisplayCallback;
    init.displayQueueCallbackDataSize = 4;
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    int r = sceGxmInitialize(&init);
    if (r < 0) {
        Say("sceGxmInitialize: " + Hex(r) + "\n", vita::kRed);
        return false;
    }
    SceUID uid;
    SceGxmContextParams params{};
    params.hostMem = std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    params.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    params.vdmRingBufferMem = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE,
                                       SCE_GXM_MEMORY_ATTRIB_READ, &uid);
    params.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    params.vertexRingBufferMem = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                                          SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ, &uid);
    params.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    params.fragmentRingBufferMem = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                                            SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ, &uid);
    params.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    unsigned usse_offset = 0;
    params.fragmentUsseRingBufferMem = UsseAlloc(true, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, &usse_offset);
    params.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    params.fragmentUsseRingBufferOffset = usse_offset;
    r = sceGxmCreateContext(&params, &g.context);
    if (r < 0) {
        Say("sceGxmCreateContext: " + Hex(r) + "\n", vita::kRed);
        return false;
    }
    SceGxmRenderTargetParams rt{};
    rt.flags = 0;
    rt.width = kSide;
    rt.height = kSide;
    rt.scenesPerFrame = 1;
    rt.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rt.multisampleLocations = 0;
    rt.driverMemBlock = -1;
    r = sceGxmCreateRenderTarget(&rt, &g.target);
    if (r < 0) {
        Say("sceGxmCreateRenderTarget: " + Hex(r) + "\n", vita::kRed);
        return false;
    }
    g.pixels = static_cast<std::uint32_t*>(
        GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, kSide * kSide * 4, SCE_GXM_MEMORY_ATTRIB_RW, &uid));
    r = sceGxmColorSurfaceInit(&g.surface, SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
                               SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, kSide, kSide, kSide,
                               g.pixels);
    if (r < 0) {
        Say("sceGxmColorSurfaceInit: " + Hex(r) + "\n", vita::kRed);
        return false;
    }
    // A depth and stencil surface, S8D24 and tiled as vita2d's: with a
    // disabled one every draw failed (0x805B001F, the first probe's run).
    const unsigned tiled_w = (kSide + SCE_GXM_TILE_SIZEX - 1) & ~(SCE_GXM_TILE_SIZEX - 1);
    const unsigned samples = tiled_w * ((kSide + SCE_GXM_TILE_SIZEY - 1) & ~(SCE_GXM_TILE_SIZEY - 1));
    void* depth = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * samples, SCE_GXM_MEMORY_ATTRIB_RW, &uid);
    void* stencil = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * samples, SCE_GXM_MEMORY_ATTRIB_RW, &uid);
    r = sceGxmDepthStencilSurfaceInit(&g.depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                      tiled_w, depth, stencil);
    if (r < 0) {
        Say("sceGxmDepthStencilSurfaceInit: " + Hex(r) + "\n", vita::kRed);
        return false;
    }
    SceGxmShaderPatcherParams sp{};
    sp.hostAllocCallback = PatcherHostAlloc;
    sp.hostFreeCallback = PatcherHostFree;
    sp.bufferMem = GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 64 * 1024, SCE_GXM_MEMORY_ATTRIB_READ, &uid);
    sp.bufferMemSize = 64 * 1024;
    unsigned vertex_offset = 0, fragment_offset = 0;
    sp.vertexUsseMem = UsseAlloc(false, 64 * 1024, &vertex_offset);
    sp.vertexUsseMemSize = 64 * 1024;
    sp.vertexUsseOffset = vertex_offset;
    sp.fragmentUsseMem = UsseAlloc(true, 64 * 1024, &fragment_offset);
    sp.fragmentUsseMemSize = 64 * 1024;
    sp.fragmentUsseOffset = fragment_offset;
    r = sceGxmShaderPatcherCreate(&sp, &g.patcher);
    if (r < 0) {
        Say("sceGxmShaderPatcherCreate: " + Hex(r) + "\n", vita::kRed);
        return false;
    }
    // A triangle covering the whole target.
    g.vertices = static_cast<float*>(GpuAlloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4096, SCE_GXM_MEMORY_ATTRIB_READ, &uid));
    const float corners[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    std::memcpy(g.vertices, corners, sizeof corners);
    g.indices = reinterpret_cast<std::uint16_t*>(g.vertices + 64);
    g.indices[0] = 0, g.indices[1] = 1, g.indices[2] = 2;
    return true;
}

struct Program {
    SceGxmShaderPatcherId id = nullptr;
    const SceGxmProgram* gxp = nullptr;
};

bool Register(Gpu& g, const Compiled& c, Program* p) {
    if (!c.program) return false;
    const int r = sceGxmShaderPatcherRegisterProgram(g.patcher, c.program, &p->id);
    if (r < 0) return false;
    p->gxp = c.program;
    return true;
}

SceGxmVertexProgram* VertexProgram(Gpu& g, const Program& p) {
    const SceGxmProgramParameter* position = sceGxmProgramFindParameterByName(p.gxp, "aPosition");
    if (!position) return nullptr;
    SceGxmVertexAttribute attribute{};
    attribute.streamIndex = 0;
    attribute.offset = 0;
    attribute.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attribute.componentCount = 2;
    attribute.regIndex = std::uint16_t(sceGxmProgramParameterGetResourceIndex(position));
    SceGxmVertexStream stream{};
    stream.stride = 8;
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    SceGxmVertexProgram* vp = nullptr;
    return sceGxmShaderPatcherCreateVertexProgram(g.patcher, p.id, &attribute, 1, &stream, 1, &vp) < 0 ? nullptr : vp;
}

SceGxmFragmentProgram* FragmentProgram(Gpu& g, const Program& p, const Program& vertex, std::string* how) {
    for (SceGxmOutputRegisterFormat format : {SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED}) {
        SceGxmFragmentProgram* fp = nullptr;
        const int r = sceGxmShaderPatcherCreateFragmentProgram(g.patcher, p.id, format, SCE_GXM_MULTISAMPLE_NONE, nullptr,
                                                               vertex.gxp, &fp);
        if (r >= 0) {
            *how = format == SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4 ? "UCHAR4 output" : "DECLARED output";
            return fp;
        }
        *how = "sceGxmShaderPatcherCreateFragmentProgram " + Hex(r);
    }
    return nullptr;
}

/* A fragment program to draw with, and the program it came from (for its
   default uniform buffer: uColour, when it has one, gets `colour`). */
struct Pass {
    SceGxmFragmentProgram* fp;
    const SceGxmProgram* gxp;
    float colour[4];
};

/* One scene: the target filled with zeros by the CPU, then each pass over
   the whole target. */
bool Draw(Gpu& g, SceGxmVertexProgram* vp, const SceGxmProgram* vertex_gxp, const std::vector<Pass>& passes) {
    std::memset(g.pixels, 0, kSide * kSide * 4);
    int r = sceGxmBeginScene(g.context, 0, g.target, nullptr, nullptr, nullptr, &g.surface, &g.depth);
    if (r < 0) {
        Say("  sceGxmBeginScene: " + Hex(r) + "\n", vita::kRed);
        return false;
    }
    // Clip space onto the whole target (y down, as the surface's rows); no
    // depth test or write.
    sceGxmSetViewportEnable(g.context, SCE_GXM_VIEWPORT_ENABLED);
    sceGxmSetViewport(g.context, kSide / 2.0f, kSide / 2.0f, kSide / 2.0f, -(kSide / 2.0f), 0.5f, 0.5f);
    sceGxmSetFrontDepthFunc(g.context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(g.context, SCE_GXM_DEPTH_WRITE_DISABLED);
    bool ok = true;
    for (const Pass& pass : passes) {
        sceGxmSetVertexProgram(g.context, vp);
        sceGxmSetFragmentProgram(g.context, pass.fp);
        if (sceGxmProgramGetDefaultUniformBufferSize(vertex_gxp)) {
            void* buffer = nullptr;
            sceGxmReserveVertexDefaultUniformBuffer(g.context, &buffer);
        }
        if (sceGxmProgramGetDefaultUniformBufferSize(pass.gxp)) {
            void* buffer = nullptr;
            r = sceGxmReserveFragmentDefaultUniformBuffer(g.context, &buffer);
            if (r < 0) Say("  sceGxmReserveFragmentDefaultUniformBuffer: " + Hex(r) + "\n", vita::kRed);
            if (const SceGxmProgramParameter* colour = sceGxmProgramFindParameterByName(pass.gxp, "uColour"))
                sceGxmSetUniformDataF(buffer, colour, 0, 4, pass.colour);
        }
        sceGxmSetVertexStream(g.context, 0, g.vertices);
        r = sceGxmDraw(g.context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, g.indices, 3);
        if (r < 0) {
            Say("  sceGxmDraw: " + Hex(r) + "\n", vita::kRed);
            ok = false;
        }
    }
    sceGxmEndScene(g.context, nullptr, nullptr);
    sceGxmFinish(g.context);
    return ok;
}

std::string Pixel(const Gpu& g, unsigned x, unsigned y, std::uint8_t out[4]) {
    const std::uint32_t p = g.pixels[y * kSide + x];
    for (int i = 0; i < 4; ++i) out[i] = std::uint8_t(p >> (8 * i));
    char text[64];
    std::snprintf(text, sizeof text, "(%u, %u, %u, %u)", out[0], out[1], out[2], out[3]);
    return text;
}

bool Near(const std::uint8_t got[4], const int want[4], int tolerance) {
    for (int i = 0; i < 4; ++i)
        if (std::abs(int(got[i]) - want[i]) > tolerance) return false;
    return true;
}

// ------------------------------------------------------------ the shaders

const char kVertex[] =
    "void main(float2 aPosition, out float4 vPosition : POSITION) {\n"
    "    vPosition = float4(aPosition, 0.5f, 1.0f);\n"
    "}\n";

// The controls: (0.8, 0.4, 0.2, 1.0), as bytes (204, 102, 51, 255) - from
// literals, and from a uniform (vita2d's clear).
const char kConstant[] = "float4 main() : COLOR { return float4(0.8f, 0.4f, 0.2f, 1.0f); }\n";
const char kUniformConstant[] = "float4 main(uniform float4 uColour) : COLOR { return uColour; }\n";

// How the compiler spells an 8-bit vector (compiled only).
struct Spelling {
    const char* name;
    const char* source;
};
const Spelling kSpellings[] = {
    {"uniform uchar4", "float4 main(uniform uchar4 u) : COLOR { return float4(u) / 255.0f; }\n"},
    {"uniform unsigned char4", "float4 main(uniform unsigned char4 u) : COLOR { return float4(u) / 255.0f; }\n"},
    {"uniform __regformat float4", "float4 main(uniform __regformat float4 u) : COLOR { return u; }\n"},
    {"out __regformat uchar4 : COLOR",
     "void main(out __regformat uchar4 col : COLOR) { col = uchar4(1, 2, 3, 4); }\n"},
    {"out __regformat unsigned char4 : COLOR",
     "void main(out __regformat unsigned char4 col : COLOR) { col = unsigned char4(1, 2, 3, 4); }\n"},
};

// The destination read: r halved, g + 0.25 (64), b inverted, a one - from
// (204, 102, 51, 255) that is (102, 166, 204, 255).
struct Variant {
    const char* name;
    const char* source;
    int tolerance;
};
const Variant kVariants[] = {
    {"in __regformat uchar4 : FRAGCOLOR, out uchar4",
     "void main(__regformat uchar4 dst : FRAGCOLOR, out __regformat uchar4 col : COLOR) {\n"
     "    col = uchar4(dst.r >> 1, dst.g + 64, 255 - dst.b, 255);\n"
     "}\n", 0},
    {"in __regformat unsigned char4 : FRAGCOLOR, out",
     "void main(__regformat unsigned char4 dst : FRAGCOLOR, out __regformat unsigned char4 col : COLOR) {\n"
     "    col = unsigned char4(dst.r >> 1, dst.g + 64, 255 - dst.b, 255);\n"
     "}\n", 0},
    {"in __regformat uchar4 : FRAGCOLOR, float4 out",
     "float4 main(__regformat uchar4 dst : FRAGCOLOR) : COLOR {\n"
     "    float4 d = float4(dst) / 255.0f;\n"
     "    return float4(d.r * 0.5f, d.g + 0.25f, 1.0f - d.b, 1.0f);\n"
     "}\n", 2},
    {"in __regformat unsigned char4 : FRAGCOLOR, float4",
     "float4 main(__regformat unsigned char4 dst : FRAGCOLOR) : COLOR {\n"
     "    float4 d = float4(dst) / 255.0f;\n"
     "    return float4(d.r * 0.5f, d.g + 0.25f, 1.0f - d.b, 1.0f);\n"
     "}\n", 2},
    {"uniform __regformat uchar4 : FRAGCOLOR",
     "void main(uniform __regformat uchar4 dst : FRAGCOLOR, out __regformat uchar4 col : COLOR) {\n"
     "    col = uchar4(dst.r >> 1, dst.g + 64, 255 - dst.b, 255);\n"
     "}\n", 0},
    {"uniform float4 : FRAGCOLOR",
     "float4 main(uniform float4 dst : FRAGCOLOR) : COLOR {\n"
     "    return float4(dst.r * 0.5f, dst.g + 0.25f, 1.0f - dst.b, 1.0f);\n"
     "}\n", 2},
    {"in __regformat float4 : FRAGCOLOR",
     "float4 main(__regformat float4 dst : FRAGCOLOR) : COLOR {\n"
     "    return float4(dst.r * 0.5f, dst.g + 0.25f, 1.0f - dst.b, 1.0f);\n"
     "}\n", 2},
    {"in __regformat half4 : FRAGCOLOR",
     "half4 main(__regformat half4 dst : FRAGCOLOR) : COLOR {\n"
     "    return half4(dst.r * 0.5h, dst.g + 0.25h, 1.0h - dst.b, 1.0h);\n"
     "}\n", 2},
    {"out __nativecolor __regformat uchar4 : COLOR",
     "void main(__regformat uchar4 dst : FRAGCOLOR, out __nativecolor __regformat uchar4 col : COLOR) {\n"
     "    col = uchar4(dst.r >> 1, dst.g + 64, 255 - dst.b, 255);\n"
     "}\n", 0},
    {"inout float4 : COLOR",
     "void main(inout float4 col : COLOR) {\n"
     "    col = float4(col.r * 0.5f, col.g + 0.25f, 1.0f - col.b, 1.0f);\n"
     "}\n", 2},
};

// Integers in a fragment program: at pixel (5, 9), ((x ^ y) & 31) << 3 = 96.
const char kIntegers[] =
    "float4 main(float2 pos : WPOS) : COLOR {\n"
    "    int x = (int)pos.x, y = (int)pos.y;\n"
    "    int v = ((x ^ y) & 31) << 3;\n"
    "    return float4((float)v / 255.0f, (float)(x * 3) / 255.0f, (float)(y >> 1) / 255.0f, 1.0f);\n"
    "}\n";

}  // namespace

int main() {
    ps2plat::SetAppName("ps2plat_gxm_probe");
    ps2plat::StartUp();
    vita::ConsoleShow();
    g_out = std::fopen(ps2plat::JoinPath(ps2plat::DataDirectory(), "result.txt").c_str(), "w");
    Say("GXM probe (2): the Vita's GPU for the GS renderer\n\n", vita::kYellow);
    int failures = 0;

    g_compiler = LoadCompiler();
    if (!g_compiler) {
        Say("\nNo libshacccg: install it (e.g. VitaDB Downloader, or the PIB tool) and run this again.\n", vita::kRed);
        ++failures;
    }
    Gpu g;
    if (g_compiler && !Start(g)) ++failures, g_compiler = false;

    Program vertex, constant, uniform_constant;
    SceGxmVertexProgram* vp = nullptr;
    SceGxmFragmentProgram* constant_fp = nullptr;      // the first control that drew right
    const SceGxmProgram* constant_gxp = nullptr;
    char line[320];
    if (g_compiler) {
        const Compiled v = Compile(kVertex, false), c = Compile(kConstant, true), u = Compile(kUniformConstant, true);
        std::snprintf(line, sizeof line, "\nvertex shader: %s (%.1f ms)\n", v.program ? "compiled" : v.error.c_str(), v.ms);
        Say(line, v.program ? vita::kWhite : vita::kRed);
        std::string how;
        if (Register(g, v, &vertex)) vp = VertexProgram(g, vertex);
        if (!vp) {
            Say("the vertex program could not be made\n", vita::kRed);
            ++failures;
        } else {
            std::snprintf(line, sizeof line, "  default uniform buffer: %u bytes\n",
                          sceGxmProgramGetDefaultUniformBufferSize(vertex.gxp));
            Say(line, vita::kGrey);
        }
        const int want[4] = {204, 102, 51, 255};
        const struct {
            const char* name;
            const Compiled* compiled;
            Program* program;
        } controls[] = {{"control (literal)", &c, &constant}, {"control (uniform)", &u, &uniform_constant}};
        for (const auto& control : controls) {
            if (!vp) break;
            SceGxmFragmentProgram* fp =
                Register(g, *control.compiled, control.program) ? FragmentProgram(g, *control.program, vertex, &how) : nullptr;
            if (!fp) {
                Say(std::string(control.name) + ": " + (control.compiled->program ? how : control.compiled->error) + "\n",
                    vita::kRed);
                ++failures;
                continue;
            }
            Draw(g, vp, vertex.gxp, {{fp, control.program->gxp, {0.8f, 0.4f, 0.2f, 1.0f}}});
            std::uint8_t px[4];
            const std::string got = Pixel(g, 32, 32, px);
            const bool ok = Near(px, want, 1);
            std::snprintf(line, sizeof line, "%s: %s %s (uniform buffer %u bytes, %.0f ms)\n", control.name, got.c_str(),
                          ok ? "as expected" : "- expected (204, 102, 51, 255)",
                          sceGxmProgramGetDefaultUniformBufferSize(control.program->gxp), control.compiled->ms);
            Say(line, ok ? vita::kGreen : vita::kRed);
            failures += !ok;
            if (ok && !constant_fp) constant_fp = fp, constant_gxp = control.program->gxp;
        }
    }

    if (g_compiler) {
        Say("\nhow the compiler spells 8-bit vectors:\n", vita::kYellow);
        for (const Spelling& s : kSpellings) {
            const Compiled c = Compile(s.source, true);
            std::snprintf(line, sizeof line, "  %-40s %.100s\n", s.name, c.program ? "compiles" : c.error.c_str());
            Say(line, c.program ? vita::kGreen : vita::kGrey);
        }
    }

    if (vp && constant_fp) {
        Say("\nreading the destination in the shader:\n", vita::kYellow);
        const int want[4] = {102, 166, 204, 255};
        for (const Variant& variant : kVariants) {
            const Compiled c = Compile(variant.source, true);
            if (!c.program) {
                std::snprintf(line, sizeof line, "  %-46s does not compile: %.100s\n", variant.name, c.error.c_str());
                Say(line, vita::kGrey);
                continue;
            }
            Program p;
            std::string how;
            SceGxmFragmentProgram* fp = Register(g, c, &p) ? FragmentProgram(g, p, vertex, &how) : nullptr;
            if (!fp) {
                std::snprintf(line, sizeof line, "  %-46s compiles, does not link: %s\n", variant.name, how.c_str());
                Say(line, vita::kRed);
                continue;
            }
            Draw(g, vp, vertex.gxp, {{constant_fp, constant_gxp, {0.8f, 0.4f, 0.2f, 1.0f}}, {fp, p.gxp, {0, 0, 0, 0}}});
            std::uint8_t px[4];
            const std::string got = Pixel(g, 32, 32, px);
            const bool ok = Near(px, want, variant.tolerance);
            std::snprintf(line, sizeof line, "  %-46s %s %s (%s, %.0f ms)\n", variant.name, got.c_str(),
                          ok ? "WORKS" : "wrong", how.c_str(), c.ms);
            Say(line, ok ? vita::kGreen : vita::kRed);
        }

        Say("\nintegers in a fragment program:\n", vita::kYellow);
        const Compiled c = Compile(kIntegers, true);
        Program p;
        std::string how;
        SceGxmFragmentProgram* fp = c.program && Register(g, c, &p) ? FragmentProgram(g, p, vertex, &how) : nullptr;
        if (!fp) {
            Say("  " + (c.program ? how : c.error) + "\n", vita::kRed);
            ++failures;
        } else {
            Draw(g, vp, vertex.gxp, {{fp, p.gxp, {0, 0, 0, 0}}});
            std::uint8_t px[4];
            const int iwant[4] = {96, 15, 4, 255};
            const std::string got = Pixel(g, 5, 9, px);
            const bool ok = Near(px, iwant, 1);
            Say("  at (5, 9): " + got + (ok ? " as expected\n" : " - expected (96, 15, 4, 255)\n"),
                ok ? vita::kGreen : vita::kRed);
            failures += !ok;
        }
    }

    Say(failures ? "\nSome of it failed: send result.txt.\n" : "\nDone.\n", failures ? vita::kRed : vita::kGreen);
    Say("All of it is in " + ps2plat::JoinPath(ps2plat::DataDirectory(), "result.txt") + ". Press X to exit.\n",
        vita::kGrey);
    if (g_out) std::fclose(g_out);
    vita::ConsoleWaitButtons(SCE_CTRL_CROSS);
    ps2plat::QuickExit(0);
}
