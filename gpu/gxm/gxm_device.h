/*
 * The Vita's GPU for the engine (GXM): the device the display and, later,
 * the GS renderer draw with - GXM started once, its context and shader
 * patcher, memory the GPU can reach, and Cg programs compiled on the Vita
 * itself with libshacccg (the system's compiler, which players install in
 * ur0:data/) and kept as GXP in the data directory, so each is compiled once.
 *
 * Everything here belongs to one thread at a time: the GS thread draws and
 * presents; nothing else calls in once it has started.
 */

#ifndef PS2GPU_GXM_DEVICE_H
#define PS2GPU_GXM_DEVICE_H

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace ps2gpu {
namespace gxm {

/* A block of memory mapped for the GPU (sceGxmMapMemory): CDRAM (256 KB
   granules: surfaces, textures) or LPDDR uncached (4 KB granules: vertices,
   uniforms, what the CPU writes often and reads back). */
struct Memory {
    SceUID uid = -1;
    void* base = nullptr;
    std::uint32_t size = 0;
};
bool Allocate(Memory* memory, SceKernelMemBlockType type, std::uint32_t size, SceGxmMemoryAttribFlags attributes,
              const char* name);
void Release(Memory* memory);

/* What the display queue's callback is given with each picture
   (sceGxmDisplayQueueAddEntry): the buffer to show from the next refresh. */
struct DisplayEntry {
    void* address;
    std::uint32_t width, height, pitch;
};

class Device {
public:
    /* GXM started (once a process), the context, the shader patcher; false and
       why when something cannot be had. */
    bool Start(std::string* error);
    bool started() const { return context_ != nullptr; }

    SceGxmContext* context() const { return context_; }
    SceGxmShaderPatcher* patcher() const { return patcher_; }

    /* A Cg program, `name` for the cache and the log: the GXP kept from an
       earlier compile of the same source, or compiled now with libshacccg
       (loaded the first time) and kept. Null and why when it cannot be had -
       no libshacccg installed, a compile error. Registered with the shader
       patcher; the program lives as long as the device. */
    const SceGxmProgram* Program(const std::string& name, const char* source, bool fragment, std::string* error,
                                 SceGxmShaderPatcherId* id);
    /* Whether libshacccg could be loaded (tried once, by the first compile). */
    bool compiler() const { return compiler_ > 0; }

    /* GXM has one scene open at a time: the GS renderer's, while it draws
       into a target. What else begins one (the display) closes it first,
       through what the renderer set here. */
    void SetSceneCloser(std::function<void()> closer) { closer_ = std::move(closer); }
    void CloseOpenScene() {
        if (closer_) closer_();
    }

private:
    bool LoadCompiler(std::string* error);

    SceGxmContext* context_ = nullptr;
    SceGxmShaderPatcher* patcher_ = nullptr;
    Memory vdm_, vertex_ring_, fragment_ring_, patcher_buffer_;
    SceUID fragment_usse_ring_ = -1, patcher_vertex_usse_ = -1, patcher_fragment_usse_ = -1;
    int compiler_ = 0;          // 0 not tried, 1 loaded, -1 not there
    struct Kept {
        SceGxmProgram* program;
        SceGxmShaderPatcherId id;
    };
    std::map<std::string, Kept> programs_;
    std::function<void()> closer_;
};

/* The process's device (GXM has one context to give). */
Device& TheDevice();

}  // namespace gxm
}  // namespace ps2gpu

#endif  /* PS2GPU_GXM_DEVICE_H */
