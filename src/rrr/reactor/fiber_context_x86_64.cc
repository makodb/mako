/**
 * @file fiber_context_x86_64.cc
 * @brief x86_64 SysV context switch primitive for Fibers.
 *
 * QUARANTINE — this file is the deepest @unsafe component of the
 * fiber runtime. It implements `fiber_swap_context` as a raw inline
 * assembly routine that saves the SysV-callee-saved register set into
 * a `FiberContext` struct pointed to by `%rdi` and restores from
 * another struct pointed to by `%rsi`. It cannot be borrow-checked
 * (the analyzer doesn't look at asm) and cannot be made safe (the
 * register save/restore IS the unsafe operation). Callers — in
 * particular `fiber_task_t::resume()`, `fiber_task_t::yield_to_caller()`,
 * and `fiber_task_t::entry()` in `reactor.cpp` — wrap the
 * `fiber_swap_context(...)` call site in `// @unsafe` annotations and
 * document the unsafety at that boundary. New callers should follow
 * the same pattern.
 *
 * Plain C++ translation unit (NOT a module impl partition). Top-level asm()
 * inside a module impl partition is treated as non-reachable by clang and
 * the symbol never makes it into the .o file.
 */

#if defined(__x86_64__)
asm(R"(
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
)");
#endif
