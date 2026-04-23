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

// @unsafe - calls fprintf() for I/O and abort() which terminates the process
void fail_always_assert(const char* file, int line,
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
void fail_masstree_invariant(const char* file, int line,
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
void fail_masstree_precondition(const char* file, int line,
                                const char* assertion, const char* message) {
    if (message)
        fprintf(stderr, "precondition \"%s\" [%s] failed: file \"%s\", line %d\n",
                message, assertion, file, line);
    else
        fprintf(stderr, "precondition \"%s\" failed: file \"%s\", line %d\n",
                assertion, file, line);
    abort();
}
