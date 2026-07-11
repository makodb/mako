/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2016 President and Fellows of Harvard College
 * Copyright (c) 2012-2016 Massachusetts Institute of Technology
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
// @unsafe - Masstree node structure definitions (leaf and internode)
// Defines memory layout for tree nodes with versioned keys and permutation arrays
// SAFETY: Uses placement new, raw pointer arithmetic, and atomic fence operations

#ifndef MASSTREE_STRUCT_HH
#define MASSTREE_STRUCT_HH
#include "masstree.hh"
#include "nodeversion.hh"
#include "stringbag.hh"
#include "mtcounters.hh"
#include "timestamp.hh"
namespace Masstree {

template <typename P>
struct make_nodeversion {
    typedef nodeversion_parameters<typename P::nodeversion_value_type> parameters_type;
    typedef typename mass::conditional<P::concurrent,
                                       nodeversion<parameters_type>,
                                       singlethreaded_nodeversion<parameters_type> >::type type;
};

template <typename P>
struct make_prefetcher {
    typedef typename mass::conditional<P::prefetch,
                                       value_prefetcher<typename P::value_type>,
                                       do_nothing>::type type;
};

template <typename P>
class node_base : public make_nodeversion<P>::type {
  public:
    static constexpr bool concurrent = P::concurrent;
    static constexpr int nikey = 1;
    typedef leaf<P> leaf_type;
    typedef internode<P> internode_type;
    typedef node_base<P> base_type;
    typedef typename P::value_type value_type;
    typedef leafvalue<P> leafvalue_type;
    typedef typename P::ikey_type ikey_type;
    typedef key<ikey_type> key_type;
    typedef typename make_nodeversion<P>::type nodeversion_type;
    typedef typename P::threadinfo_type threadinfo;
    typedef rusty::MutPtr<base_type> base_ptr;
    typedef rusty::MutPtr<leaf_type> leaf_ptr;
    typedef rusty::MutPtr<internode_type> internode_ptr;

    // @safe - initializes base with isleaf flag
    node_base(bool isleaf)
        : nodeversion_type(isleaf) {
    }

    // @unsafe - downcasts this and returns a raw parent pointer
    inline base_ptr parent() const {
        // almost always an internode
        if (this->isleaf())
            return static_cast<const leaf_type*>(this)->parent_;
        else
            return static_cast<const internode_type*>(this)->parent_;
    }
    // @safe - null check
    inline bool parent_exists(base_ptr p) const {
        return p != 0;
    }
    // @unsafe - delegates to parent(), which downcasts this to raw node storage
    inline bool has_parent() const {
        return parent_exists(parent());
    }
    // @unsafe - manipulates parent pointers under locks
    inline internode_ptr locked_parent(threadinfo& ti) const;
    // @unsafe { Uses static_cast to downcast this to leaf/internode }
    inline void set_parent(base_ptr p) {
        if (this->isleaf())
            static_cast<leaf_type*>(this)->parent_ = p;
        else
            static_cast<internode_type*>(this)->parent_ = p;
    }
    // @unsafe { Calls set_parent() which uses static_cast on this }
    inline void make_layer_root() {
        set_parent(nullptr);
        this->mark_root();
    }
    // @unsafe { Uses raw pointer, const_cast }
    inline base_ptr maybe_parent() const {
        base_ptr x = parent();
        return parent_exists(x) ? x : const_cast<base_type*>(this);
    }

    // @unsafe - traverses tree via raw pointers
    inline leaf_ptr reach_leaf(const key_type& k, nodeversion_type& version,
                               threadinfo& ti) const;

    // @unsafe - casts this to raw bytes and prefetches by address
    void prefetch_full() const {
        for (int i = 0; i < std::min(16 * std::min(P::leaf_width, P::internode_width) + 1, 4 * 64); i += 64)
            ::prefetch((const char *) this + i);
    }

    void print(FILE* f, const char* prefix, int indent, int kdepth);
};

template <typename P>
class internode : public node_base<P> {
  public:
    static constexpr int width = P::internode_width;
    typedef typename node_base<P>::nodeversion_type nodeversion_type;
    typedef key<typename P::ikey_type> key_type;
    typedef typename P::ikey_type ikey_type;
    typedef typename key_bound<width, P::bound_method>::type bound_type;
    typedef typename P::threadinfo_type threadinfo;
    typedef rusty::MutPtr<node_base<P>> node_ptr;
    typedef rusty::MutPtr<internode<P>> internode_ptr;

    uint8_t nkeys_;
    ikey_type ikey0_[width];
    node_ptr child_[width + 1];
    node_ptr parent_;
    kvtimestamp_t created_at_[P::debug_level > 0];

    // @safe - default initialization
    internode()
        : node_base<P>(false), nkeys_(0), parent_() {
    }

    // @unsafe { Placement new on raw memory from pool_allocate() }
    static internode<P>* make(threadinfo& ti) {
        void* ptr = ti.pool_allocate(sizeof(internode<P>),
                                     memtag_masstree_internode);
        internode<P>* n = new(ptr) internode<P>;
        assert(n);
        if (P::debug_level > 0)
            n->created_at_[0] = ti.operation_timestamp();
        return n;
    }

    // @safe - returns stored value
    int size() const {
        return nkeys_;
    }

    // @safe - returns key from array
    key_type get_key(int p) const {
        return key_type::from_ikey(ikey0_[p]);
    }
    // @safe - returns ikey from array
    ikey_type ikey(int p) const {
        return ikey0_[p];
    }
    // @unsafe { Calls ::compare() which is not borrow-checked }
    int compare_key(ikey_type a, int bp) const {
        return ::compare(a, ikey(bp));
    }
    // @unsafe { Calls ::compare() which is not borrow-checked }
    int compare_key(const key_type& a, int bp) const {
        return ::compare(a.ikey(), ikey(bp));
    }
    inline int stable_last_key_compare(const key_type& k, nodeversion_type v,
                                       threadinfo& ti) const;

    // @unsafe - casts this to raw bytes and prefetches by address
    void prefetch() const {
        for (int i = 64; i < std::min(16 * width + 1, 4 * 64); i += 64)
            ::prefetch((const char *) this + i);
    }

    void print(FILE* f, const char* prefix, int indent, int kdepth);

    // @unsafe { Frees this via pool_deallocate() without RAII }
    void deallocate(threadinfo& ti) {
        ti.pool_deallocate(this, sizeof(*this), memtag_masstree_internode);
    }
    // @unsafe { Schedules RCU free via pool_deallocate_rcu() }
    void deallocate_rcu(threadinfo& ti) {
        ti.pool_deallocate_rcu(this, sizeof(*this), memtag_masstree_internode);
    }

  private:
    // @unsafe { Calls set_parent() and assigns to child_[] array }
    void assign(int p, ikey_type ikey, node_ptr child) {
        child->set_parent(this);
        child_[p + 1] = child;
        ikey0_[p] = ikey;
    }

    // @unsafe { memcpy() on ikey0_[] and child_[] arrays }
    void shift_from(int p, const internode<P>* x, int xp, int n) {
        masstree_precondition(x != this);
        if (n) {
            memcpy(ikey0_ + p, x->ikey0_ + xp, sizeof(ikey0_[0]) * n);
            memcpy(child_ + p + 1, x->child_ + xp + 1, sizeof(child_[0]) * n);
        }
    }
    // @unsafe { memmove() on ikey0_[], raw pointer arithmetic on child_[] }
    void shift_up(int p, int xp, int n) {
        memmove(ikey0_ + p, ikey0_ + xp, sizeof(ikey0_[0]) * n);
        for (node_ptr *a = child_ + p + n, *b = child_ + xp + n; n; --a, --b, --n)
            *a = *b;
    }
    // @unsafe { memmove() on ikey0_[], raw pointer arithmetic on child_[] }
    void shift_down(int p, int xp, int n) {
        memmove(ikey0_ + p, ikey0_ + xp, sizeof(ikey0_[0]) * n);
        for (node_ptr *a = child_ + p + 1, *b = child_ + xp + 1; n; ++a, ++b, --n)
            *a = *b;
    }

    int split_into(internode_ptr nr, int p, ikey_type ka, node_ptr value,
                   ikey_type& split_ikey, int split_type);

    template <typename PP> friend class tcursor;
};

template <typename P>
// @unsafe - low-level Masstree type with tagged union, skip borrow checking
class leafvalue {
  public:
    typedef typename P::value_type value_type;
    typedef typename make_prefetcher<P>::type prefetcher_type;

    // @safe - default initialization (empty body)
    leafvalue() {
    }
    // @safe - value initialization (assigns to owned member)
    leafvalue(value_type v) {
        u_.v = v;
    }
    // @unsafe { Uses reinterpret_cast to store pointer as uintptr_t }
    leafvalue(rusty::MutPtr<node_base<P>> n) {
        u_.x = reinterpret_cast<uintptr_t>(n);
    }

    // @safe - creates empty leafvalue (calls safe value constructor)
    static leafvalue<P> make_empty() {
        return leafvalue<P>(value_type());
    }

    typedef bool (leafvalue<P>::*unspecified_bool_type)() const;
    // @unsafe { Takes address of member function for safe-bool idiom }
    operator unspecified_bool_type() const {
        return u_.x ? &leafvalue<P>::empty : 0;
    }
    // @safe - returns bool check on union state
    bool empty() const {
        return !u_.x;
    }

    // @unsafe { May return reference-like value depending on P::value_type }
    value_type value() const {
        return u_.v;
    }
    // @unsafe { Returns mutable reference without lifetime tracking }
    value_type& value() {
        return u_.v;
    }

    // @unsafe { Uses reinterpret_cast to recover pointer from uintptr_t }
    rusty::MutPtr<node_base<P>> layer() const {
        return reinterpret_cast<node_base<P>*>(u_.x);
    }

    // @unsafe { Dereferences u_.n pointer for prefetch }
    void prefetch(int keylenx) const {
        if (!leaf<P>::keylenx_is_layer(keylenx))
            prefetcher_type()(u_.v);
        else
            u_.n->prefetch_full();
    }

  private:
    union {
        rusty::MutPtr<node_base<P>> n;
        value_type v;
        uintptr_t x;
    } u_;
};

template <typename P>
class leaf : public node_base<P> {
  public:
    static constexpr int width = P::leaf_width;
    typedef typename node_base<P>::nodeversion_type nodeversion_type;
    typedef key<typename P::ikey_type> key_type;
    typedef typename node_base<P>::leafvalue_type leafvalue_type;
    typedef kpermuter<P::leaf_width> permuter_type;
    typedef typename P::ikey_type ikey_type;
    typedef typename key_bound<width, P::bound_method>::type bound_type;
    typedef typename P::threadinfo_type threadinfo;
    typedef stringbag<uint8_t> internal_ksuf_type;
    typedef stringbag<uint16_t> external_ksuf_type;
    typedef typename P::phantom_epoch_type phantom_epoch_type;
    typedef rusty::MutPtr<leaf<P>> leaf_ptr;
    typedef rusty::MutPtr<node_base<P>> node_ptr;
    static constexpr int ksuf_keylenx = 64;
    static constexpr int layer_keylenx = 128;

    enum {
        modstate_insert = 0, modstate_remove = 1, modstate_deleted_layer = 2
    };

    int8_t extrasize64_;
    uint8_t modstate_;
    uint8_t keylenx_[width];
    typename permuter_type::storage_type permutation_;
    ikey_type ikey0_[width];
    leafvalue_type lv_[width];
    external_ksuf_type* ksuf_;
    union {
        leaf_ptr ptr;
        uintptr_t x;
    } next_;
    leaf_ptr prev_;
    node_ptr parent_;
    phantom_epoch_type phantom_epoch_[P::need_phantom_epoch];
    kvtimestamp_t created_at_[P::debug_level > 0];
    internal_ksuf_type iksuf_[0];

    leaf(size_t sz, phantom_epoch_type phantom_epoch)
        : node_base<P>(true), modstate_(modstate_insert),
          permutation_(permuter_type::make_empty()),
          ksuf_(), parent_(), iksuf_{} {
        masstree_precondition(sz % 64 == 0 && sz / 64 < 128);
        extrasize64_ = (int(sz) >> 6) - ((int(sizeof(*this)) + 63) >> 6);
        if (extrasize64_ > 0)
            new((void *)&iksuf_[0]) internal_ksuf_type(width, sz - sizeof(*this));
        if (P::need_phantom_epoch)
            phantom_epoch_[0] = phantom_epoch;
    }

    // @unsafe { Placement new on raw memory from pool_allocate() }
    static leaf<P>* make(int ksufsize, phantom_epoch_type phantom_epoch, threadinfo& ti) {
        size_t sz = iceil(sizeof(leaf<P>) + std::min(ksufsize, 128), 64);
        void* ptr = ti.pool_allocate(sz, memtag_masstree_leaf);
        leaf<P>* n = new(ptr) leaf<P>(sz, phantom_epoch);
        assert(n);
        if (P::debug_level > 0)
            n->created_at_[0] = ti.operation_timestamp();
        return n;
    }
    // @unsafe { Calls make() and make_layer_root() which are @unsafe }
    static leaf<P>* make_root(int ksufsize, leaf_ptr parent, threadinfo& ti) {
        leaf<P>* n = make(ksufsize, parent ? parent->phantom_epoch() : phantom_epoch_type(), ti);
        n->next_.ptr = n->prev_ = 0;
        n->make_layer_root();
        return n;
    }

    // @safe - returns compile-time constant
    static size_t min_allocated_size() {
        return (sizeof(leaf<P>) + 63) & ~size_t(63);
    }
    // @safe - pure arithmetic
    size_t allocated_size() const {
        int es = (extrasize64_ >= 0 ? extrasize64_ : -extrasize64_ - 1);
        return (sizeof(*this) + es * 64 + 63) & ~size_t(63);
    }
    // @safe - returns stored value
    phantom_epoch_type phantom_epoch() const {
        return P::need_phantom_epoch ? phantom_epoch_[0] : phantom_epoch_type();
    }

    // @safe - delegates to permuter_type::size
    int size() const {
        return permuter_type::size(permutation_);
    }
    // @safe - returns permuter copy (kpermuter value constructor is @safe)
    permuter_type permutation() const {
        return permuter_type(permutation_);
    }
    // @unsafe { Uses operator<< which is not borrow-checked }
    typename nodeversion_type::value_type full_version_value() const {
        static_assert(int(nodeversion_type::traits_type::top_stable_bits) >= int(permuter_type::size_bits), "not enough bits to add size to version");
        return (this->version_value() << permuter_type::size_bits) + size();
    }
    // @unsafe { Calls v.unlock() which manipulates version bits atomically }
    typename nodeversion_type::value_type full_unlocked_version_value() const {
        static_assert(int(nodeversion_type::traits_type::top_stable_bits) >= int(permuter_type::size_bits), "not enough bits to add size to version");
        typename node_base<P>::nodeversion_type v(*this);
        if (v.locked())
            // subtlely, unlocked_version_value() is different than v.unlock(); v.version_value() because the latter will add a
            // split bit if we're doing a split. So we do the latter to get the fully correct version.
            v.unlock();
        return (v.version_value() << permuter_type::size_bits) + size();
    }

    using node_base<P>::has_changed;
    // @unsafe { Calls base has_changed() which uses memory fence() }
    bool has_changed(nodeversion_type oldv,
                     typename permuter_type::storage_type oldperm) const {
        return this->has_changed(oldv) || oldperm != permutation_;
    }

    // @unsafe - may read key suffix through raw stringbag storage
    key_type get_key(int p) const {
        int keylenx = keylenx_[p];
        if (!keylenx_has_ksuf(keylenx))
            return key_type::from_ikey_len(ikey0_[p], keylenx);
        else
            return key_type::from_ikey_suffix(ikey0_[p], ksuf(p));
    }
    // @safe - array access
    ikey_type ikey(int p) const {
        return ikey0_[p];
    }
    // @safe - array access
    ikey_type ikey_bound() const {
        return ikey0_[0];
    }
    // @unsafe { Calls key::compare() which is not borrow-checked }
    int compare_key(const key_type& a, int bp) const {
        return a.compare(ikey(bp), keylenx_[bp]);
    }
    inline int stable_last_key_compare(const key_type& k, nodeversion_type v,
                                       threadinfo& ti) const;

    inline leaf_ptr advance_to_key(const key_type& k, nodeversion_type& version,
                                   threadinfo& ti) const;

    // @safe - pure comparison
    static bool keylenx_is_layer(int keylenx) {
        return keylenx > 127;
    }
    // @safe - pure comparison
    static bool keylenx_has_ksuf(int keylenx) {
        return keylenx == ksuf_keylenx;
    }

    // @safe - delegates to keylenx_is_layer
    bool is_layer(int p) const {
        return keylenx_is_layer(keylenx_[p]);
    }
    // @safe - delegates to keylenx_has_ksuf
    bool has_ksuf(int p) const {
        return keylenx_has_ksuf(keylenx_[p]);
    }
    // @unsafe - returns suffix view from raw stringbag storage
    Str ksuf(int p, int keylenx) const {
        (void) keylenx;
        masstree_precondition(keylenx_has_ksuf(keylenx));
        // @unsafe - ksuf_->get / iksuf_->get
        { return ksuf_ ? ksuf_->get(p) : iksuf_[0].get(p); }
    }
    // @unsafe - delegates to ksuf(), which reads raw suffix storage
    Str ksuf(int p) const {
        // @unsafe - calls ksuf(p, keylenx)
        { return ksuf(p, keylenx_[p]); }
    }
    // @unsafe - delegates to suffix comparison using raw suffix storage
    bool ksuf_equals(int p, const key_type& ka) const {
        // @unsafe - calls ksuf_equals
        { return ksuf_equals(p, ka, keylenx_[p]); }
    }
    // @unsafe - compares suffix bytes from raw stringbag storage
    bool ksuf_equals(int p, const key_type& ka, int keylenx) const {
        if (!keylenx_has_ksuf(keylenx))
            return true;
        // @unsafe - ksuf, suffix, equals_sloppy
        {
            Str s = ksuf(p, keylenx);
            return s.len == ka.suffix().len
                && string_slice<uintptr_t>::equals_sloppy(s.s, ka.suffix().s, s.len);
        }
    }
    // @unsafe - compares suffix bytes from raw stringbag storage
    // Returns 1 if match & not layer, 0 if no match, <0 if match and layer
    int ksuf_matches(int p, const key_type& ka) const {
        int keylenx = keylenx_[p];
        if (keylenx < ksuf_keylenx)
            return 1;
        if (keylenx == layer_keylenx)
            return -(int) sizeof(ikey_type);
        // @unsafe - ksuf, suffix, equals_sloppy
        {
            Str s = ksuf(p, keylenx);
            return s.len == ka.suffix().len
                && string_slice<uintptr_t>::equals_sloppy(s.s, ka.suffix().s, s.len);
        }
    }
    // @unsafe - compares suffix view from raw stringbag storage
    int ksuf_compare(int p, const key_type& ka) const {
        int keylenx = keylenx_[p];
        if (!keylenx_has_ksuf(keylenx))
            return 0;
        return ksuf(p, keylenx).compare(ka.suffix());
    }

    // @unsafe - reads capacity from raw stringbag storage
    size_t ksuf_used_capacity() const {
        if (ksuf_)
            return ksuf_->used_capacity();
        else if (extrasize64_ > 0)
            return iksuf_[0].used_capacity();
        else
            return 0;
    }
    // @unsafe - reads capacity from raw stringbag storage
    size_t ksuf_capacity() const {
        if (ksuf_)
            return ksuf_->capacity();
        else if (extrasize64_ > 0)
            return iksuf_[0].capacity();
        else
            return 0;
    }
    // @safe - null check
    bool ksuf_external() const {
        return ksuf_;
    }
    // @unsafe - returns string view from raw stringbag storage
    Str ksuf_storage(int p) const {
        if (ksuf_)
            return ksuf_->get(p);
        else if (extrasize64_ > 0)
            return iksuf_[0].get(p);
        else
            return Str::empty();
    }

    // @safe - value comparison
    bool deleted_layer() const {
        return modstate_ == modstate_deleted_layer;
    }

    // @unsafe - casts raw leaf and suffix storage pointers for prefetch
    void prefetch() const {
        for (int i = 64; i < std::min(16 * width + 1, 4 * 64); i += 64)
            ::prefetch((const char *) this + i);
        if (extrasize64_ > 0)
            ::prefetch((const char *) &iksuf_[0]);
        else if (extrasize64_ < 0) {
            ::prefetch((const char *) ksuf_);
            ::prefetch((const char *) ksuf_ + CACHE_LINE_SIZE);
        }
    }

    void print(FILE* f, const char* prefix, int indent, int kdepth);

    // @unsafe { Uses reinterpret_cast and mask on next_.x union member }
    leaf_ptr safe_next() const {
        return reinterpret_cast<leaf<P>*>(next_.x & ~(uintptr_t) 1);
    }

    // @unsafe { Frees ksuf_ and this via deallocate() without RAII }
    void deallocate(threadinfo& ti) {
        if (ksuf_)
            ti.deallocate(ksuf_, ksuf_->capacity(),
                          memtag_masstree_ksuffixes);
        if (extrasize64_ != 0)
            iksuf_[0].~stringbag();
        ti.pool_deallocate(this, allocated_size(), memtag_masstree_leaf);
    }
    // @unsafe { Schedules RCU free for ksuf_ and this }
    void deallocate_rcu(threadinfo& ti) {
        if (ksuf_)
            ti.deallocate_rcu(ksuf_, ksuf_->capacity(),
                              memtag_masstree_ksuffixes);
        ti.pool_deallocate_rcu(this, allocated_size(), memtag_masstree_leaf);
    }

  private:
    // @safe - sets modstate flag
    inline void mark_deleted_layer() {
        modstate_ = modstate_deleted_layer;
    }

    // @unsafe { Writes to lv_[], ikey0_[], keylenx_[] arrays; calls assign_ksuf() }
    inline void assign(int p, const key_type& ka, threadinfo& ti) {
        lv_[p] = leafvalue_type::make_empty();
        ikey0_[p] = ka.ikey();
        if (!ka.has_suffix())
            keylenx_[p] = ka.length();
        else {
            keylenx_[p] = ksuf_keylenx;
            assign_ksuf(p, ka.suffix(), false, ti);
        }
    }
    // @unsafe { Writes to lv_[], ikey0_[], keylenx_[] arrays; calls assign_ksuf() }
    inline void assign_initialize(int p, const key_type& ka, threadinfo& ti) {
        lv_[p] = leafvalue_type::make_empty();
        ikey0_[p] = ka.ikey();
        if (!ka.has_suffix())
            keylenx_[p] = ka.length();
        else {
            keylenx_[p] = ksuf_keylenx;
            assign_ksuf(p, ka.suffix(), true, ti);
        }
    }
    // @unsafe { Copies from source leaf x via raw pointer access }
    inline void assign_initialize(int p, leaf_ptr x, int xp, threadinfo& ti) {
        lv_[p] = x->lv_[xp];
        ikey0_[p] = x->ikey0_[xp];
        keylenx_[p] = x->keylenx_[xp];
        if (x->has_ksuf(xp))
            assign_ksuf(p, x->ksuf(xp), true, ti);
    }
    // @safe - initializes key slot for layer
    inline void assign_initialize_for_layer(int p, const key_type& ka) {
        assert(ka.has_suffix());
        ikey0_[p] = ka.ikey();
        keylenx_[p] = layer_keylenx;
    }
    void assign_ksuf(int p, Str s, bool initializing, threadinfo& ti);

    inline ikey_type ikey_after_insert(const permuter_type& perm, int i,
                                       const key_type& ka, int ka_i) const;
    int split_into(leaf_ptr nr, int p, const key_type& ka, ikey_type& split_ikey,
                   threadinfo& ti);

    template <typename PP> friend class tcursor;
};


template <typename P>
// @unsafe { Allocates root leaf via raw memory pool }
void basic_table<P>::initialize(threadinfo& ti) {
    masstree_precondition(!root_);
    root_ = node_type::leaf_type::make_root(0, 0, ti);
}


/** @brief Return this node's parent in locked state.
    @pre this->locked()
    @post this->parent() == result && (!result || result->locked()) */
template <typename P>
// @unsafe { Traverses parent chain acquiring locks, uses raw pointers }
rusty::MutPtr<internode<P>> node_base<P>::locked_parent(threadinfo& ti) const
{
    rusty::MutPtr<node_base<P>> p;
    masstree_precondition(!this->concurrent || this->locked());
    while (1) {
        p = this->parent();
        if (!this->parent_exists(p))
            break;
        nodeversion_type pv = p->lock(*p, ti.lock_fence(tc_internode_lock));
        if (p == this->parent()) {
            masstree_invariant(!p->isleaf());
            break;
        }
        p->unlock(pv);
        relax_fence();
    }
    return static_cast<internode<P>*>(p);
}


/** @brief Return the result of compare_key(k, LAST KEY IN NODE).

    Reruns the comparison until a stable comparison is obtained. */
template <typename P>
// @unsafe { Retries comparison under concurrent modification using fence }
inline int
internode<P>::stable_last_key_compare(const key_type& k, nodeversion_type v,
                                      threadinfo& ti) const
{
    while (1) {
        int cmp = compare_key(k, size() - 1);
        if (likely(!this->has_changed(v)))
            return cmp;
        v = this->stable_annotated(ti.stable_fence());
    }
}

template <typename P>
// @unsafe { Retries comparison under concurrent modification using fence }
inline int
leaf<P>::stable_last_key_compare(const key_type& k, nodeversion_type v,
                                 threadinfo& ti) const
{
    while (1) {
        typename leaf<P>::permuter_type perm(permutation_);
        int p = perm[perm.size() - 1];
        int cmp = compare_key(k, p);
        if (likely(!this->has_changed(v)))
            return cmp;
        v = this->stable_annotated(ti.stable_fence());
    }
}


/** @brief Return the leaf in this tree layer responsible for @a ka.

    Returns a stable leaf. Sets @a version to the stable version. */
template <typename P>
// @unsafe { Traverses tree via raw internode/child pointers with retry loops }
inline rusty::MutPtr<leaf<P>> node_base<P>::reach_leaf(const key_type& ka,
                                                       nodeversion_type& version,
                                                       threadinfo& ti) const
{
    rusty::Ptr<node_base<P>> n[2];
    typename node_base<P>::nodeversion_type v[2];
    bool sense;

    // Get a non-stale root.
    // Detect staleness by checking whether n has ever split.
    // The true root has never split.
 retry:
    sense = false;
    n[sense] = this;
    while (1) {
        v[sense] = n[sense]->stable_annotated(ti.stable_fence());
        if (v[sense].is_root())
            break;
        ti.mark(tc_root_retry);
        n[sense] = n[sense]->maybe_parent();
    }

    // Loop over internal nodes.
    while (!v[sense].isleaf()) {
        const internode<P> *in = static_cast<const internode<P> *>(n[sense]);
        in->prefetch();
        int kp = internode<P>::bound_type::upper(ka, *in);
        n[!sense] = in->child_[kp];
        if (!n[!sense])
            goto retry;
        v[!sense] = n[!sense]->stable_annotated(ti.stable_fence());

        if (likely(!in->has_changed(v[sense]))) {
            sense = !sense;
            continue;
        }

        typename node_base<P>::nodeversion_type oldv = v[sense];
        v[sense] = in->stable_annotated(ti.stable_fence());
        if (oldv.has_split(v[sense])
            && in->stable_last_key_compare(ka, v[sense], ti) > 0) {
            ti.mark(tc_root_retry);
            goto retry;
        } else
            ti.mark(tc_internode_retry);
    }

    version = v[sense];
    return const_cast<leaf<P> *>(static_cast<const leaf<P> *>(n[sense]));
}

/** @brief Return the leaf at or after *this responsible for @a ka.
    @pre *this was responsible for @a ka at version @a v

    Checks whether *this has split since version @a v. If it has split, then
    advances through the leaves using the B^link-tree pointers and returns
    the relevant leaf, setting @a v to the stable version for that leaf. */
template <typename P>
// @unsafe { Follows B^link-tree next pointers via safe_next() }
rusty::MutPtr<leaf<P>> leaf<P>::advance_to_key(const key_type& ka, nodeversion_type& v,
                                               threadinfo& ti) const
{
    rusty::Ptr<leaf<P>> n = this;
    nodeversion_type oldv = v;
    v = n->stable_annotated(ti.stable_fence());
    if (v.has_split(oldv)
        && n->stable_last_key_compare(ka, v, ti) > 0) {
        rusty::MutPtr<leaf<P>> next;
        ti.mark(tc_leaf_walk);
        while (likely(!v.deleted()) && (next = n->safe_next())
               && compare(ka.ikey(), next->ikey_bound()) >= 0) {
            n = next;
            v = n->stable_annotated(ti.stable_fence());
        }
    }
    return const_cast<leaf<P>*>(n);
}


/** @brief Assign position @a p's keysuffix to @a s.

    This may allocate a new suffix container, copying suffixes over.

    The @a initializing parameter determines which suffixes are copied. If @a
    initializing is false, then this is an insertion into a live node. The
    live node's permutation indicates which keysuffixes are active, and only
    active suffixes are copied. If @a initializing is true, then this
    assignment is part of the initialization process for a new node. The
    permutation might not be set up yet. In this case, it is assumed that key
    positions [0,p) are ready: keysuffixes in that range are copied. In either
    case, the key at position p is NOT copied; it is assigned to @a s. */
template <typename P>
// @unsafe { May allocate new suffix storage, copies via raw pointers, RCU frees old }
void leaf<P>::assign_ksuf(int p, Str s, bool initializing, threadinfo& ti) {
    if ((ksuf_ && ksuf_->assign(p, s))
        || (extrasize64_ > 0 && iksuf_[0].assign(p, s)))
        return;

    external_ksuf_type* oksuf = ksuf_;

    permuter_type perm(permutation_);
    int n = initializing ? p : perm.size();

    size_t csz = 0;
    for (int i = 0; i < n; ++i) {
        int mp = initializing ? i : perm[i];
        if (mp != p && has_ksuf(mp))
            csz += ksuf(mp).len;
    }

    size_t sz = iceil_log2(external_ksuf_type::safe_size(width, csz + s.len));
    if (oksuf)
        sz = std::max(sz, oksuf->capacity());

    void* ptr = ti.allocate(sz, memtag_masstree_ksuffixes);
    external_ksuf_type* nksuf = new(ptr) external_ksuf_type(width, sz);
    for (int i = 0; i < n; ++i) {
        int mp = initializing ? i : perm[i];
        if (mp != p && has_ksuf(mp)) {
            bool ok = nksuf->assign(mp, ksuf(mp));
            assert(ok); (void) ok;
        }
    }
    bool ok = nksuf->assign(p, s);
    assert(ok); (void) ok;
    fence();

    // removed ksufs aren't copied to the new ksuf, but observers
    // might need them. We ensure that observers must retry by
    // ensuring that we are not currently in the remove state.
    // State transitions are accompanied by mark_insert() so observers
    // will retry.
    masstree_invariant(modstate_ != modstate_remove);

    ksuf_ = nksuf;
    fence();

    if (extrasize64_ >= 0)      // now the new ksuf_ installed, mark old dead
        extrasize64_ = -extrasize64_ - 1;

    if (oksuf)
        ti.deallocate_rcu(oksuf, oksuf->capacity(),
                          memtag_masstree_ksuffixes);
}

// @safe - default construction
template <typename P>
inline basic_table<P>::basic_table()
    : root_(0) {
}

// @safe - move ctor: take ownership of other's root_ and leave other empty.
template <typename P>
inline basic_table<P>::basic_table(basic_table<P>&& other) noexcept
    : root_(other.root_) {
    other.root_ = 0;
}

// @unsafe - transfers raw root pointer ownership and returns a reference without lifetime annotation.
// Caller is responsible for having destroy()'d the destination's prior
// tree if it had one (we can't do it here without a threadinfo).
template <typename P>
inline basic_table<P>& basic_table<P>::operator=(basic_table<P>&& other) noexcept {
    if (this != &other) {
        root_ = other.root_;
        other.root_ = 0;
    }
    return *this;
}

// @safe - returns rusty::MutPtr (borrow-checked pointer type)
template <typename P>
inline rusty::MutPtr<node_base<P>> basic_table<P>::root() const {
    return root_;
}

// @unsafe - walks raw root parent pointers and performs atomic CAS
template <typename P>
inline rusty::MutPtr<node_base<P>> basic_table<P>::fix_root() {
    rusty::MutPtr<node_base<P>> root = root_;
    if (unlikely(!root->is_root())) {
        rusty::MutPtr<node_base<P>> old_root = root;
        root = root->maybe_parent();
        // @unsafe { Atomic compare-exchange on raw pointer }
        (void) cmpxchg(&root_, old_root, root);
    }
    return root;
}

} // namespace Masstree
#endif
