/*
 * The fiber switch outside Windows (fibers.cpp): on ARM a few instructions of
 * our own (fiber_arm.S) - the Vita has no fibers, and its kernel's threads are
 * far too heavy to switch the EE's threads with; elsewhere ucontext.
 */

#ifndef PS2PLAT_FIBER_SWITCH_H
#define PS2PLAT_FIBER_SWITCH_H

#if defined(__arm__)
#define PS2PLAT_FIBER_ASM 1
extern "C" {
/* Saves the callee-saved registers (r4-r11, lr, d8-d15) on the current stack,
   the stack pointer in *from_sp, and resumes the fiber whose stack pointer is
   to_sp. */
void ps2plat_fiber_switch(void** from_sp, void* to_sp);
/* Where a new fiber's first switch lands: calls r4(r5), never returns. */
void ps2plat_fiber_start();
}
#else
#define PS2PLAT_FIBER_ASM 0
#endif

#endif  /* PS2PLAT_FIBER_SWITCH_H */
