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
// @unsafe - Circular integer type for epoch and sequence numbers
// Wraps arithmetic for modular comparison (handles overflow correctly)
// SAFETY: Reference-returning operators require lifetime tracking

#ifndef KVDB_CIRCULAR_INT_HH
#define KVDB_CIRCULAR_INT_HH 1
#include "compiler.hh"

// @unsafe - Contains reference-returning operators that require lifetime tracking
template <typename T>
class circular_int {
  public:
    typedef typename mass::make_unsigned<T>::type value_type;
    typedef typename mass::make_signed<T>::type difference_type;

    // @safe - default initialization
    circular_int()
        : v_() {
    }
    // @safe - value initialization
    circular_int(T x)
        : v_(x) {
    }

    // @safe - returns stored value
    value_type value() const {
        return v_;
    }

    // @unsafe
    // @lifetime: (&'a mut) -> &'a mut
    circular_int<T> &operator++() {
        ++v_;
        return *this;
    }
    circular_int<T> operator++(int) {
        ++v_;
        return circular_int<T>(v_ - 1);
    }
    // @unsafe
    // @lifetime: (&'a mut) -> &'a mut
    circular_int<T> &operator--() {
        --v_;
        return *this;
    }
    circular_int<T> operator--(int) {
        --v_;
        return circular_int<T>(v_ + 1);
    }
    // @unsafe
    // @lifetime: (&'a mut, unsigned) -> &'a mut
    circular_int<T> &operator+=(unsigned x) {
        v_ += x;
        return *this;
    }
    // @unsafe
    // @lifetime: (&'a mut, int) -> &'a mut
    circular_int<T> &operator+=(int x) {
        v_ += x;
        return *this;
    }
    // @unsafe
    // @lifetime: (&'a mut, unsigned) -> &'a mut
    circular_int<T> &operator-=(unsigned x) {
        v_ -= x;
        return *this;
    }
    // @unsafe
    // @lifetime: (&'a mut, int) -> &'a mut
    circular_int<T> &operator-=(int x) {
        v_ -= x;
        return *this;
    }

    // @unsafe - performs atomic compare-exchange on raw integral storage
    circular_int<T> cmpxchg(circular_int<T> expected, circular_int<T> desired) {
        return ::cmpxchg(&v_, expected.v_, desired.v_);
    }
    // @unsafe - performs atomic compare-exchange on raw integral storage
    circular_int<T> cmpxchg(T expected, T desired) {
        return ::cmpxchg(&v_, expected, desired);
    }

    typedef value_type (circular_int<T>::*unspecified_bool_type)() const;
    // @safe - value-based bool conversion
    operator unspecified_bool_type() const {
        return v_ != 0 ? &circular_int<T>::value : 0;
    }
    // @safe - value comparison
    bool operator!() const {
        return v_ == 0;
    }

    // @safe - pure arithmetic
    circular_int<T> operator+(unsigned x) const {
        return circular_int<T>(v_ + x);
    }
    // @safe - pure arithmetic
    circular_int<T> operator+(int x) const {
        return circular_int<T>(v_ + x);
    }
    // @unsafe - uses logical NOT that checker interprets as address-of
    circular_int<T> next_nonzero() const {
        value_type v = v_ + 1;
        return circular_int<T>(v + !v);
    }
    // @unsafe - uses logical NOT that checker interprets as address-of
    static value_type next_nonzero(value_type x) {
        ++x;
        return x + !x;
    }
    // @safe - pure arithmetic
    circular_int<T> operator-(unsigned x) const {
        return circular_int<T>(v_ - x);
    }
    // @safe - pure arithmetic
    circular_int<T> operator-(int x) const {
        return circular_int<T>(v_ - x);
    }
    // @safe - pure arithmetic
    difference_type operator-(circular_int<T> x) const {
        return v_ - x.v_;
    }

    // @safe - value comparison
    bool operator==(circular_int<T> x) const {
        return v_ == x.v_;
    }
    // @safe - value comparison
    bool operator!=(circular_int<T> x) const {
        return !(*this == x);
    }
    // @safe - pure arithmetic comparison
    static bool less(value_type a, value_type b) {
        return difference_type(a - b) < 0;
    }
    // @safe - pure arithmetic comparison
    static bool less_equal(value_type a, value_type b) {
        return difference_type(a - b) <= 0;
    }
    // @safe - delegates to safe less()
    bool operator<(circular_int<T> x) const {
        return less(v_, x.v_);
    }
    // @safe - delegates to safe less()
    bool operator<=(circular_int<T> x) const {
        return !less(x.v_, v_);
    }
    // @safe - delegates to safe less()
    bool operator>=(circular_int<T> x) const {
        return !less(v_, x.v_);
    }
    // @safe - delegates to safe less()
    bool operator>(circular_int<T> x) const {
        return less(x.v_, v_);
    }

  private:
    value_type v_;
};

typedef circular_int<uint64_t> kvepoch_t;

// @unsafe - atomic CAS helper on raw circular ints
template <typename T>
inline circular_int<T> cmpxchg(circular_int<T> *object, circular_int<T> expected,
                               circular_int<T> desired) {
    return object->cmpxchg(expected, desired);
}

#endif
