/**
 * @file fiber_context_aarch64.S
 * @brief AArch64 (ARM64) context switch primitive for Fibers.
 *
 * QUARANTINE — same role as fiber_context_x86_64.cc: implements
 * `fiber_swap_context` in raw asm. Cannot be borrow-checked and
 * cannot be made safe; callers in `reactor.cpp` (`fiber_task_t::resume`,
 * `yield_to_caller`, `entry`) wrap the call site in `// @unsafe`.
 *
 * Mirrors fiber_context_x86_64.cc for ARM64.
 *
 * AAPCS64 callee-saved registers: x19-x28, x29 (fp), x30 (lr), sp.
 * d8-d15 are also callee-saved but omitted (not used by the fiber code).
 *
 * FiberContext layout (must match struct in fiber_impl.h):
 *   [  0]  sp  (void*)
 *   [  8]  pc  (void*) — saved lr on entry, used as return address on resume
 *   [ 16]  x19
 *   [ 24]  x20
 *   [ 32]  x21
 *   [ 40]  x22
 *   [ 48]  x23
 *   [ 56]  x24
 *   [ 64]  x25
 *   [ 72]  x26
 *   [ 80]  x27
 *   [ 88]  x28
 *   [ 96]  fp (x29)
 */

#if defined(__aarch64__)

#if defined(__APPLE__)
/* Mach-O: global symbols have an underscore prefix; no .type/.size. */
.text
.globl _fiber_swap_context
_fiber_swap_context:
    mov  x9,  sp
    str  x9,  [x0, #0]
    str  x30, [x0, #8]
    stp  x19, x20, [x0, #16]
    stp  x21, x22, [x0, #32]
    stp  x23, x24, [x0, #48]
    stp  x25, x26, [x0, #64]
    stp  x27, x28, [x0, #80]
    str  x29, [x0, #96]

    ldp  x19, x20, [x1, #16]
    ldp  x21, x22, [x1, #32]
    ldp  x23, x24, [x1, #48]
    ldp  x25, x26, [x1, #64]
    ldp  x27, x28, [x1, #80]
    ldr  x29, [x1, #96]
    ldr  x30, [x1, #8]
    ldr  x9,  [x1, #0]
    mov  sp,  x9
    ret
#else
/* ELF (Linux): no underscore prefix; .type and .size are valid. */
.text
.globl fiber_swap_context
.type fiber_swap_context, @function
fiber_swap_context:
    mov  x9,  sp
    str  x9,  [x0, #0]
    str  x30, [x0, #8]
    stp  x19, x20, [x0, #16]
    stp  x21, x22, [x0, #32]
    stp  x23, x24, [x0, #48]
    stp  x25, x26, [x0, #64]
    stp  x27, x28, [x0, #80]
    str  x29, [x0, #96]

    ldp  x19, x20, [x1, #16]
    ldp  x21, x22, [x1, #32]
    ldp  x23, x24, [x1, #48]
    ldp  x25, x26, [x1, #64]
    ldp  x27, x28, [x1, #80]
    ldr  x29, [x1, #96]
    ldr  x30, [x1, #8]
    ldr  x9,  [x1, #0]
    mov  sp,  x9
    ret
.size fiber_swap_context, .-fiber_swap_context
#endif /* __APPLE__ */

#endif /* __aarch64__ */
