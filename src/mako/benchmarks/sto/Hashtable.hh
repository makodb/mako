#pragma once
#include "config.h"
#include "compiler.hh"
// XXX: honestly hashtable should probably use local_vector too
#include <vector>
#include "Interface.hh"
#include "Transaction.hh"
#include "TWrapped.hh"
#include "simple_str.hh"
#include "print_value.hh"

#define HASHTABLE_DELETE 1

#ifndef READ_MY_WRITES
#define READ_MY_WRITES 1
#endif 

template <typename KeyType, typename ValueType, bool Opacity = true, unsigned InitialSize = 129, typename WriteValueType = ValueType, typename HashFunction = std::hash<KeyType>, typename KeyPredicate = std::equal_to<KeyType>>
#ifdef STO_NO_STM
class Hashtable {
#else
class Hashtable : public TObject {
#endif
public:
    typedef KeyType Key;
    typedef KeyType key_type;
    typedef ValueType Value;
    typedef WriteValueType Value_type;

    typedef typename std::conditional<Opacity, TVersion, TNonopaqueVersion>::type Version_type;
    typedef typename std::conditional<Opacity, TWrapped<Value>, TNonopaqueWrapped<Value>>::type wrapped_type;

    typedef ValueType write_value_type;

    static constexpr typename Version_type::type invalid_bit = TransactionTid::user_bit;
private:
  // our hashtable is an array of linked lists. 
  // an internal_elem is the node type for these linked lists
  struct internal_elem {
    // nate: I wonder if this would perform better if these had their own
    // cache line.
    Key key;
    internal_elem *next;
    Version_type version;
    wrapped_type value;
#ifndef STO_NO_STM
    internal_elem(Key k, Value val, bool mark_valid)
        : key(k), next(NULL), version(Sto::initialized_tid() | (mark_valid ? 0 : invalid_bit)), value(val) {}
    bool valid() const {
        return !(version.value() & invalid_bit);
    }
#else
    internal_elem(Key k, Value val, bool)
        : key(k), next(NULL), version(Sto::initialized_tid()), value(val) {}
#endif
  };

  struct bucket_entry {
    // nate: we could inline the first element of a bucket. Would probably
    // make resize harder though.
    internal_elem *head;
    // this is the bucket version number, which is incremented on insert
    // we use it to make sure that an unsuccessful key lookup will still be
    // unsuccessful at commit time (because this will always be true if no
    // new inserts have occurred in this bucket)
    Version_type version;
    bucket_entry() : head(NULL), version(0) {}
  };

  typedef std::vector<bucket_entry> MapType;
  // this is the hashtable itself, an array of bucket_entry's
  MapType map_;
  HashFunction hasher_;
  KeyPredicate pred_;

  unsigned long long int table_id;
  bool is_remote;
  std::string table_name_;

  // used to mark whether a key is a bucket (for bucket version checks)
  // or a pointer (which will always have the lower 3 bits as 0)
  static constexpr uintptr_t bucket_bit = 1U<<0;

  static constexpr TransItem::flags_type insert_bit = TransItem::user0_bit;
  static constexpr TransItem::flags_type delete_bit = TransItem::user0_bit<<1;

public:
  Hashtable(unsigned size = InitialSize, HashFunction h = HashFunction(), KeyPredicate p = KeyPredicate()) : map_(), hasher_(h), pred_(p) {
    map_.resize(size);
  }

  inline size_t hash(const Key& k) const {
    return hasher_(k);
  }

  inline size_t nbuckets() const {
    return map_.size();
  }

  inline size_t bucket(const Key& k) const {
    return hash(k) % nbuckets();
  }

#ifndef STO_NO_STM
  // returns true if found false if not
  template <typename KT, typename VT>
  bool transGet(const KT& k, VT& retval) {
    auto lookup = lookup_element(k);
    
    if (lookup.found) {
      auto item = t_read_only_item(lookup.element);
      if (!validate_element_access(item, lookup.element)) {
        return false;
      }
      
      // Check for read-my-writes scenarios
      if (handle_deleted_element_read(item, retval)) {
        return item.has_write(); // true if we found a write, false if deleted
      }
      
      // Normal read path
      retval = lookup.element->value.read(item, lookup.element->version);
      return true;
    } else {
      // Element not found - observe bucket version for consistency
      Sto::item(this, pack_bucket(bucket(k))).observe(Version_type(lookup.bucket_version.unlocked()));
      return false;
    }
  }

#if HASHTABLE_DELETE
  // returns true if successful
  bool transDelete(const Key& k) {
    auto lookup = lookup_element(k);
    
    if (lookup.found) {
      Version_type elemvers = lookup.element->version;
      fence();
      auto item = t_item(lookup.element);
      bool valid = lookup.element->valid();
      
#if READ_MY_WRITES
      if (!valid && has_insert(item)) {
        // Special case: deleting our own insert
        return handle_delete_own_insert(lookup.element, item, k, lookup.bucket_version);
      }
#endif
      
      if (!valid) {
        Sto::abort();
        return false;
      }
      
#if READ_MY_WRITES
      if (has_delete(item)) {
        return false; // Already deleted in this transaction
      }
#endif
      
      // Mark for deletion
      item.observe(elemvers);
      item.add_write().add_flags(delete_bit);
      return true;
    } else {
      // Element doesn't exist - observe bucket for consistency
      Sto::item(this, pack_bucket(bucket(k))).observe(Version_type(lookup.bucket_version.unlocked()));
      return false;
    }
  }
  
private:
  // Helper for handling delete of own insert
  bool handle_delete_own_insert(internal_elem* element, TransProxy& item, const Key& k, Version_type bucket_version) {
    _remove(element);
    // Clear all transaction item attributes
    item.remove_read().remove_write().clear_flags(insert_bit | delete_bit);
    // Still need to check that no one else inserts this key
    Sto::item(this, pack_bucket(bucket(k))).observe(Version_type(bucket_version.unlocked()));
    return true;
  }
  
public:
#endif

private:
  // returns true if item already existed, false if it did not
  template <bool INSERT, bool SET, typename KT, typename VT>
  bool trans_write(const KT& k, const VT& v) {
    // TODO: technically puts don't need to look into the table at all until lock time
    bucket_entry& buck = buck_entry(k);
    // TODO: update doesn't need to lock the table
    // also we should lock the head pointer instead so we don't
    // mess with tids
    lock(buck.version);
    internal_elem *e = find(buck, k);
    if (e) {
      unlock(buck.version);
      Version_type elemvers = e->version;
      fence();
      auto item = t_item(e);
      if (!validity_check(item, e)) {
        Sto::abort();
        // unreachable (t.abort() raises an exception)
        return false;
      }
#if READ_MY_WRITES
      if (has_delete(item)) {
        // delete-then-insert == update (technically v# would get set to 0, but this doesn't matter
        // if user can't read v#)
        if (INSERT) {
          item.clear_flags(delete_bit).clear_write().template add_write<write_value_type>(v);
        } else {
          // delete-then-update == not found
          // delete will check for other deletes so we don't need to re-log that check
        }
        return false;
      }
#endif

#if HASHTABLE_DELETE
      // make sure the item doesn't get deleted before us
      item.observe(elemvers);
      //if (Opacity)
      //  check_opacity(e->version);
#endif
      if (SET) {
        item.template add_write<write_value_type>(v);
#if READ_MY_WRITES
        if (has_insert(item)) {
          // Updating the value here, as we won't update it during install
          e->value.write(v);
        }
#endif
      }
      return true;
    } else {
      if (!INSERT) {
        auto buck_vers = buck.version.unlocked();
        fence();
        unlock(buck.version);
        Sto::item(this, pack_bucket(bucket(k))).observe(Version_type(buck_vers));
        //if (Opacity)
        //    check_opacity(buck.version);
        return false;
      }

      auto prev_version = buck.version.unlocked();
      // not there so need to insert
      insert_locked<false>(buck, k, v); // marked as invalid
      auto new_head = buck.head;
      auto new_version = buck.version.unlocked();
      fence();
      unlock(buck.version);
      // see if this item was previously read
      auto bucket_item = Sto::check_item(this, pack_bucket(bucket(k)));
      if (bucket_item) {
        bucket_item->update_read(Version_type(prev_version), Version_type(new_version));
        //} else { could abort transaction now
      }
      // use new_item because we know there are no collisions
      auto item = Sto::new_item(this, new_head);
      // don't actually need to Store anything for the write, just mark as valid on install
      // (for now insert and set will just do the same thing on install, set a value and then mark valid)
      item.template add_write<write_value_type>(v);
      // need to remove this item if we abort
      item.add_flags(insert_bit);
      return false;
    }
  }

public:
  template <typename KT, typename VT>
  bool transPut(const KT& k, const VT& v) {
    return trans_write</*insert*/true, /*set*/true>(k, v);
  }

  // returns true if successful
  template <typename KT, typename VT>
  bool transInsert(const KT& k, const VT& v) {
    return !trans_write</*insert*/true, /*set*/false>(k, v);
  }

  template <typename KT, typename VT>
  bool transUpdate(const KT& k, const VT& v) {
    return trans_write</*insert*/false, /*set*/true>(k, v);
  }


  bool check(TransItem& item, Transaction&) override {
    if (is_bucket(item)) {
      bucket_entry& buck = map_[bucket_key(item)];
      return buck.version.check_version(item.template read_value<Version_type>());
    }
    auto el = item.key<internal_elem*>();
    auto read_version = item.template read_value<Version_type>();
    // if item has insert_bit then its an insert so no validity check needed.
    // otherwise we check that it is both valid and not locked
    // XXX bool validity_check = has_insert(item) || (el->valid() && (!is_locked(el->version) || item.has_lock(t)));
    // XXX Why isn't it enough to just do the versionCheck?
    return el->version.check_version(read_version);
  }

  unsigned long long int get_table_id() const override {
    return table_id;
  }

  const std::string& get_table_name() const { 
    return table_name_; 
  }

  void set_table_name(const std::string& table_name) { 
    table_name_ = table_name; 
  }

  void set_table_id(const unsigned long long int table_id) {
    table_id = table_id;
  }

  bool get_is_remote() const override {
    return is_remote;
  }

  void set_is_remote(const bool is_remote) {
    is_remote = is_remote;
  }

  bool lock(TransItem& item, Transaction& txn) override {
    assert(!is_bucket(item));
    auto el = item.key<internal_elem*>();
    return txn.try_lock(item, el->version);
  }

  void install(TransItem& item, Transaction& t) override {
    assert(!is_bucket(item));
    auto el = item.key<internal_elem*>();
    assert(is_locked(el));
    // delete
    if (item.flags() & delete_bit) {
      // XXX: think we need an extra bit in here for opacity, or we should remove this now 
      // rather than in cleanup
      el->version.set_version_locked(el->version.value() | invalid_bit);
      // we wait to remove the node til cleanup() (unclear that this is actually necessary)
      return;
    }
    // else must be insert/update
    if (!(item.flags() & insert_bit)) {
      // Update
      Value& new_v = item.template write_value<write_value_type>();
      el->value.write(new_v);
    }
    //if (!__has_trivial_copy(Value)) {
      //Transaction::rcu_delete(new_v);
    //}

    el->version.set_version(t.commit_tid()); // automatically sets valid to true
    // nate: this has no visible perf change on vacation (maybe slightly slower).
#if 1
    // convert nonopaque bucket version to a commit tid
    if (Opacity && has_insert(item)) {
      bucket_entry& buck = buck_entry(el->key);
      lock(buck.version);
      // only update if it's still nonopaque. Otherwise someone with a higher tid
      // could've already updated it.
      if (buck.version.value() & TransactionTid::nonopaque_bit)
	buck.version.set_version(t.commit_tid());
      unlock(buck.version);
    }
#endif
  }

  void unlock(TransItem& item) override {
    assert(!is_bucket(item));
    auto el = item.key<internal_elem*>();
    unlock(el->version);
  }

  void cleanup(TransItem& item, bool committed) override {
    if (committed ? has_delete(item) : has_insert(item)) {
      auto el = item.key<internal_elem*>();
      assert(!el->valid());
      _remove(el);
    }
  }

  // these are wrappers for concurrent.cc and other
  // frameworks we use the hashtable in
  Value transGet(Key k) {
    Value v;
    transGet(k, v);
    return v;
  }

  Value unsafe_get(Key k) {
    if (Value* p = readPtr(k))
      return *p;
    else
      return Value();
  }

#endif /* STO_NO_STM */

  bool is_locked(const internal_elem *element) const {
    return is_locked(element->version);
  }

  void print_stats() const {
    int total_count = 0;
    int max_chaining = 0;
    int num_empty = 0;

    for (unsigned i = 0; i < map_.size(); ++i) {
      const bucket_entry& buck = map_[i];
      if (!buck.head) {
        num_empty++;
        continue;
      }
      int chain_length = 0;
      const internal_elem* list = buck.head;
      while (list) {
        chain_length++;
        total_count++;
        list = list->next;
      }

      if (chain_length > max_chaining) {
        max_chaining = chain_length;
      }
    }

    double avg_chaining = (map_.size() - num_empty) > 0 ? 
                         static_cast<double>(total_count) / (map_.size() - num_empty) : 0.0;
    printf("Total count: %d, Empty buckets: %d, Avg chaining: %f, Max chaining: %d\n", 
           total_count, num_empty, avg_chaining, max_chaining);
  }

    void print(std::ostream& w, const TransItem& item) const override {
        w << "{Hashtable<" << typeid(KeyType).name() << "," << typeid(ValueType).name() << "> " << (void*) this;
        if (is_bucket(item)) {
            w << ".b[" << bucket_key(item) << "]";
            if (item.has_read())
                w << " R" << item.read_value<Version_type>();
        } else {
            auto el = item.key<internal_elem*>();
            w << "[" << mass::print_value(el->key) << "]";
            if (item.has_read())
                w << " R" << item.read_value<Version_type>();
            if (item.has_write())
                w << " =" << mass::print_value(item.write_value<write_value_type>());
        }
        w << "}";
    }

  void print() const {
    printf("Hashtable:\n");
    for (unsigned i = 0; i < map_.size(); ++i) {
      const bucket_entry& buck = map_[i];
      if (!buck.head)
        continue;
      printf("bucket %d (version %d): ", i, buck.version);
      const internal_elem* list = buck.head;
      while (list) {
        printf("key: %d, val: %d, version: %d, valid: %d ; ", 
               list->key, list->value, list->version, list->valid());
        list = list->next;
      }
      printf("\n");
    }
  }

  // non-transactional const iteration
  // (we don't have current support for transactional iteration)
  class const_iterator {
  public:
    std::pair<Key, Value> operator*() const {
      return std::make_pair(node->key, node->value);
    }

    const_iterator& operator++() {
      if (node) {
        node = node->next;
      }
      while (!node && bucket != table->map_.size()) {
        node = table->map_[bucket].head;
        bucket++;
      }
      return *this;
    }
    
    bool operator!=(const const_iterator& it) const {
      return node != it.node || bucket != it.bucket;
    }
  private:
    const Hashtable *table;
    int bucket;
    internal_elem *node;
    friend class Hashtable;
  };

  const_iterator begin() const {
    const_iterator begin;
    begin.table = this;
    begin.bucket = -1;
    begin.node = NULL;
    return ++begin; //eh
  }
  const_iterator end() const {
    const_iterator end;
    end.bucket = map_.size();
    end.node = NULL;
    return end;
  }

  // remove given the internal element node. used by transaction system
  void _remove(internal_elem *el) {
    bucket_entry& buck = buck_entry(el->key);
    lock(buck.version);
    internal_elem *prev = NULL;
    internal_elem *cur = buck.head;
    while (cur != NULL && cur != el) {
      prev = cur;
      cur = cur->next;
    }
    assert(cur);
    if (prev) {
      prev->next = cur->next;
    } else {
      buck.head = cur->next;
    }
    unlock(buck.version);
    Transaction::rcu_delete(cur);
  }

  // non-txnal remove given a key
  bool remove(const Key& k) {
    bucket_entry& buck = buck_entry(k);
    lock(buck.version);
    internal_elem *prev = NULL;
    internal_elem *cur = buck.head;
    while (cur != NULL && !pred_(cur->key, k)) {
      prev = cur;
      cur = cur->next;
    }
    if (!cur) {
      unlock(buck.version);
      return false;
    }
    if (prev) {
      prev->next = cur->next;
    } else {
      buck.head = cur->next;
    }
    unlock(buck.version);    
    // TODO(nate): this would probably work fine as-is
    // Transaction::rcu_free(cur);
    return true;
  }

  bool read(const Key& k, Value& retval) {
    auto e = find(buck_entry(k), k);
    if (e) {
      // TODO(nate): this isn't safe for non-trivial types (need an atomic read)
      assign_val(retval, e->value.access());
    }
    return !!e;
  }

  template <typename ValType>
  static void assign_val(ValType& val, const ValType& val_to_assign) {
    val = val_to_assign;
  }
  static void assign_val(std::string& val, simple_str& val_to_assign) {
    val.assign(val_to_assign.data(), val_to_assign.length());
  }

  Value* readPtr(const Key& k) {
    auto e = find(buck_entry(k), k);
    if (e) {
      return &e->value.access();
    }
    return NULL;
  }

  // returns pointer to the value in the hashtable 
  // (no current way to distinguish if insert or set)
  Value* putIfAbsentPtr(const Key& k, const Value& val) {
    bucket_entry& buck = buck_entry(k);
    lock(buck.version);
    internal_elem *e = find(buck, k);
    if (!e) {
      insert_locked<true>(buck, k, val);
      e = buck.head;
    }
    Value *ret = &e->value.access();
    unlock(buck.version);
    return ret;
  }

  // returns true if inserted. otherwise return false and val is set to current value.
  bool putIfAbsent(const Key& k, Value& val) {
    bool exists = false;
    bucket_entry& buck = buck_entry(k);
    lock(buck.version);
    internal_elem *e = find(buck, k);
    if (e) {
      assign_val(val, e->value.access());
      exists = true;
    } else {
      insert_locked<true>(buck, k, val);
      exists = false;
    }
    unlock(buck.version);
    return exists;
  }

  // returns true if item already existed
  template <bool Insert = true, bool Set = true>
  bool put(const Key& k, const Value& val) {
    bool exists = false;
    bucket_entry& buck = buck_entry(k);
    lock(buck.version);
    internal_elem *e = find(buck, k);
    if (e) {
      // XXX: kind of a stupid Set-only (still locks bucket)
      if (Set)
        set(e, val);
      exists = true;
    } else {
      if (Insert)
        insert_locked<true>(buck, k, val);
      exists = false;
    }
    unlock(buck.version);
    return exists;
  }

  // returns true if item already existed
  // if item did exist, oldval is its old value
  template <bool Insert = true, bool Set = true>
  bool put_getold(const Key& k, const Value& val, Value& oldval) {
    bool exists = false;
    bucket_entry& buck = buck_entry(k);
    lock(buck.version);
    internal_elem *e = find(buck, k);
    if (e) {
      assign_val(oldval, e->value.access());
      // XXX: kind of a stupid Set-only (still locks bucket)
      if (Set)
        set(e, val);
      exists = true;
    } else {
      if (Insert)
        insert_locked<true>(buck, k, val);
      exists = false;
    }
    unlock(buck.version);
    return exists;
  }

  // returns true if successfully inserted
  bool insert(const Key& k, const Value& val) {
    return !put<true, false>(k, val);
  }

  void set(internal_elem *e, const Value& val) {
    assert(e);
    // XXX: we probably don't need this lock since we have the bucket lock still
    // (or we could do an optimistic set without the bucket lock)
    lock(e->version);
    e->value.access() = val;
#ifndef STO_NO_STM
    e->version.inc_nonopaque_version();
#endif
    unlock(e->version);
  }

  bool nontrans_insert(const Key& k, const Value& v) { return insert(k, v); }

  bool nontrans_find(const Key& k, Value& v) { return read(k, v); }

  bool nontrans_remove(const Key& k) { return remove(k); }

  // XXX: there's a race between the read and the remove (oldval might be stale) but mehh
  bool nontrans_remove(const Key& k, Value& oldval) { if (read(k,oldval)) return remove(k); else return false; }

private:
  // Helper functions for common transactional patterns
  
  // Common pattern: get bucket and check for element existence
  struct ElementLookupResult {
    bucket_entry* bucket;
    internal_elem* element;
    Version_type bucket_version;
    bool found;
    
    ElementLookupResult(bucket_entry* b, internal_elem* e, Version_type bv) 
      : bucket(b), element(e), bucket_version(bv), found(e != nullptr) {}
  };
  
  ElementLookupResult lookup_element(const Key& k) {
    bucket_entry& buck = buck_entry(k);
    Version_type buck_version = buck.version;
    fence();
    internal_elem* e = find(buck, k);
    return ElementLookupResult(&buck, e, buck_version);
  }
  
  // Common pattern: validate element and check for transaction conflicts
  bool validate_element_access(TransProxy& item, internal_elem* element) const {
    if (!validity_check(item, element)) {
      Sto::abort();
      return false;
    }
    return true;
  }
  
  // Common pattern: handle read-my-writes for deleted elements
  template<typename ReturnType>
  bool handle_deleted_element_read(const TransProxy& item, ReturnType& retval) const {
#if READ_MY_WRITES
    if (has_delete(item)) {
      return false; // Element was deleted in this transaction
    }
    if (item.has_write()) {
      retval = item.template write_value<write_value_type>();
      return true;
    }
#endif
    return false; // Continue with normal read path
  }

  bucket_entry& buck_entry(const Key& k) {
    return map_[bucket(k)];
  }
  
  const bucket_entry& buck_entry(const Key& k) const {
    return map_[bucket(k)];
  }

  // looks up a key's internal_elem, given its bucket
  internal_elem* find(bucket_entry& buck, const Key& k) {
    internal_elem* list = buck.head;
    while (list && !pred_(list->key, k)) {
      list = list->next;
    }
    return list;
  }
  
  // const version of find
  const internal_elem* find(const bucket_entry& buck, const Key& k) const {
    const internal_elem* list = buck.head;
    while (list && !pred_(list->key, k)) {
      list = list->next;
    }
    return list;
  }

  // looks up a key's internal_elem
  internal_elem* elem(const Key& k) {
    return find(buck_entry(k), k);
  }
  
  // const version of elem
  const internal_elem* elem(const Key& k) const {
    return find(buck_entry(k), k);
  }

  bool has_delete(const TransItem& item) const {
    return item.flags() & delete_bit;
  }

  bool has_insert(const TransItem& item) const {
    return item.flags() & insert_bit;
  }

  bool validity_check(const TransItem& item, const internal_elem* element) const {
    return has_insert(item) || element->valid();
  }

#if 0
  void check_opacity(Version& v) {
    assert(Opacity);
    Version v2 = v;
    fence();
    Sto::check_opacity(v2);
  }
#endif

  static bool is_bucket(const TransItem& item) {
    return is_bucket(item.key<void*>());
  }
  
  static bool is_bucket(void* key) {
    return (uintptr_t)key & bucket_bit;
  }
  
  static unsigned bucket_key(const TransItem& item) {
    assert(is_bucket(item));
    return (uintptr_t)item.key<void*>() >> 1;
  }
  
  void* pack_bucket(unsigned bucket_index) const {
    return (void*)((bucket_index << 1) | bucket_bit);
  }

  static bool is_locked(const Version_type& version) {
    return version.is_locked();
  }
  
  static void lock(Version_type& version) {
    version.lock();
  }
  
  static void unlock(Version_type& version) {
    version.unlock();
  }

  template <bool MarkValid>
  void insert_locked(bucket_entry& bucket, const Key& key, const Value& value) {
    assert(is_locked(bucket.version));
    auto new_element = new internal_elem(key, value, MarkValid);
    internal_elem* current_head = bucket.head;
    new_element->next = current_head;
    bucket.head = new_element;
    // Update bucket version to reflect the insertion
    bucket.version.inc_nonopaque_version();
  }

#if 0
  bool atomicRead(internal_elem *e, Version& vers, Value& val) {
    Version v2;
    do {
      v2 = e->version;
      if (is_locked(v2))
        Sto::abort();
      fence();
      assign_val(val, e->value);
      fence();
      vers = e->version;
    } while (vers != v2);
    return true;
  }
#endif

  TransProxy t_item(internal_elem* element) {
    return Sto::item(this, element);
  }

  TransProxy t_read_only_item(internal_elem* element) {
#if READ_MY_WRITES
    return Sto::read_item(this, element);
#else
    return Sto::fresh_item(this, element);
#endif
  }
};
