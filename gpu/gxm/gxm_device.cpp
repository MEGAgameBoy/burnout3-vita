/*
 * The Vita's GPU device (gxm_device.h): GXM, its context and shader patcher,
 * GPU memory, and Cg programs compiled with libshacccg and kept as GXP.
 */

#include "gxm_device.h"

#include <psp2/display.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/shacccg.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ps2plat/platform.h"

namespace ps2gpu {
namespace gxm {

namespace {

std::string Hex(int value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08X", unsigned(value));
    return text;
}

/* Memory for the USSE (the shader cores' code): LPDDR uncached, mapped for
   the vertex or the fragment side, at an offset the GPU gives. */
SceUID UsseAllocate(bool fragment, std::uint32_t size, unsigned* offset) {
    size = (size + 4095u) & ~4095u;
    const SceUID uid = sceKernelAllocMemBlock(fragment ? "gxm fragment usse" : "gxm vertex usse",
                                              SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, nullptr);
    if (uid < 0) return uid;
    void* base = nullptr;
    sceKernelGetMemBlockBase(uid, &base);
    const int r = fragment ? sceGxmMapFragmentUsseMemory(base, size, offset)
                           : sceGxmMapVertexUsseMemory(base, size, offset);
    if (r < 0) {
        sceKernelFreeMemBlock(uid);
        return r;
    }
    return uid;
}

void* UsseBase(SceUID uid) {
    void* base = nullptr;
    sceKernelGetMemBlockBase(uid, &base);
    return base;
}

void* PatcherHostAlloc(void*, unsigned int size) { return std::malloc(size); }
void PatcherHostFree(void*, void* memory) { std::free(memory); }

/* The display queue's: a finished picture shown from the next refresh, the
   queue's thread waiting for it - so the queue drains at the screen's pace,
   and a display that finds it full (a loading screen's blanks eight times
   as fast) drops its picture rather than draw one the screen never shows:
   without the wait, 320 a second were drawn, the buffers turned over faster
   than the screen took them. */
void DisplayCallback(const void* data) {
    const DisplayEntry* entry = static_cast<const DisplayEntry*>(data);
    SceDisplayFrameBuf frame{};
    frame.size = sizeof frame;
    frame.base = entry->address;
    frame.pitch = entry->pitch;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = entry->width;
    frame.height = entry->height;
    sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitSetFrameBuf();
    if (entry->pending) entry->pending->fetch_sub(1, std::memory_order_release);
}

// ------------------------------------------------------------ the compiler

const char* g_source = nullptr;      // the source being compiled (one compile at a time)

SceShaccCgSourceFile* OpenSource(const char* name, const SceShaccCgSourceLocation*, const SceShaccCgCompileOptions*,
                                 const char**) {
    static SceShaccCgSourceFile file;
    file.fileName = name;
    file.text = g_source;
    file.size = SceUInt32(std::strlen(g_source));
    return &file;
}

/* FNV-1a: the cache's file name from the source, the profile and how it is
   compiled (kCompileOptions changes when the options below do). */
constexpr const char* kCompileOptions = "O3 fastmath fastint fx";
std::uint64_t Fingerprint(const char* source, bool fragment) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    auto add = [&h](const char* text) {
        for (const char* p = text; *p; ++p) h = (h ^ std::uint8_t(*p)) * 0x100000001B3ull;
        h = (h ^ 0xFF) * 0x100000001B3ull;
    };
    add(source);
    add(fragment ? "fp" : "vp");
    add(kCompileOptions);
    return h;
}

std::string CacheDirectory() { return ps2plat::JoinPath(ps2plat::DataDirectory(), "shaders"); }

SceGxmProgram* ReadCached(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    std::vector<std::uint8_t> bytes;
    std::uint8_t chunk[4096];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(f);
    if (bytes.size() < 32) return nullptr;
    SceGxmProgram* program = static_cast<SceGxmProgram*>(std::malloc(bytes.size()));
    std::memcpy(program, bytes.data(), bytes.size());
    if (sceGxmProgramCheck(program) < 0 || sceGxmProgramGetSize(program) != bytes.size()) {
        std::free(program);
        return nullptr;
    }
    return program;
}

}  // namespace

bool Allocate(Memory* memory, SceKernelMemBlockType type, std::uint32_t size, SceGxmMemoryAttribFlags attributes,
              const char* name) {
    const std::uint32_t granule = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256u * 1024u : 4096u;
    size = (size + granule - 1) & ~(granule - 1);
    const SceUID uid = sceKernelAllocMemBlock(name, type, size, nullptr);
    if (uid < 0) return false;
    void* base = nullptr;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || sceGxmMapMemory(base, size, attributes) < 0) {
        sceKernelFreeMemBlock(uid);
        return false;
    }
    memory->uid = uid;
    memory->base = base;
    memory->size = size;
    return true;
}

void Release(Memory* memory) {
    if (memory->uid < 0) return;
    sceGxmUnmapMemory(memory->base);
    sceKernelFreeMemBlock(memory->uid);
    *memory = Memory{};
}

bool Device::Start(std::string* error) {
    if (context_) return true;
    SceGxmInitializeParams init{};
    init.flags = 0;
    init.displayQueueMaxPendingCount = 2;
    init.displayQueueCallback = DisplayCallback;
    init.displayQueueCallbackDataSize = sizeof(DisplayEntry);
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    int r = sceGxmInitialize(&init);
    if (r < 0) {
        *error = "sceGxmInitialize " + Hex(r);
        return false;
    }
    const auto uncached = SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
    if (!Allocate(&vdm_, uncached, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ, "gxm vdm") ||
        !Allocate(&vertex_ring_, uncached, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ,
                  "gxm vertex ring") ||
        !Allocate(&fragment_ring_, uncached, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ,
                  "gxm fragment ring")) {
        *error = "no memory for GXM's rings";
        return false;
    }
    unsigned usse_offset = 0;
    fragment_usse_ring_ = UsseAllocate(true, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, &usse_offset);
    if (fragment_usse_ring_ < 0) {
        *error = "no memory for GXM's fragment USSE ring";
        return false;
    }
    SceGxmContextParams params{};
    params.hostMem = std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    params.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    params.vdmRingBufferMem = vdm_.base;
    params.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    params.vertexRingBufferMem = vertex_ring_.base;
    params.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    params.fragmentRingBufferMem = fragment_ring_.base;
    params.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    params.fragmentUsseRingBufferMem = UsseBase(fragment_usse_ring_);
    params.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    params.fragmentUsseRingBufferOffset = usse_offset;
    SceGxmContext* context = nullptr;
    r = sceGxmCreateContext(&params, &context);
    if (r < 0) {
        *error = "sceGxmCreateContext " + Hex(r);
        return false;
    }
    constexpr std::uint32_t kPatcherBuffer = 64 * 1024, kPatcherUsse = 64 * 1024;
    if (!Allocate(&patcher_buffer_, uncached, kPatcherBuffer, SCE_GXM_MEMORY_ATTRIB_READ, "gxm patcher")) {
        *error = "no memory for the shader patcher";
        return false;
    }
    unsigned vertex_offset = 0, fragment_offset = 0;
    patcher_vertex_usse_ = UsseAllocate(false, kPatcherUsse, &vertex_offset);
    patcher_fragment_usse_ = UsseAllocate(true, kPatcherUsse, &fragment_offset);
    if (patcher_vertex_usse_ < 0 || patcher_fragment_usse_ < 0) {
        *error = "no USSE memory for the shader patcher";
        return false;
    }
    SceGxmShaderPatcherParams sp{};
    sp.hostAllocCallback = PatcherHostAlloc;
    sp.hostFreeCallback = PatcherHostFree;
    sp.bufferMem = patcher_buffer_.base;
    sp.bufferMemSize = kPatcherBuffer;
    sp.vertexUsseMem = UsseBase(patcher_vertex_usse_);
    sp.vertexUsseMemSize = kPatcherUsse;
    sp.vertexUsseOffset = vertex_offset;
    sp.fragmentUsseMem = UsseBase(patcher_fragment_usse_);
    sp.fragmentUsseMemSize = kPatcherUsse;
    sp.fragmentUsseOffset = fragment_offset;
    r = sceGxmShaderPatcherCreate(&sp, &patcher_);
    if (r < 0) {
        *error = "sceGxmShaderPatcherCreate " + Hex(r);
        return false;
    }
    context_ = context;
    return true;
}

bool Device::LoadCompiler(std::string* error) {
    if (compiler_ > 0) return true;
    if (compiler_ < 0) {
        *error = "libshacccg is not installed (ur0:data/libshacccg.suprx)";
        return false;
    }
    for (const char* path : {"ur0:data/libshacccg.suprx", "ur0:data/external/libshacccg.suprx"}) {
        const SceUID id = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
        if (id >= 0) {
            sceShaccCgSetDefaultAllocator(std::malloc, std::free);
            const char* version = sceShaccCgGetVersionString();
            std::fprintf(stderr, "[gxm] libshacccg %s, from %s\n", version ? version : "(no version)", path);
            compiler_ = 1;
            return true;
        }
    }
    compiler_ = -1;
    *error = "libshacccg is not installed (ur0:data/libshacccg.suprx)";
    return false;
}

const SceGxmProgram* Device::Program(const std::string& name, const char* source, bool fragment, std::string* error,
                                     SceGxmShaderPatcherId* id) {
    char file[64];
    std::snprintf(file, sizeof file, "%016llx.gxp", (unsigned long long)Fingerprint(source, fragment));
    const std::string key = file;
    auto kept = programs_.find(key);
    if (kept != programs_.end()) {
        if (id) *id = kept->second.id;
        return kept->second.program;
    }
    const std::string directory = CacheDirectory();
    const std::string path = ps2plat::JoinPath(directory, name + "-" + key);
    SceGxmProgram* program = ReadCached(path);
    if (!program) {
        if (!LoadCompiler(error)) return nullptr;
        SceShaccCgCompileOptions options;
        sceShaccCgInitializeCompileOptions(&options);
        options.mainSourceFile = name.c_str();
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
        g_source = source;
        const SceShaccCgCompileOutput* result = sceShaccCgCompileProgram(&options, &callbacks, 0);
        g_source = nullptr;
        if (!result) {
            *error = name + ": the compiler returned nothing";
            return nullptr;
        }
        std::string message;
        for (int i = 0; i < result->diagnosticCount; ++i) {
            const SceShaccCgDiagnosticMessage& d = result->diagnostics[i];
            if (d.level == SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR && message.empty())
                message = d.message ? d.message : "an error";
        }
        if (result->programData && result->programSize) {
            program = static_cast<SceGxmProgram*>(std::malloc(result->programSize));
            std::memcpy(program, result->programData, result->programSize);
            if (sceGxmProgramCheck(program) < 0) {
                std::free(program);
                program = nullptr;
                message = "sceGxmProgramCheck refused it";
            }
        }
        sceShaccCgDestroyCompileOutput(result);
        if (!program) {
            *error = name + ": " + (message.empty() ? std::string("no program") : message);
            return nullptr;
        }
        sceIoMkdir(directory.c_str(), 0777);
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(program, 1, sceGxmProgramGetSize(program), f);
            std::fclose(f);
        }
        std::fprintf(stderr, "[gxm] %s compiled, kept as %s\n", name.c_str(), path.c_str());
    }
    SceGxmShaderPatcherId registered = nullptr;
    const int r = sceGxmShaderPatcherRegisterProgram(patcher_, program, &registered);
    if (r < 0) {
        std::free(program);
        *error = name + ": sceGxmShaderPatcherRegisterProgram " + Hex(r);
        return nullptr;
    }
    programs_[key] = Kept{program, registered};
    if (id) *id = registered;
    return program;
}

Device& TheDevice() {
    static Device device;
    return device;
}

}  // namespace gxm
}  // namespace ps2gpu
