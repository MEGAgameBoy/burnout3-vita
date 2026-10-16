/*
 * Fibers outside Windows: on the Vita the system's own (SceFiber), on other
 * ARM systems the switch of fiber_arm.S over stacks of our own, elsewhere
 * ucontext (Linux on x86-64, for the tests).
 *
 * The Vita's kernel checks, at every system call, that the stack pointer is
 * in the calling thread's stack: a fiber of our own making - its stack on the
 * heap - was stopped at its first (the game's first log line: stop reason
 * 0x10006 in sceIoLseek, the stack pointer in the heap). SceFiber's switches
 * tell the kernel of the stack they move to.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "fiber_switch.h"
#include "ps2plat/platform.h"

#if defined(__vita__)
#include <psp2/fiber.h>
#include <psp2/sysmodule.h>

#include <cstdio>
#elif !PS2PLAT_FIBER_ASM
#include <ucontext.h>
#endif

namespace ps2plat {

#if defined(__vita__)

struct Fiber {
    SceFiber fiber;                 // first: SceFiber wants it 8-aligned (the struct's alignment)
    void* stack = nullptr;          // null: the thread itself (ThreadToFiber), no SceFiber
    void (*entry)(void*) = nullptr;
    void* argument = nullptr;
};

namespace {
thread_local Fiber* t_current = nullptr;

void Entry(SceUInt32 argument_on_initialize, SceUInt32) {
    auto* fiber = reinterpret_cast<Fiber*>(std::uintptr_t(argument_on_initialize));
    fiber->entry(fiber->argument);
    std::abort();                   // a fiber's entry never returns
}

void Check(SceInt32 result, const char* what) {
    if (result >= 0) return;
    std::fprintf(stderr, "[fiber] %s: 0x%08X\n", what, unsigned(result));
    std::abort();
}
}  // namespace

Fiber* ThreadToFiber() {
    if (!t_current) t_current = new Fiber;      // the thread: what sceFiberReturnToThread goes back to
    return t_current;
}

Fiber* NewFiber(std::size_t stack_size, void (*entry)(void*), void* argument) {
    static const bool loaded = sceSysmoduleLoadModule(SCE_SYSMODULE_FIBER) >= 0 || true;
    (void)loaded;
    stack_size = (stack_size + 7) & ~std::size_t(7);
    void* stack = std::malloc(stack_size);     // 8-aligned, as SceFiber wants its context
    if (!stack) throw std::bad_alloc();
    auto* fiber = new Fiber;
    fiber->stack = stack;
    fiber->entry = entry;
    fiber->argument = argument;
    static char name[] = "ps2plat fiber";
    Check(_sceFiberInitializeImpl(&fiber->fiber, name, &Entry, SceUInt32(reinterpret_cast<std::uintptr_t>(fiber)),
                                  stack, SceSize(stack_size), nullptr),
          "_sceFiberInitializeImpl");
    return fiber;
}

void SwitchTo(Fiber* to) {
    Fiber* from = ThreadToFiber();
    if (to == from) return;
    t_current = to;
    SceUInt32 got = 0;
    if (!from->stack) Check(sceFiberRun(&to->fiber, 0, &got), "sceFiberRun");             // the thread, into a fiber
    else if (!to->stack) Check(sceFiberReturnToThread(0, &got), "sceFiberReturnToThread"); // a fiber, back to the thread
    else Check(sceFiberSwitch(&to->fiber, 0, &got), "sceFiberSwitch");                     // fiber to fiber
}

void FreeFiber(Fiber* fiber) {
    if (!fiber || fiber == t_current || !fiber->stack) return;
    sceFiberFinalize(&fiber->fiber);
    std::free(fiber->stack);
    delete fiber;
}

#else

struct Fiber {
#if PS2PLAT_FIBER_ASM
    void* sp = nullptr;             // while it is not running: where its registers are
#else
    ucontext_t context{};
    void (*entry)(void*) = nullptr;
    void* argument = nullptr;
#endif
    void* stack = nullptr;          // null: a thread's own stack (ThreadToFiber)
};

namespace {
thread_local Fiber* t_current = nullptr;

#if !PS2PLAT_FIBER_ASM
void Trampoline(unsigned high, unsigned low) {
    auto* fiber = reinterpret_cast<Fiber*>(std::uintptr_t((std::uint64_t(high) << 32) | low));
    fiber->entry(fiber->argument);
    std::abort();                   // a fiber's entry never returns
}
#endif
}  // namespace

Fiber* ThreadToFiber() {
    if (!t_current) t_current = new Fiber;
    return t_current;
}

Fiber* NewFiber(std::size_t stack_size, void (*entry)(void*), void* argument) {
    void* stack = std::malloc(stack_size);
    if (!stack) throw std::bad_alloc();
    auto* fiber = new Fiber;
    fiber->stack = stack;
#if PS2PLAT_FIBER_ASM
    // The frame ps2plat_fiber_switch pops, at the top: d8-d15 (16 words),
    // then r3-r11 and the return address (10 words).
    const std::uintptr_t top = (reinterpret_cast<std::uintptr_t>(stack) + stack_size) & ~std::uintptr_t(15);
    auto* frame = reinterpret_cast<std::uint32_t*>(top) - 26;
    std::memset(frame, 0, 26 * sizeof(std::uint32_t));
    frame[16 + 1] = std::uint32_t(reinterpret_cast<std::uintptr_t>(entry));      // r4
    frame[16 + 2] = std::uint32_t(reinterpret_cast<std::uintptr_t>(argument));   // r5
    frame[16 + 9] = std::uint32_t(reinterpret_cast<std::uintptr_t>(&ps2plat_fiber_start));   // pc
    fiber->sp = frame;
#else
    fiber->entry = entry;
    fiber->argument = argument;
    getcontext(&fiber->context);
    fiber->context.uc_stack.ss_sp = stack;
    fiber->context.uc_stack.ss_size = stack_size;
    fiber->context.uc_link = nullptr;
    const auto address = std::uint64_t(reinterpret_cast<std::uintptr_t>(fiber));
    makecontext(&fiber->context, reinterpret_cast<void (*)()>(&Trampoline), 2, unsigned(address >> 32),
                unsigned(address & 0xFFFFFFFFu));
#endif
    return fiber;
}

void SwitchTo(Fiber* to) {
    Fiber* from = ThreadToFiber();
    if (to == from) return;
    t_current = to;
#if PS2PLAT_FIBER_ASM
    ps2plat_fiber_switch(&from->sp, to->sp);
#else
    swapcontext(&from->context, &to->context);
#endif
}

void FreeFiber(Fiber* fiber) {
    if (!fiber || fiber == t_current) return;
    std::free(fiber->stack);
    delete fiber;
}

#endif

}  // namespace ps2plat
