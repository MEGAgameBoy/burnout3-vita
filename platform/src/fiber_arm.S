/*
 * Fibers on ARMv7-A (the Vita; ARM Linux under qemu for the tests), AAPCS
 * with VFP: a switch saves what a call must preserve - r4-r11, the return
 * address, d8-d15 - on the stack it leaves, and restores the same from the
 * stack it enters. r3 rides along to keep the frame a multiple of 8 bytes
 * (the stack is 8-aligned at every call). FPSCR is not switched: it belongs
 * to the thread, and the host sets it around the code that needs it.
 *
 * A new fiber's stack (fibers.cpp) holds a frame this switch pops: d8-d15
 * zero, r4 the entry, r5 its argument, and ps2plat_fiber_start as the return
 * address.
 *
 * Both are marked as having no unwind table entry beyond themselves: C++
 * exceptions are thrown and caught inside a fiber's own frames (threads.cpp
 * catches everything at a fiber's base), never across a switch.
 */

    .syntax unified
    .arch armv7-a
    .fpu neon
    .arm
    .text

    .global ps2plat_fiber_switch
    .type ps2plat_fiber_switch, %function
    .align 2
ps2plat_fiber_switch:
    .fnstart
    .cantunwind
    push    {r3-r11, lr}
    vpush   {d8-d15}
    str     sp, [r0]
    mov     sp, r1
    vpop    {d8-d15}
    pop     {r3-r11, pc}
    .fnend
    .size ps2plat_fiber_switch, .-ps2plat_fiber_switch

    .global ps2plat_fiber_start
    .type ps2plat_fiber_start, %function
    .align 2
ps2plat_fiber_start:
    .fnstart
    .cantunwind
    mov     r0, r5
    blx     r4
    bl      abort
    .fnend
    .size ps2plat_fiber_start, .-ps2plat_fiber_start

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack, "", %progbits
#endif
