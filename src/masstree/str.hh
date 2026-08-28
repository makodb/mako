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
// Non-owning string slice (pointer + length)
// Lightweight view into string data without memory management
// SAFETY: Does not own data - caller must ensure lifetime validity

#ifndef STR_HH
#define STR_HH
#include "string_base.hh"
#include <stdarg.h>
#include <stdio.h>
// @unsafe: entire lcdf namespace - contains raw pointer operations
namespace lcdf {

struct Str : public String_base<Str> {
    typedef Str substring_type;
    typedef Str argument_type;

    const char *s;
    int len;

    Str()
        : Str(empty()) {
    }
    template <typename T>
    Str(const String_base<T>& x)
        : Str(from_string_base(x)) {
    }
    Str(const char* s_)
        : Str(from_cstring(s_)) {
    }
    Str(const char* s_, int len_)
        : s(s_), len(len_) {
    }
    Str(const unsigned char* s_, int len_)
        : Str(from_bytes(s_, len_)) {
    }
    Str(const char *first, const char *last)
        : Str(from_range(first, last)) {
    }
    Str(const unsigned char *first, const unsigned char *last)
        : Str(from_byte_range(first, last)) {
    }
    Str(const std::string& str)
        : Str(from_std_string(str)) {
    }
    Str(const uninitialized_type &unused) {
        (void) unused;
    }

    static Str empty() {
        return Str(static_cast<const char*>(0), 0);
    }
    template <typename T>
    static Str from_string_base(const String_base<T>& x) {
        return from_chars(x.data(), x.length());
    }
    static Str from_cstring(const char* s_) {
        return from_chars(s_, strlen(s_));
    }
    static Str from_chars(const char* s_, int len_) {
        return Str(s_, len_);
    }
    static Str from_bytes(const unsigned char* s_, int len_) {
        return from_chars(reinterpret_cast<const char*>(s_), len_);
    }
    static Str from_range(const char *first, const char *last) {
        precondition(first <= last);
        return from_chars(first, last - first);
    }
    static Str from_byte_range(const unsigned char *first, const unsigned char *last) {
        precondition(first <= last);
        return from_chars(reinterpret_cast<const char*>(first), last - first);
    }
    static Str from_std_string(const std::string& str) {
        return from_chars(str.data(), str.length());
    }
    static Str from_uninitialized(const uninitialized_type &unused) {
        (void) unused;
        return Str(uninitialized_type());
    }

    static const Str maxkey;

    void assign() {
        s = 0;
        len = 0;
    }
    // @unsafe
    template <typename T> void assign(const String_base<T> &x) {
        s = x.data();
        len = x.length();
    }
    // @unsafe - stores raw C string pointer without ownership
    void assign(const char *s_) {
        s = s_;
        len = strlen(s_);
    }
    // @unsafe - stores raw pointer/length without validation
    void assign(const char *s_, int len_) {
        s = s_;
        len = len_;
    }

    const char *data() const {
        return s;
    }
    int length() const {
        return len;
    }
    char* mutable_data() {
        return const_cast<char*>(s);
    }

    Str prefix(int lenx) const {
        return Str::from_chars(s, lenx < len ? lenx : len);
    }
    Str substring(const char *first, const char *last) const {
        if (first <= last && first >= s && last <= s + len)
            return Str::from_range(first, last);
        else
            return Str::empty();
    }
    Str substring(const unsigned char *first, const unsigned char *last) const {
        const unsigned char *u = reinterpret_cast<const unsigned char*>(s);
        if (first <= last && first >= u && last <= u + len)
            return Str::from_byte_range(first, last);
        else
            return Str::empty();
    }
    Str fast_substring(const char *first, const char *last) const {
        assert(begin() <= first && first <= last && last <= end());
        return Str::from_range(first, last);
    }
    Str fast_substring(const unsigned char *first, const unsigned char *last) const {
        assert(ubegin() <= first && first <= last && last <= uend());
        return Str::from_byte_range(first, last);
    }
    // @unsafe - pointer arithmetic on string data
    Str ltrim() const {
        return String_generic::ltrim(*this);
    }
    // @unsafe - pointer arithmetic on string data
    Str rtrim() const {
        return String_generic::rtrim(*this);
    }
    // @unsafe - pointer arithmetic on string data
    Str trim() const {
        return String_generic::trim(*this);
    }

    long to_i() const {         // XXX does not handle negative
        long x = 0;
        int p;
        for (p = 0; p < len && s[p] >= '0' && s[p] <= '9'; ++p)
            x = (x * 10) + s[p] - '0';
        return p == len && p != 0 ? x : -1;
    }

    static Str snprintf(char *buf, size_t size, const char *fmt, ...) {
        va_list val;
        va_start(val, fmt);
        int n = vsnprintf(buf, size, fmt, val);
        va_end(val);
        return Str::from_chars(buf, n);
    }
};

struct inline_string : public String_base<inline_string> {
    int len;
    char s[0];

    const char *data() const {
        return s;
    }
    int length() const {
        return len;
    }

    size_t size() const {
        return sizeof(inline_string) + len;
    }
    static size_t size(int len) {
        return sizeof(inline_string) + len;
    }
};

} // namespace lcdf

LCDF_MAKE_STRING_HASH(lcdf::Str)
LCDF_MAKE_STRING_HASH(lcdf::inline_string)
#endif
