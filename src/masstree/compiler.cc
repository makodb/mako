/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2014 President and Fellows of Harvard College
 * Copyright (c) 2012-2014 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */
// Assertion and invariant failure handlers
// All functions are @unsafe - they call abort() which terminates the process
//
// @external_unsafe_type: std::*
// @external_unsafe: std::*
// @external_unsafe: fprintf
// @external_unsafe: abort

#include "compiler.hh"
#include <stdio.h>
#include <stdlib.h>
#include <utility>

using c_char = char;

// C++ kernels for the DSL bodies. These stay hand-written because formatted
// stderr output and abort() are intentionally unsafe process-boundary effects.
// @unsafe - calls fprintf() for I/O and abort() which terminates the process
[[noreturn]] static void fail_always_assert_kernel(const char* file, int line,
                                                   const char* assertion, const char* message) {
    if (message)
        fprintf(stderr, "assertion \"%s\" [%s] failed: file \"%s\", line %d\n",
                message, assertion, file, line);
    else
        fprintf(stderr, "assertion \"%s\" failed: file \"%s\", line %d\n",
                assertion, file, line);
    abort();
}

// @unsafe - calls fprintf() for I/O and abort() which terminates the process
[[noreturn]] static void fail_masstree_invariant_kernel(const char* file, int line,
                                                        const char* assertion, const char* message) {
    if (message)
        fprintf(stderr, "invariant \"%s\" [%s] failed: file \"%s\", line %d\n",
                message, assertion, file, line);
    else
        fprintf(stderr, "invariant \"%s\" failed: file \"%s\", line %d\n",
                assertion, file, line);
    abort();
}

// @unsafe - calls fprintf() for I/O and abort() which terminates the process
[[noreturn]] static void fail_masstree_precondition_kernel(const char* file, int line,
                                                           const char* assertion, const char* message) {
    if (message)
        fprintf(stderr, "precondition \"%s\" [%s] failed: file \"%s\", line %d\n",
                message, assertion, file, line);
    else
        fprintf(stderr, "precondition \"%s\" failed: file \"%s\", line %d\n",
                assertion, file, line);
    abort();
}

#if RUSTYCPP_RUST
// The public failure functions are authored in the DSL. Their only behavior is
// delegating to explicit unsafe kernels that do process I/O and terminate.
pub fn fail_always_assert(file: *const c_char, line: i32,
                          assertion: *const c_char, message: *const c_char) {
    unsafe { fail_always_assert_kernel(file, line, assertion, message) }
}

pub fn fail_masstree_invariant(file: *const c_char, line: i32,
                               assertion: *const c_char, message: *const c_char) {
    unsafe { fail_masstree_invariant_kernel(file, line, assertion, message) }
}

pub fn fail_masstree_precondition(file: *const c_char, line: i32,
                                  assertion: *const c_char, message: *const c_char) {
    unsafe { fail_masstree_precondition_kernel(file, line, assertion, message) }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=masstree.compiler.failures version=1 rust_sha256=64d2b19076941fd9af35d06c32cab62d734ef837ab8d108b6a8342bfd9e7b9a4*/
void fail_always_assert(const c_char* file, int32_t line, const c_char* assertion, const c_char* message);
void fail_masstree_invariant(const c_char* file, int32_t line, const c_char* assertion, const c_char* message);
void fail_masstree_precondition(const c_char* file, int32_t line, const c_char* assertion, const c_char* message);

void fail_always_assert(const c_char* file, int32_t line, const c_char* assertion, const c_char* message) {
    // @unsafe
    {
        fail_always_assert_kernel(file, std::move(line), assertion, message);
    }
}

void fail_masstree_invariant(const c_char* file, int32_t line, const c_char* assertion, const c_char* message) {
    // @unsafe
    {
        fail_masstree_invariant_kernel(file, std::move(line), assertion, message);
    }
}

void fail_masstree_precondition(const c_char* file, int32_t line, const c_char* assertion, const c_char* message) {
    // @unsafe
    {
        fail_masstree_precondition_kernel(file, std::move(line), assertion, message);
    }
}
/*RUSTYCPP:GEN-END id=masstree.compiler.failures*/
