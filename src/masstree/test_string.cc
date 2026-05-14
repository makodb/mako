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
// String encoding conversion tests
//
// @external_unsafe_type: std::*
// @external_unsafe: std::*
// @external_unsafe: lcdf::String::*
// @external_unsafe: lcdf::StringAccum::*

#include "string.hh"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "straccum.hh"

using lcdf::String;
using lcdf::StringAccum;

namespace Encoding {

struct UTF8 {};
struct UTF8NoNul {};
struct Windows1252 {};

template <typename T>
struct Converter;

template <>
struct Converter<UTF8> {
    static String to_utf8(const char* in, int inlen) {
        return String(in, inlen);
    }
};

template <>
struct Converter<UTF8NoNul> {
    static String to_utf8(const char* in, int inlen) {
        StringAccum filtered;
        for (int i = 0; i < inlen; ++i) {
            if (in[i] != '\0') {
                filtered.append(&in[i], 1);
            }
        }
        return filtered.take_string();
    }
};

template <>
struct Converter<Windows1252> {
    static String to_utf8(const char* in, int inlen) {
        return String(in, inlen).windows1252_to_utf8();
    }
};

template <typename T>
String to_utf8(const char* in, int inlen) {
    return Converter<T>::to_utf8(in, inlen);
}

}  // namespace Encoding

// @unsafe - exercises encoding conversions using raw C strings and unchecked buffers
template <typename T>
static bool
check_straccum_utf8(StringAccum &sa, const char *in, int inlen,
                    const char *out, int outlen)
{
    sa.clear();
    String encoded = Encoding::to_utf8<T>(in, inlen);
    sa.append(encoded.data(), encoded.length());
    return sa.length() == outlen && memcmp(sa.begin(), out, sa.length()) == 0;
}

template <typename T>
static bool
check_straccum2_utf8(StringAccum &sa, const char *in, int inlen,
                     const char *out, int outlen)
{
    sa.clear();
    String encoded = Encoding::to_utf8<T>(in, inlen);
    sa.append(encoded.data(), encoded.length());
    return sa.length() == outlen && memcmp(sa.begin(), out, sa.length()) == 0;
}

int
main(int argc, char *argv[])
{
    assert(String("abc").to_utf8() == "abc");
    assert(String("").to_utf8() == "");
    assert(String("ab\000cd", 5).to_utf8() == "abcd");
    assert(String("\xc3\x9dHi!").to_utf8() == "\xc3\x9dHi!");
    assert(String("\xddHi!").to_utf8() == "\xc3\x9dHi!");
    assert(String("\xc3\x9dHi!\x9c").to_utf8() == "\xc3\x9dHi!\xc5\x93");
    assert(String("ab\000c\x9c", 5).to_utf8() == "abc\xc5\x93");
    assert(String("\xc3\x9dXY\000c\x9c", 7).to_utf8() == "\xc3\x9dXYc\xc5\x93");

    StringAccum sa;
    check_straccum_utf8<Encoding::UTF8>(sa, "abc", 3, "abc", 3);
    check_straccum_utf8<Encoding::UTF8>(sa, "", 0, "", 0);
    check_straccum_utf8<Encoding::UTF8>(sa, "ab\000cd", 5, "ab\000cd", 5);
    check_straccum_utf8<Encoding::UTF8NoNul>(sa, "ab\000cd", 5, "abcd", 4);
    check_straccum_utf8<Encoding::UTF8>(sa, "\xc3\x9dHi!", 5, "\xc3\x9dHi!", 5);
    check_straccum_utf8<Encoding::Windows1252>(sa, "\xddHi!", 4, "\xc3\x9dHi!", 5);

    check_straccum2_utf8<Encoding::UTF8>(sa, "abc", 3, "abc", 3);
    check_straccum2_utf8<Encoding::UTF8>(sa, "", 0, "", 0);
    check_straccum2_utf8<Encoding::UTF8>(sa, "ab\000cd", 5, "ab\000cd", 5);
    check_straccum2_utf8<Encoding::UTF8NoNul>(sa, "ab\000cd", 5, "abcd", 4);
    check_straccum2_utf8<Encoding::UTF8>(sa, "\xc3\x9dHi!", 5, "\xc3\x9dHi!", 5);
    check_straccum2_utf8<Encoding::Windows1252>(sa, "\xddHi!", 4, "\xc3\x9dHi!", 5);

    if (argc == 2) {
        FILE *f;
        if (strcmp(argv[1], "-") == 0)
            f = stdin;
        else if (!(f = fopen(argv[1], "rb"))) {
            perror("test_string");
            exit(1);
        }
        StringAccum sa;
        while (!feof(f)) {
            size_t x = fread(sa.reserve(1024), 1, 1024, f);
            sa.adjust_length(x);
        }
        String s = sa.take_string().to_utf8(String::utf_strip_bom);
        fwrite(s.data(), 1, s.length(), stdout);
    }
}
