/**
 * @file fiber_context_x86_64.S
 * @brief x86_64 SysV context switch primitive for Fibers.
 *
 * QUARANTINE — the deepest @unsafe component of the fiber runtime.
 * `fiber_swap_context` saves the SysV-callee-saved register set into a
 * `FiberContext` struct pointed to by %rdi and restores from another
 * pointed to by %rsi. It cannot be borrow-checked (the analyzer does
 * not look at asm) and cannot be made safe (the register save/restore
 * IS the unsafe operation). Callers — `fiber_task_t::resume()`,
 * `::yield_to_caller()`, `::entry()` in reactor.cpp — wrap the call
 * site in `// @unsafe` and document the unsafety at that boundary.
 * New callers should follow the same pattern.
 *
 * Was `fiber_context_x86_64.cc`: the identical assembly wrapped in a
 * top-level `asm(R"(...)")` inside a plain C++ TU. As a real `.S` it
 * is preprocessed the same way (so the arch guard still works), gets
 * actual assembler syntax checking instead of living in an unchecked
 * raw string, and leaves the hand-written-C++ census entirely.
 */

#if defined(__x86_64__)
.text
.globl fiber_swap_context
.type fiber_swap_context, @function
fiber_swap_context:
    # Save current context (*from in %rdi).
    movq %rsp, 0(%rdi)
    leaq .Lfiber_resume(%rip), %rax
    movq %rax, 8(%rdi)
    movq %rbx, 16(%rdi)
    movq %rbp, 24(%rdi)
    movq %r12, 32(%rdi)
    movq %r13, 40(%rdi)
    movq %r14, 48(%rdi)
    movq %r15, 56(%rdi)

    # Restore target context (*to in %rsi).
    movq 16(%rsi), %rbx
    movq 24(%rsi), %rbp
    movq 32(%rsi), %r12
    movq 40(%rsi), %r13
    movq 48(%rsi), %r14
    movq 56(%rsi), %r15
    movq 0(%rsi), %rsp
    movq 8(%rsi), %rax
    jmp *%rax

.Lfiber_resume:
    ret

.size fiber_swap_context, .-fiber_swap_context
#endif
