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
// @unsafe - Buffered I/O structures for network communication
// Provides kvout buffer management for serialized key-value protocol
// SAFETY: Raw buffer allocation, file descriptor management
// @external_unsafe: assign

#ifndef KVIO_H
#define KVIO_H
#include <string>
#include <vector>
#include <stdlib.h>
#include "string.hh"
#include "str.hh"

struct kvout {
    int fd;
    char* buf;
    unsigned capacity; // allocated size of buf
    unsigned n;   // # of chars we've written to buf

    // @unsafe - appends into raw buffer, calls grow() which may reallocate
    inline void append(char c);
    // @unsafe - returns pointer into raw buffer that may be invalidated by reallocation
    inline char* reserve(int n);
    // @safe - pure arithmetic on length field with precondition check
    inline void adjust_length(int delta);
    // @safe - pure pointer arithmetic to update length field
    inline void set_end(char* end);
    // @unsafe - calls realloc() which may invalidate existing pointers
    void grow(unsigned want);
};

kvout* new_kvout(int fd, int buflen);
kvout* new_bufkvout();
// @safe - resets buffer position, no allocation or pointer operations
void kvout_reset(kvout& kv);
void free_kvout(kvout* kv);
int kvwrite(kvout* kv, const void* buf, unsigned int n);
void kvflush(kvout* kv);

// @unsafe - may call grow() which reallocates
inline void kvout::append(char c) {
    if (n == capacity)
        grow(0);
    buf[n] = c;
    ++n;
}

// @unsafe - returns pointer into buffer that may be invalidated
inline char* kvout::reserve(int nchars) {
    if (n + nchars > capacity)
        grow(n + nchars);
    return buf + n;
}

// @safe - pure arithmetic with precondition check, no allocation or pointer operations
inline void kvout::adjust_length(int delta) {
    masstree_precondition(n + delta <= capacity);
    n += delta;
}

// @safe - pure pointer arithmetic with bounds check, no allocation
inline void kvout::set_end(char* x) {
    masstree_precondition(x >= buf && x <= buf + capacity);
    n = x - buf;
}

#endif
