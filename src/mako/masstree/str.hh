/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2013 President and Fellows of Harvard College
 * Copyright (c) 2012-2013 Massachusetts Institute of Technology
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

// RustyCpp Safety Status: str.hh
// This file defines lightweight string views (Str) and inline strings
// Most operations are @safe as they only view existing memory
// Some use @unsafe for pointer casts but are internally sound

#ifndef STR_HH
#define STR_HH
#include "string_base.hh"
#include <stdarg.h>
#include <stdio.h>
namespace lcdf {

struct Str : public String_base<Str> {
    typedef Str substring_type;
    typedef Str argument_type;

    const char *s;
    int len;

    // @safe - Default constructor creates empty string view
    // SAFETY: Initializes to null pointer with zero length
    Str()
        : s(0), len(0) {
    }
    
    // @safe - Copy from String_base views existing data
    // SAFETY: Only copies pointer and length from source string
    template <typename T>
    Str(const String_base<T>& x)
        : s(x.data()), len(x.length()) {
    }
    
    // @safe - C string constructor views existing null-terminated string
    // SAFETY: Uses strlen to compute length, caller ensures s_ is valid
    Str(const char* s_)
        : s(s_), len(strlen(s_)) {
    }
    
    // @safe - Pointer + length constructor creates view
    // SAFETY: Caller ensures s_ points to valid memory of len_ bytes
    Str(const char* s_, int len_)
        : s(s_), len(len_) {
    }
    
    // @unsafe - Uses reinterpret_cast for unsigned char pointer
    // SAFETY: Cast from unsigned char* to char* is safe (same representation)
    //         Caller ensures s_ points to valid memory of len_ bytes
    Str(const unsigned char* s_, int len_)
        : s(reinterpret_cast<const char*>(s_)), len(len_) {
    }
    
    // @safe - Range constructor from pointers
    // SAFETY: Precondition checks first <= last, caller ensures valid range
    Str(const char *first, const char *last)
        : s(first), len(last - first) {
        precondition(first <= last);
    }
    
    // @unsafe - Uses reinterpret_cast for unsigned char pointers
    // SAFETY: Cast from unsigned char* to char* is safe (same representation)
    //         Precondition checks first <= last
    Str(const unsigned char *first, const unsigned char *last)
        : s(reinterpret_cast<const char*>(first)), len(last - first) {
        precondition(first <= last);
    }
    
    // @safe - std::string constructor views existing string data
    // SAFETY: Only copies pointer and length from std::string
    Str(const std::string& str)
        : s(str.data()), len(str.length()) {
    }
    
    // @safe - Uninitialized constructor for optimization
    // SAFETY: Leaves members uninitialized, caller responsible for initialization
    Str(const uninitialized_type &unused) {
        (void) unused;
    }

    static const Str maxkey;

    // @safe - Reset to empty string
    // SAFETY: Sets to null pointer with zero length
    void assign() {
        s = 0;
        len = 0;
    }
    
    // @safe - Assign from String_base copies pointer and length
    // SAFETY: Only copies data pointer and length from source
    template <typename T>
    void assign(const String_base<T> &x) {
        s = x.data();
        len = x.length();
    }
    
    // @safe - Assign from C string
    // SAFETY: Uses strlen to compute length, caller ensures s_ is valid
    void assign(const char *s_) {
        s = s_;
        len = strlen(s_);
    }
    
    // @safe - Assign from pointer and length
    // SAFETY: Caller ensures s_ points to valid memory of len_ bytes
    void assign(const char *s_, int len_) {
        s = s_;
        len = len_;
    }

    // @safe - Returns const pointer to string data
    // SAFETY: Returns stored const pointer, no ownership transfer
    const char *data() const {
        return s;
    }
    
    // @safe - Returns string length
    // SAFETY: Returns stored integer value
    int length() const {
        return len;
    }
    
    // @unsafe - Casts away const to return mutable pointer
    // SAFETY: Uses const_cast which removes const qualifier
    //         Caller must ensure underlying memory is actually mutable
    char* mutable_data() {
        return const_cast<char*>(s);
    }

    // @safe - Returns prefix substring up to lenx characters
    // SAFETY: Clamps lenx to actual length, returns new Str view
    Str prefix(int lenx) const {
        return Str(s, lenx < len ? lenx : len);
    }
    
    // @safe - Returns substring with bounds checking
    // SAFETY: Validates first/last are within string bounds before creating view
    Str substring(const char *first, const char *last) const {
        if (first <= last && first >= s && last <= s + len)
            return Str(first, last);
        else
            return Str();
    }
    
    // @unsafe - Uses reinterpret_cast for unsigned char pointers
    // SAFETY: Cast from char* to unsigned char* is safe (same representation)
    //         Validates bounds before creating substring view
    Str substring(const unsigned char *first, const unsigned char *last) const {
        const unsigned char *u = reinterpret_cast<const unsigned char*>(s);
        if (first <= last && first >= u && last <= u + len)
            return Str(first, last);
        else
            return Str();
    }
    
    // @safe - Fast substring without bounds checking (debug assert only)
    // SAFETY: Assert checks bounds in debug builds, caller ensures valid range
    Str fast_substring(const char *first, const char *last) const {
        assert(begin() <= first && first <= last && last <= end());
        return Str(first, last);
    }
    
    // @safe - Fast substring without bounds checking (debug assert only)
    // SAFETY: Assert checks bounds in debug builds, caller ensures valid range
    Str fast_substring(const unsigned char *first, const unsigned char *last) const {
        assert(ubegin() <= first && first <= last && last <= uend());
        return Str(first, last);
    }
    
    // @safe - Trim whitespace from left
    // SAFETY: Delegates to String_generic::ltrim which performs safe operations
    Str ltrim() const {
        return String_generic::ltrim(*this);
    }
    
    // @safe - Trim whitespace from right
    // SAFETY: Delegates to String_generic::rtrim which performs safe operations
    Str rtrim() const {
        return String_generic::rtrim(*this);
    }
    
    // @safe - Trim whitespace from both ends
    // SAFETY: Delegates to String_generic::trim which performs safe operations
    Str trim() const {
        return String_generic::trim(*this);
    }

    // @safe - Parse string to integer (returns -1 on failure)
    // SAFETY: Only reads from string buffer within bounds (p < len)
    //         Returns -1 for invalid input, does not handle negative numbers
    long to_i() const {         // XXX does not handle negative
        long x = 0;
        int p;
        for (p = 0; p < len && s[p] >= '0' && s[p] <= '9'; ++p)
            x = (x * 10) + s[p] - '0';
        return p == len && p != 0 ? x : -1;
    }

    // @unsafe - Uses variadic arguments and vsnprintf
    // SAFETY: vsnprintf is a C library function that writes to buf
    //         Caller must ensure buf has size bytes available
    //         Returns Str view of formatted string in buf
    static Str snprintf(char *buf, size_t size, const char *fmt, ...) {
        va_list val;
        va_start(val, fmt);
        int n = vsnprintf(buf, size, fmt, val);
        va_end(val);
        return Str(buf, n);
    }
};

// inline_string: zero-length array for flexible string storage
struct inline_string : public String_base<inline_string> {
    int len;
    char s[0];  // flexible array member

    // @safe - Returns const pointer to inline string data
    // SAFETY: s is a flexible array member, returns pointer to inline storage
    const char *data() const {
        return s;
    }
    
    // @safe - Returns string length
    // SAFETY: Returns stored length value
    int length() const {
        return len;
    }

    // @safe - Returns total size including string data
    // SAFETY: Computes size as struct size plus string length
    size_t size() const {
        return sizeof(inline_string) + len;
    }
    
    // @safe - Static helper to compute size for given length
    // SAFETY: Pure computation, no memory access
    static size_t size(int len) {
        return sizeof(inline_string) + len;
    }
};

} // namespace lcdf

LCDF_MAKE_STRING_HASH(lcdf::Str)
LCDF_MAKE_STRING_HASH(lcdf::inline_string)
#endif
