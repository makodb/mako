/* ONE source of truth for the context switch.
 *
 * Rust includes this via global_asm!(include_str!(...)); the C++ build
 * assembles the same file. Neither side owns a copy, so there is no
 * twin to drift.
 *
 * void srpc_fiber_swap(FiberContext *from, FiberContext *to);
 * from = rdi, to = rsi (System V AMD64).
 * Ctx is a single u64: the saved stack pointer.
 */
    .text
    .globl srpc_fiber_swap
    .type srpc_fiber_swap,@function
srpc_fiber_swap:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, (%rdi)      /* from->sp = rsp */
    movq  (%rsi), %rsp      /* rsp = to->sp  */
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret
    .size srpc_fiber_swap,.-srpc_fiber_swap
    .section .note.GNU-stack,"",@progbits
