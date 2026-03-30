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
// @unsafe - Optimistic versioned node locking for concurrent tree access
// Implements sequence locks with split detection for lock-free reads
// SAFETY: Uses atomic CAS, memory fences, and spinning for write locks

#ifndef MASSTREE_NODEVERSION_HH
#define MASSTREE_NODEVERSION_HH
#include "compiler.hh"

template <typename P>
class nodeversion {
  public:
    typedef P traits_type;
    typedef typename P::value_type value_type;

    // @safe - default construction
    nodeversion() {
    }
    // @safe - value initialization
    explicit nodeversion(bool isleaf) {
        v_ = isleaf ? (value_type) P::isleaf_bit : 0;
    }

    // @safe - bit extraction
    bool isleaf() const {
        return v_ & P::isleaf_bit;
    }

    // @unsafe - calls fence functions
    nodeversion<P> stable() const {
        return stable(relax_fence_function());
    }
    // @unsafe - calls fence functions and spin_function
    template <typename SF>
    nodeversion<P> stable(SF spin_function) const {
        value_type x = v_;
        while (x & P::dirty_mask) {
            spin_function();
            x = v_;
        }
        acquire_fence();
        return x;
    }
    // @unsafe - calls fence functions and spin_function
    template <typename SF>
    nodeversion<P> stable_annotated(SF spin_function) const {
        value_type x = v_;
        while (x & P::dirty_mask) {
            spin_function(nodeversion<P>(x));
            x = v_;
        }
        acquire_fence();
        return x;
    }

    // @safe - bit extraction
    bool locked() const {
        return v_ & P::lock_bit;
    }
    // @safe - bit extraction
    bool inserting() const {
        return v_ & P::inserting_bit;
    }
    // @safe - bit extraction
    bool splitting() const {
        return v_ & P::splitting_bit;
    }
    // @safe - bit extraction
    bool deleted() const {
        return v_ & P::deleted_bit;
    }
    // @unsafe - calls fence()
    bool has_changed(nodeversion<P> x) const {
        fence();
        return (x.v_ ^ v_) > P::lock_bit;
    }
    // @safe - bit extraction
    bool is_root() const {
        return v_ & P::root_bit;
    }
    // @unsafe - calls fence()
    bool has_split(nodeversion<P> x) const {
        fence();
        return (x.v_ ^ v_) >= P::vsplit_lowbit;
    }
    // @safe - bit comparison
    bool simple_has_split(nodeversion<P> x) const {
        return (x.v_ ^ v_) >= P::vsplit_lowbit;
    }

    // @unsafe - spins/CAS on raw version words
    nodeversion<P> lock() {
        return lock(*this);
    }
    nodeversion<P> lock(nodeversion<P> expected) {
        return lock(expected, relax_fence_function());
    }
    template <typename SF>
    nodeversion<P> lock(nodeversion<P> expected, SF spin_function) {
        while (1) {
            if (!(expected.v_ & P::lock_bit)
                && bool_cmpxchg(&v_, expected.v_,
                                expected.v_ | P::lock_bit))
                break;
            spin_function();
            expected.v_ = v_;
        }
        masstree_invariant(!(expected.v_ & P::dirty_mask));
        expected.v_ |= P::lock_bit;
        acquire_fence();
        masstree_invariant(expected.v_ == v_);
        return expected;
    }

    // @unsafe - releases raw lock bits and mutates version
    void unlock() {
        unlock(*this);
    }
    void unlock(nodeversion<P> x) {
        masstree_invariant((fence(), x.v_ == v_));
        masstree_invariant(x.v_ & P::lock_bit);
        if (x.v_ & P::splitting_bit)
            x.v_ = (x.v_ + P::vsplit_lowbit) & P::split_unlock_mask;
        else
            x.v_ = (x.v_ + ((x.v_ & P::inserting_bit) << 2)) & P::unlock_mask;
        release_fence();
        v_ = x.v_;
    }

    // @unsafe - calls acquire_fence()
    void mark_insert() {
        masstree_invariant(locked());
        v_ |= P::inserting_bit;
        acquire_fence();
    }
    // @unsafe - calls fence() and acquire_fence()
    nodeversion<P> mark_insert(nodeversion<P> current_version) {
        masstree_invariant((fence(), v_ == current_version.v_));
        masstree_invariant(current_version.v_ & P::lock_bit);
        v_ = (current_version.v_ |= P::inserting_bit);
        acquire_fence();
        return current_version;
    }
    // @unsafe - calls acquire_fence()
    void mark_split() {
        masstree_invariant(locked());
        v_ |= P::splitting_bit;
        acquire_fence();
    }
    // @unsafe - calls acquire_fence()
    void mark_change(bool is_split) {
        masstree_invariant(locked());
        v_ |= (is_split + 1) << P::inserting_shift;
        acquire_fence();
    }
    // @unsafe - calls acquire_fence() and returns *this
    nodeversion<P> mark_deleted() {
        masstree_invariant(locked());
        v_ |= P::deleted_bit | P::splitting_bit;
        acquire_fence();
        return *this;
    }
    // @unsafe - calls acquire_fence()
    void mark_deleted_tree() {
        masstree_invariant(locked() && is_root());
        v_ |= P::deleted_bit;
        acquire_fence();
    }
    // @unsafe - calls acquire_fence()
    void mark_root() {
        v_ |= P::root_bit;
        acquire_fence();
    }
    // @unsafe - calls acquire_fence()
    void mark_nonroot() {
        v_ &= ~P::root_bit;
        acquire_fence();
    }

    // @safe - value assignment
    void assign_version(nodeversion<P> x) {
        v_ = x.v_;
    }

    // @safe - returns stored value
    value_type version_value() const {
        return v_;
    }
    // @safe - bit extraction
    value_type unlocked_version_value() const {
        return v_ & P::unlock_mask;
    }

  private:
    value_type v_;

    nodeversion(value_type v)
        : v_(v) {
    }
};


template <typename P>
class singlethreaded_nodeversion {
  public:
    typedef P traits_type;
    typedef typename P::value_type value_type;

    // @safe - default construction
    singlethreaded_nodeversion() {
    }
    // @safe - value initialization
    explicit singlethreaded_nodeversion(bool isleaf) {
        v_ = isleaf ? (value_type) P::isleaf_bit : 0;
    }

    // @safe - bit extraction
    bool isleaf() const {
        return v_ & P::isleaf_bit;
    }

    // @unsafe - returns *this without lifetime tracking
    singlethreaded_nodeversion<P> stable() const {
        return *this;
    }
    // @unsafe - returns *this without lifetime tracking
    template <typename SF>
    singlethreaded_nodeversion<P> stable(SF) const {
        return *this;
    }
    // @unsafe - returns *this without lifetime tracking
    template <typename SF>
    singlethreaded_nodeversion<P> stable_annotated(SF) const {
        return *this;
    }

    // @safe - always returns false in singlethreaded mode
    bool locked() const {
        return false;
    }
    // @safe - always returns false in singlethreaded mode
    bool inserting() const {
        return false;
    }
    // @safe - always returns false in singlethreaded mode
    bool splitting() const {
        return false;
    }
    // @safe - always returns false in singlethreaded mode
    bool deleted() const {
        return false;
    }
    // @safe - always returns false in singlethreaded mode
    bool has_changed(singlethreaded_nodeversion<P>) const {
        return false;
    }
    // @safe - bit extraction
    bool is_root() const {
        return v_ & P::root_bit;
    }
    // @safe - always returns false in singlethreaded mode
    bool has_split(singlethreaded_nodeversion<P>) const {
        return false;
    }
    // @safe - always returns false in singlethreaded mode
    bool simple_has_split(singlethreaded_nodeversion<P>) const {
        return false;
    }

    // @unsafe - returns *this without lifetime tracking
    singlethreaded_nodeversion<P> lock() {
        return *this;
    }
    // @unsafe - returns *this without lifetime tracking
    singlethreaded_nodeversion<P> lock(singlethreaded_nodeversion<P>) {
        return *this;
    }
    // @unsafe - returns *this without lifetime tracking
    template <typename SF>
    singlethreaded_nodeversion<P> lock(singlethreaded_nodeversion<P>, SF) {
        return *this;
    }

    // @safe - no-op in singlethreaded mode
    void unlock() {
    }
    // @safe - no-op in singlethreaded mode
    void unlock(singlethreaded_nodeversion<P>) {
    }

    // @safe - no-op in singlethreaded mode
    void mark_insert() {
    }
    // @unsafe - returns *this without lifetime tracking
    singlethreaded_nodeversion<P> mark_insert(singlethreaded_nodeversion<P>) {
        return *this;
    }
    // @safe - bit manipulation
    void mark_split() {
        v_ &= ~P::root_bit;
    }
    // @safe - delegates to mark_split
    void mark_change(bool is_split) {
        if (is_split)
            mark_split();
    }
    // @unsafe - returns *this without lifetime tracking
    singlethreaded_nodeversion<P> mark_deleted() {
        return *this;
    }
    // @safe - bit manipulation
    void mark_deleted_tree() {
        v_ |= P::deleted_bit;
    }
    // @safe - bit manipulation
    void mark_root() {
        v_ |= P::root_bit;
    }
    // @safe - bit manipulation
    void mark_nonroot() {
        v_ &= ~P::root_bit;
    }

    // @safe - value assignment
    void assign_version(singlethreaded_nodeversion<P> x) {
        v_ = x.v_;
    }

    // @safe - returns stored value
    value_type version_value() const {
        return v_;
    }
    // @safe - returns stored value
    value_type unlocked_version_value() const {
        return v_;
    }

  private:
    value_type v_;
};


template <typename V> struct nodeversion_parameters {};

template <> struct nodeversion_parameters<uint32_t> {
    enum {
        lock_bit = (1U << 0),
        inserting_shift = 1,
        inserting_bit = (1U << 1),
        splitting_bit = (1U << 2),
        dirty_mask = inserting_bit | splitting_bit,
        vinsert_lowbit = (1U << 3), // == inserting_bit << 2
        vsplit_lowbit = (1U << 9),
        unused1_bit = (1U << 28),
        deleted_bit = (1U << 29),
        root_bit = (1U << 30),
        isleaf_bit = (1U << 31),
        split_unlock_mask = ~(root_bit | unused1_bit | (vsplit_lowbit - 1)),
        unlock_mask = ~(unused1_bit | (vinsert_lowbit - 1)),
        top_stable_bits = 4
    };

    typedef uint32_t value_type;
};

template <> struct nodeversion_parameters<uint64_t> {
    enum {
        lock_bit = (1ULL << 8),
        inserting_shift = 9,
        inserting_bit = (1ULL << 9),
        splitting_bit = (1ULL << 10),
        dirty_mask = inserting_bit | splitting_bit,
        vinsert_lowbit = (1ULL << 11), // == inserting_bit << 2
        vsplit_lowbit = (1ULL << 27),
        unused1_bit = (1ULL << 60),
        deleted_bit = (1ULL << 61),
        root_bit = (1ULL << 62),
        isleaf_bit = (1ULL << 63),
        split_unlock_mask = ~(root_bit | unused1_bit | (vsplit_lowbit - 1)),
        unlock_mask = ~(unused1_bit | (vinsert_lowbit - 1)),
        top_stable_bits = 4
    };

    typedef uint64_t value_type;
};

typedef nodeversion<nodeversion_parameters<uint32_t> > nodeversion32;

#endif
