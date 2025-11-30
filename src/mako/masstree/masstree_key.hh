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
// @unsafe - Key adapter for Masstree with 64-bit slice comparison
// Converts variable-length keys to fixed-width slices for efficient comparison
// SAFETY: Uses memcpy, big-endian byte swapping, and unchecked slicing

#ifndef MASSTREE_KEY_HH
#define MASSTREE_KEY_HH
#include "masstree.hh"
#include "string_slice.hh"

namespace Masstree {

/** @brief Strings used as Masstree keys.

    Masstree key strings are divided into parts: an initial slice, called
    the <em>ikey</em>, and a suffix. The ikey is stored as a byte-swapped
    integer, which is faster to compare than a string.

    Keys can be <em>shifted</em> by the shift() method. <code>k.shift()</code>
    is like <code>k = key(k.suffix())</code>: the key is reconstructed from
    the original key's suffix. This corresponds to descending a layer in
    Masstree. However, <code>k.shift()</code> is faster than the assignment,
    and its effects can be undone by the <code>k.unshift_all()</code>
    method. */
template <typename I>
class key {
  public:
    static constexpr int nikey = 1;
    /** @brief Type of ikeys. */
    typedef I ikey_type;
    /** @brief Size of ikeys in bytes. */
    static constexpr int ikey_size = sizeof(ikey_type);

    // @safe - default construction
    /** @brief Construct an uninitialized key. */
    key() {
    }
    // @safe - initializes from string view
    /** @brief Construct a key for string @a s. */
    key(Str s)
        : ikey0_(string_slice<ikey_type>::make_comparable(s.s, s.len)),
          len_(s.len), s_(s.s), first_(s.s) {
    }
    // @safe - initializes from buffer and length
    /** @brief Construct a key for string @a s with length @a len.
        @pre @a len >= 0 */
    key(const char* s, int len)
        : ikey0_(string_slice<ikey_type>::make_comparable(s, len)),
          len_(len), s_(s), first_(s) {
    }
    // @safe - initializes from ikey value
    /** @brief Construct a key for ikey @a ikey.

        Any trailing zero bytes in @a ikey are not counted towards the key's
        length. */
    explicit key(ikey_type ikey)
        : ikey0_(ikey),
          len_(ikey ? ikey_size - ctz(ikey) / 8 : 0), s_(0), first_(0) {
    }
    // @safe - initializes from ikey value and length
    /** @brief Construct a key for ikey @a ikey with length @a len.
        @pre @a len >= 0
        @post length() >= 0 && length() <= ikey_size */
    key(ikey_type ikey, int len)
        : ikey0_(ikey),
          len_(std::min(len, ikey_size)), s_(0), first_(0) {
    }
    // @safe - initializes from ikey and suffix
    /** @brief Construct a key with ikey @a ikey and suffix @a suf. */
    key(ikey_type ikey, Str suf)
        : ikey0_(ikey),
          len_(ikey_size + suf.len), s_(suf.s - ikey_size), first_(s_) {
    }

    // @safe - value comparison
    /** @brief Test if this key is empty (holds the empty string). */
    bool empty() const {
        return ikey0_ == 0 && len_ == 0;
    }
    // @safe - returns stored value
    /** @brief Return the ikey. */
    ikey_type ikey() const {
        return ikey0_;
    }
    // @safe - returns stored value
    /** @brief Return the key's length. */
    int length() const {
        return len_;
    }
    // @safe - value comparison
    /** @brief Test whether this key has a suffix (length() > ikey_size). */
    bool has_suffix() const {
        return len_ > ikey_size;
    }
    // @safe - returns suffix view
    /** @brief Return this key's suffix.
        @pre has_suffix() */
    Str suffix() const {
        return Str(s_ + ikey_size, len_ - ikey_size);
    }
    // @safe - pure arithmetic
    /** @brief Return the length of this key's suffix.
        @pre has_suffix() */
    int suffix_length() const {
        return len_ - ikey_size;
    }

    /** @brief Shift this key forward to model the current key's suffix.
        @pre has_suffix() */
    // @unsafe - performs unchecked pointer arithmetic into raw key buffer
    void shift() {
        s_ += ikey_size;
        len_ -= ikey_size;
        ikey0_ = string_slice<ikey_type>::make_comparable_sloppy(s_, len_);
    }
    /** @brief Shift this key forward to model the current key's suffix.
        @pre has_suffix() */
    // @unsafe - adjusts raw suffix pointer
    void shift_by(int delta) {
        s_ += delta;
        len_ -= delta;
        ikey0_ = string_slice<ikey_type>::make_comparable_sloppy(s_, len_);
    }
    // @safe - pointer comparison
    /** @brief Test whether this key has been shifted by shift(). */
    bool is_shifted() const {
        return first_ != s_;
    }
    // @safe - restores pointers to original positions
    /** @brief Undo all previous shift() calls. */
    void unshift_all() {
        if (s_ != first_) {
            len_ += s_ - first_;
            s_ = first_;
            ikey0_ = string_slice<ikey_type>::make_comparable(s_, len_);
        }
    }

    // @safe - pure comparison
    int compare(ikey_type ikey, int keylenx) const {
        int cmp = ::compare(this->ikey(), ikey);
        if (cmp == 0) {
            int al = this->length();
            if (al > ikey_size)
                cmp = keylenx <= ikey_size;
            else
                cmp = al - keylenx;
        }
        return cmp;
    }
    // @safe - delegates to compare
    int compare(const key<I>& x) const {
        return compare(x.ikey(), x.length());
    }

    // @unsafe - writes to raw buffer
    int unparse(char* data, int datalen) const {
        int cplen = std::min(len_, datalen);
        string_slice<ikey_type>::unparse_comparable(data, cplen, ikey0_, ikey_size);
        if (cplen > ikey_size)
            memcpy(data + ikey_size, s_ + ikey_size, cplen - ikey_size);
        return cplen;
    }
    // @unsafe - constructs mutable buffer and forwards to raw unparse
    String unparse() const {
        String s = String::make_uninitialized(len_);
        unparse(s.mutable_data(), s.length());
        return s;
    }
    // @unsafe - writes to raw buffer
    int unparse_printable(char* data, int datalen) const {
        String s = unparse().printable();
        int cplen = std::min(s.length(), datalen);
        memcpy(data, s.data(), cplen);
        return cplen;
    }
    // @safe - creates new String
    static String unparse_ikey(ikey_type ikey) {
        key<ikey_type> k(ikey);
        return k.unparse();
    }

    // used during scan
    // @safe - returns string view
    Str prefix_string() const {
        return Str(first_, s_);
    }
    // @safe - pointer arithmetic
    int prefix_length() const {
        return s_ - first_;
    }
    // @safe - returns string view
    Str full_string() const {
        return Str(first_, s_ + len_);
    }
    // @safe - conversion to Str
    operator Str() const {
        return full_string();
    }
    // @safe - modifies owned values
    bool increment() {
        // Return true iff wrapped.
        if (has_suffix()) {
            ++ikey0_;
            len_ = 1;
            return unlikely(!ikey0_);
        } else {
            ++len_;
            return false;
        }
    }
    // @unsafe - writes through raw pointer
    void assign_store_ikey(ikey_type ikey) {
        ikey0_ = ikey;
        *reinterpret_cast<ikey_type*>(const_cast<char*>(s_)) = host_to_net_order(ikey);
    }
    // @unsafe - memcpy to raw buffer
    int assign_store_suffix(Str s) {
        memcpy(const_cast<char*>(s_ + ikey_size), s.s, s.len);
        return ikey_size + s.len;
    }
    // @safe - value assignment
    void assign_store_length(int len) {
        len_ = len;
    }
    // @unsafe - pointer arithmetic without bounds check
    void unshift() {
        masstree_precondition(is_shifted());
        s_ -= ikey_size;
        ikey0_ = string_slice<ikey_type>::make_comparable_sloppy(s_, ikey_size);
        len_ = ikey_size + 1;
    }
    // @unsafe - pointer arithmetic
    void shift_clear() {
        ikey0_ = 0;
        len_ = 0;
        s_ += ikey_size;
    }
    // @unsafe - pointer arithmetic
    void shift_clear_reverse() {
        ikey0_ = ~ikey_type(0);
        len_ = ikey_size + 1;
        s_ += ikey_size;
    }

  private:
    ikey_type ikey0_;
    int len_;
    const char* s_;
    const char* first_;
};

template <typename I> constexpr int key<I>::ikey_size;

} // namespace Masstree

template <typename I>
inline std::ostream& operator<<(std::ostream& stream,
                                const Masstree::key<I>& x) {
    return stream << x.unparse();
}

#endif
