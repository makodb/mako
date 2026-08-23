#pragma once

#include "masstree.hh"
#include "kvthread.hh"
#include "masstree_tcursor.hh"
#include "masstree_insert.hh"
#include "masstree_print.hh"
#include "masstree_remove.hh"
#include "masstree_scan.hh"
#include "string.hh"
#include "Transaction.hh"

#include "StringWrapper.hh"
#include "versioned_value.hh"
#include "stuffed_str.hh"
#include "multiversion.hh"
#include "sync_util.hh"
#include "lib/common.h"
#include <atomic>
#include "common.hh"
#include "stdlib.h"

#define RCU 1
#define ABORT_ON_WRITE_READ_CONFLICT 0

#ifndef READ_MY_WRITES
#define READ_MY_WRITES 1
#endif

#include "Debug_rcu.hh"

template <typename V, typename Box = versioned_value_struct<V>, bool Opacity = true>
class MassTrans : public TObject {
public:
#if !RCU
  typedef debug_threadinfo threadinfo;
#endif

  struct ti_wrapper {
    threadinfo *ti;
  };

  typedef V value_type;
  typedef ti_wrapper threadinfo_type;
  typedef Masstree::Str Str;

  typedef typename Box::version_type Version;
  typedef typename std::conditional<Opacity, TVersion, TNonopaqueVersion>::type tversion_type;

  static __thread threadinfo_type mythreadinfo;
  unsigned long long int table_id = 0;
  bool is_remote = false;
  std::string table_name_;

protected:

  typedef Box versioned_value;
  
public:
    typedef V write_value_type;
    typedef std::string key_write_value_type;

  MassTrans() {
#if RCU
    if (!mythreadinfo.ti) {
      auto* ti = threadinfo::make(threadinfo::TI_MAIN, -1);
      mythreadinfo.ti = ti;
    }
#else
    mythreadinfo.ti = new threadinfo;
#endif
    table_.initialize(*mythreadinfo.ti);
    // TODO: technically we could probably free this threadinfo at this point since we won't use it again,
    // but doesn't seem to be possible
  }

  static void static_init() {
    Transaction::epoch_advance_callback = [] (unsigned) {
      // just advance blindly because of the way Masstree uses epochs
      globalepoch++;
    };
  }

  const std::string& get_table_name() const { return table_name_; }

  void set_table_name(const std::string& t) { table_name_ = t; }

  unsigned long long int get_table_id() const override{
    return table_id;
  }

  void set_table_id(const unsigned long long int tid) {
    table_id = tid;
  }

  bool get_is_remote() const override{
    return is_remote;
  }

  void set_is_remote(const bool is_remote_t) {
    is_remote = is_remote_t;
  }

  static void thread_init() {
#if !RCU
    mythreadinfo.ti = new threadinfo;
    return;
#else
    auto current_context = MasstreeContext::Current();
    if (!mythreadinfo.ti || mythreadinfo.ti->context() != current_context) {
      auto* ti = threadinfo::make(threadinfo::TI_PROCESS, TThread::id());
      mythreadinfo.ti = ti;
    }
    if (MAX_THREADS<=TThread::id()) {
      Panic("the id is so large, %d-%d", MAX_THREADS, TThread::id());
    }
    Transaction::tinfo[TThread::id()].trans_start_callback = [] () {
      mythreadinfo.ti->rcu_start();
    };
    Transaction::tinfo[TThread::id()].trans_end_callback = [] () {
      mythreadinfo.ti->rcu_stop();
    };
#endif
  }

  template <typename ValType>
  bool transGet(Str key, ValType& retval, threadinfo_type& ti = mythreadinfo) {
    // if false:
    //   1) not found a key
    //   2) found a key, but updated by other writes 
    /// in the original implement, throw exception to distinguish case2
    unlocked_cursor_type lp(table_, key);
    bool found = lp.find_unlocked(*ti.ti);
    if (found) {
      versioned_value *e = lp.value();
      auto item = t_read_only_item(e);
      if (!validityCheck(item, e)) { 
        //
        Sto::abort_without_throw(); // Sto::abort(); // this throw an error
        TThread::transget_without_throw=true;
        return false;
      }
#if READ_MY_WRITES
      if (readMyWritesEnabled()) {
        if (has_delete(item)) {
          return false;
        }
        if (item.has_write()) {
          // A newly inserted value already lives in its private, invalid
          // Masstree entry. Updates remain copy-staged in the TransItem until
          // install. Copy either representation into the caller's output;
          // never expose transaction- or tree-owned storage.
          if (has_insert(item)) {
            assign_val(retval, e->read_value());
          } else {
            retval = item.template write_value<write_value_type>();
          }
          return true;
        }
      }
#endif
      Version elem_vers;
      if(!atomicRead(e, elem_vers, retval)){ // atomicRead might throw an error as well
        return false;
      }
      item.observe(tversion_type(elem_vers));
      if (TThread::is_multiversion())
        return MultiVersionValue::mvGET(retval, (char*)e->data(), TThread::txn->get_current_term(), sync_util::sync_logger::hist_timestamp);
    } else {
      //Warning("Not found a value");
      ensureNotFound(lp.node(), lp.full_version_value());
    }
    return found;
  }

  template <typename K>
  bool transDelete(const K& key, threadinfo_type& ti = mythreadinfo) {
    unlocked_cursor_type lp(table_, key);
    bool found = lp.find_unlocked(*ti.ti);
    if (found) {
      versioned_value *e = lp.value();
      Version v = e->version();
      fence();
      auto item = t_item(e);
      item.add_extra(key) ;
      bool valid = !(v & invalid_bit);
#if READ_MY_WRITES
      if (readMyWritesEnabled() && !valid && has_insert(item)) {
        if (has_delete(item)) {
          // insert-then-delete then delete, so second delete should return false
          return false;
        }
        // otherwise this is an insert-then-delete
	// has_insert() is used all over the place so we just keep that flag set
        item.add_flags(delete_bit);
        // key is already in write data since this used to be an insert
        return true;
      } else 
#endif
     if (!valid) {
        Sto::abort();
        return false;
      }
      assert(valid);
#if READ_MY_WRITES
      // already deleted!
      if (readMyWritesEnabled() && has_delete(item)) {
        return false;
      }
#endif
      item.observe(tversion_type(v));
      // same as inserts we need to Store (copy) key so we can lookup to remove later
      item.template add_write<key_write_value_type>(key).add_flags(delete_bit);
      return found;
    } else {
      ensureNotFound(lp.node(), lp.full_version_value());
      return found;
    }
  }

private:
  template <bool INSERT, bool SET, typename StringType, typename ValueType>
  bool trans_write(const StringType& key, const ValueType& value, bool(*compar)(const std::string& newValue,const std::string& oldValue), threadinfo_type& ti = mythreadinfo) {
    // optimization to do an unlocked lookup first
    if (SET) {
      unlocked_cursor_type lp(table_, key);
      bool found = lp.find_unlocked(*ti.ti);
      if (found) {
        if (compar != nullptr) {
          versioned_value *e = lp.value ();
          if(!compar(value, e->read_value())){
            return false;
          }
        }
        return handlePutFound<INSERT, SET>(lp.value(), key, value);
      } else {
        if (!INSERT) {
          ensureNotFound(lp.node(), lp.full_version_value());
          return false;
        }
      }
    }

    cursor_type lp(table_, key);
    bool found = lp.find_insert(*ti.ti);
    if (found) {
      versioned_value *e = lp.value();
      if (compar != nullptr) {
        if(!compar(value, e->read_value())){
          lp.finish (0, *ti.ti);
          return false;
        }
      }
      lp.finish(0, *ti.ti);
      return handlePutFound<INSERT, SET>(e, key, value);
    } else {
      //      auto p = ti.ti->allocate(sizeof(versioned_value), memtag_value);
      versioned_value* val;
      try {
        val = (versioned_value*)versioned_value::make(value, invalid_bit);  // malloc customer::value
      } catch (...) {
        // find_insert holds the cursor lock and may have assigned an unpublished
        // slot. Cancel that candidate before propagating allocation failure;
        // otherwise the ABI catch would leave the Masstree node locked forever.
        lp.finish(0, *ti.ti);
        throw;
      }
      lp.value() = val;
#if ABORT_ON_WRITE_READ_CONFLICT
      auto orig_node = lp.node();
      auto orig_version = lp.previous_full_version_value();
      auto upd_version = lp.next_full_version_value(1);
#endif

      lp.finish(1, *ti.ti);
      fence();

#if !ABORT_ON_WRITE_READ_CONFLICT
      auto orig_node = lp.original_node();
      auto orig_version = lp.original_version_value();
      auto upd_version = lp.updated_version_value();
#endif

      if (updateNodeVersion(orig_node, orig_version, upd_version)) {
        // add any new nodes as a result of splits, etc. to the read/absent set
#if !ABORT_ON_WRITE_READ_CONFLICT
        for (auto&& pair : lp.new_nodes()) {
          auto nodeitem = Sto::new_item(this, tag_inter(pair.first));
          if (Opacity)
            nodeitem.add_read_opaque(pair.second);
          else
            nodeitem.add_read(pair.second);
        }
#endif
      }
      // TransItem::key_ is actuall value, TransItem::wdata_ or rdata_ is actual key in write-set and read-set, respectively
      auto item = Sto::new_item(this, val);
      item.template add_write<key_write_value_type>(key).add_flags(insert_bit);
      return found;
    }
  }

public:
  template <typename KT, typename VT>
  bool transPut(const KT& k, const VT& v, threadinfo_type& ti = mythreadinfo) {
    return trans_write</*insert*/true, /*set*/true>(k, v, nullptr, ti);
  }

  template <typename KT, typename VT>
  bool transPutMbta(const KT& k, const VT& v,
                    bool(*compar)(const std::string& newValue,const std::string& oldValue),
                    threadinfo_type& ti = mythreadinfo) {
      return trans_write</*insert*/true, /*set*/true>(k, v, compar, ti);
  }

  template <typename KT, typename VT>
  bool transUpdate(const KT& k, const VT& v, threadinfo_type& ti = mythreadinfo) {
    return trans_write</*insert*/false, /*set*/true>(k, v, nullptr, ti);
  }

  // TODO: for new order, there is no case for one new order: remove and then insert the same order again
  template <typename KT, typename VT>
  bool transInsert(const KT& k, const VT& v, threadinfo_type& ti = mythreadinfo) { 
    return trans_write</*insert*/true, /*set*/false>(k, v, nullptr, ti);
  }


  // Returns an approximate count of keys in the table (local shard only).
  // Updated at commit time under lock; may be stale for recently aborted transactions.
  size_t approx_size() const {
    // @safe - returns count updated at commit time (under lock); no atomics needed.
    return size_count_;
  }

  // goddammit templates/hax
  template <typename Callback, typename V2>
  static bool query_callback_overload(Str key, versioned_value_struct<V2> *val, Callback c) {
    return c(key, val->read_value());
  }

  template <typename Callback>
  static bool query_callback_overload(Str key, versioned_str_struct *val, Callback c) {
    return c(key, val);
  }

  // unused (we just stack alloc if no allocator is passed)
  class DefaultValAllocator {
  public:
    value_type* operator()() {
      assert(0);
      return new value_type();
    }
  };

  // range queries
  template <typename Callback, typename ValAllocator = DefaultValAllocator>
  void transQuery(Str begin, Str end, Callback callback, ValAllocator *va = NULL, threadinfo_type& ti = mythreadinfo) {
    auto node_callback = [&] (leaf_type* node, typename unlocked_cursor_type::nodeversion_value_type version) {
      this->ensureNotFound(node, version);
    };
    int deleted_cnt=0;
    auto value_callback = [&] (Str key, versioned_value* e) {
      // TODO: this needs to read my writes
      auto item = this->t_read_only_item(e);
// #if READ_MY_WRITES
//       if (has_delete(item)) {
//         return true;
//       }
//       if (item.has_write()) {
//         // read directly from the element if we're inserting it
//         if (has_insert(item)) {
// 	      return range_query_has_insert(callback, key, e, va);
// 	  //return callback(key, val);
//         } else {
//           return callback(key, item.template write_value<write_value_type>());
//         }
//       }
// #endif
      // not sure of a better way to do this
      value_type stack_val;
      value_type& val = va ? *(*va)() : stack_val;
      Version v;
      if(!atomicRead(e, v, val)){
        Sto::abort();
      }
      item.observe(tversion_type(v));

      if (!TThread::is_multiversion())
        return callback(key, val);

      // key and val are both only guaranteed until callback returns
      bool ret = MultiVersionValue::mvGET(val,
                                          (char*)e->data(),
                                          TThread::txn->get_current_term(), 
                                          sync_util::sync_logger::hist_timestamp);
      if (ret){
        return callback(key, val);//query_callback_overload(key, val, callback);
      }else {
        deleted_cnt++;
        if (deleted_cnt>10){
          return false; // TODO, it's better to keep new_order id for taking over
        }
        return true;//skip the deleted items
      }
    };

    range_scanner<decltype(node_callback), decltype(value_callback)> scanner(end, node_callback, value_callback);
    table_.scan(begin, true, scanner, *ti.ti);
  }

  template <typename Callback, typename ValAllocator = DefaultValAllocator>
  void transRQuery(Str begin, Str end, Callback callback, ValAllocator *va = NULL, threadinfo_type& ti = mythreadinfo) {
    auto node_callback = [&] (leaf_type* node, typename unlocked_cursor_type::nodeversion_value_type version) {
      this->ensureNotFound(node, version);
    };
    int deleted_cnt=0;
    auto value_callback = [&] (Str key, versioned_value* e) {
      auto item = this->t_read_only_item(e);
      // not sure of a better way to do this
      value_type stack_val;
      value_type& val = va ? *(*va)() : stack_val;
// #if READ_MY_WRITES
//       if (has_delete(item)) {
//         return true;
//       }
//       if (item.has_write()) {
//         // read directly from the element if we're inserting it
//         if (has_insert(item)) {
// 	        return range_query_has_insert(callback, key, e, va);
//         } else {
//             return callback(key, item.template write_value<write_value_type>());
//         }
//       }
// #endif
      Version v;
      if(!atomicRead(e, v, val)){
        Sto::abort();
      }
      item.observe(tversion_type(v));

      if (!TThread::is_multiversion())
        return callback(key, val);

      bool ret = MultiVersionValue::mvGET(val,
                                          (char*)e->data(),
                                          TThread::txn->get_current_term(), 
                                          sync_util::sync_logger::hist_timestamp);
      if (ret)
        return callback(key, val);//query_callback_overload(key, val, callback);
      else {
        deleted_cnt++;
        if (deleted_cnt>10){
          return false; // TODO, it's better to keep new_order id for taking over
        }
        return true;//skip the deleted items
      }
    };

    range_scanner<decltype(node_callback), decltype(value_callback), true> scanner(end, node_callback, value_callback);
    table_.rscan(begin, true, scanner, *ti.ti);
  }

#if READ_MY_WRITES
  template <typename Callback, typename ValAllocator>
  // for some reason inlining this/not making it a function gives a 5% slowdown on g++...
  static __attribute__((noinline)) bool range_query_has_insert(Callback callback, Str key, versioned_value *e, ValAllocator *va) {
    value_type stack_val;
    value_type& val = va ? *(*va)() : stack_val;
    assign_val(val, e->read_value());
    return callback(key, val);
  }
#endif

protected:
  // range query class thang
  template <typename Nodecallback, typename Valuecallback, bool Reverse = false>
  class range_scanner {
  public:
    range_scanner(Str upper, Nodecallback nodecallback, Valuecallback valuecallback) : boundary_(upper), boundary_compar_(false),
                                                                                       nodecallback_(nodecallback), valuecallback_(valuecallback) {}

    template <typename ITER, typename KEY>
    void check(const ITER& iter,
               const KEY& key) {
      int min = std::min(boundary_.length(), key.prefix_length());
      int cmp = memcmp(boundary_.data(), key.full_string().data(), min);
      if (!Reverse) {
        if (cmp < 0 || (cmp == 0 && boundary_.length() <= key.prefix_length()))
          boundary_compar_ = true;
        else if (cmp == 0) {
          uint64_t last_ikey = iter.node()->ikey0_[iter.permutation()[iter.permutation().size() - 1]];
          uint64_t slice = string_slice<uint64_t>::make_comparable(boundary_.data() + key.prefix_length(), std::min(boundary_.length() - key.prefix_length(), 8));
          boundary_compar_ = slice <= last_ikey;
        }
      } else {
        if (cmp >= 0)
          boundary_compar_ = true;
      }
    }

    template <typename ITER>
    void visit_leaf(const ITER& iter, const Masstree::key<uint64_t>& key, threadinfo& ) {
      nodecallback_(iter.node(), iter.full_version_value());
      if (this->boundary_) {
        check(iter, key);
      }
    }
    bool visit_value(const Masstree::key<uint64_t>& key, versioned_value *value, threadinfo&) {
      if (this->boundary_compar_) {
        if ((!Reverse && boundary_ <= key.full_string()) ||
            ( Reverse && boundary_ >= key.full_string()))
          return false;
      }
      
      return valuecallback_(key.full_string(), value);
    }

    Str boundary_;
    bool boundary_compar_;
    Nodecallback nodecallback_;
    Valuecallback valuecallback_;
  };

public:

  // Non-transactional API (Masstree-shape; see
  // docs/storage-interface.md). Each op below is per-key
  // atomic: it does NOT participate in a caller's transaction.
  // put/get/insert/scan/rscan wrap a one-op OCC transaction (safe
  // under concurrent OCC readers/writers); remove (further down) is
  // a direct raw write — the asymmetry is accepted and documented in
  // the plan doc.
  // VT is templated (like transPut) so callers can pass StringWrapper
  // for zero-copy packing as well as plain value_type.
  // Returns true iff the key was newly inserted (Masstree's insert
  // convention). Note trans_write returns "key already existed", so
  // the result is inverted here.
  template <typename VT = value_type>
  bool put(Str key, const VT& value, threadinfo_type& ti = mythreadinfo) {
    // @unsafe: Sto uses thread-local global transaction state.
    Sto::start_transaction();
    auto existed = transPut(key, value, ti);
    Sto::commit();
    return !existed;
  }

  bool get(Str key, value_type& value, threadinfo_type& ti = mythreadinfo) {
    // @unsafe: Sto uses thread-local global transaction state.
    Sto::start_transaction();
    auto ret = transGet(key, value, ti);
    Sto::commit();
    return ret;
  }

  // Put-if-absent: returns true iff the key was newly inserted.
  // (transInsert returns "key already existed"; inverted here.)
  template <typename VT = value_type>
  bool insert(Str key, const VT& value, threadinfo_type& ti = mythreadinfo) {
    // @unsafe: Sto uses thread-local global transaction state.
    Sto::start_transaction();
    auto existed = transInsert(key, value, ti);
    Sto::commit();
    return !existed;
  }

  // Forward range scan over [begin, end). Callback is invoked per
  // key-value pair; return false from the callback to stop early.
  template <typename Callback, typename ValAllocator = DefaultValAllocator>
  void scan(Str begin, Str end, Callback callback, ValAllocator *va = NULL,
            threadinfo_type& ti = mythreadinfo) {
    // @unsafe: Sto uses thread-local global transaction state.
    Sto::start_transaction();
    transQuery(begin, end, callback, va, ti);
    Sto::commit();
  }

  // Reverse range scan; same contract as scan but descending order.
  template <typename Callback, typename ValAllocator = DefaultValAllocator>
  void rscan(Str begin, Str end, Callback callback, ValAllocator *va = NULL,
             threadinfo_type& ti = mythreadinfo) {
    // @unsafe: Sto uses thread-local global transaction state.
    Sto::start_transaction();
    transRQuery(begin, end, callback, va, ti);
    Sto::commit();
  }

  // implementation of TObject methods

  void lock(versioned_value *e) {
    lock(&e->version());
  }
  void unlock(versioned_value *e) {
    unlock(&e->version());
  }

    bool lock(TransItem& item, Transaction& txn) override {
        versioned_value* vv = item.key<versioned_value*>();
        return txn.try_lock(item, vv->version());
    }
  bool check(TransItem& item, Transaction&) override {
    if (is_inter(item)) {
      auto n = untag_inter(item.key<leaf_type*>());
      auto cur_version = n->full_version_value();
      auto read_version = item.template read_value<typename unlocked_cursor_type::nodeversion_value_type>();
      return cur_version == read_version;
    }
    auto e = item.key<versioned_value*>();
    auto read_version = item.template read_value<Version>();
    bool valid = validityCheck(item, e);
    if (!valid) {
      return false;
    }
    return TransactionTid::check_version(e->version(), read_version);
  }

  #define RESET_NODE_BY_E(e) \
    char *oldval_str=(char*)e->data();\
    int oldval_len=e->length();\
    mako::ResetEncodedNodeState(oldval_str, static_cast<size_t>(oldval_len));

  void install(TransItem& item, Transaction& t) override {
    assert(!is_inter(item));
    versioned_value* e = item.key<versioned_value*>();
    assert(is_locked(e->version()));
    bool isInsert = has_insert(item), isDelete = has_delete(item);

    if (isDelete) { // delete
      // Update count at commit time (under lock), so no atomics needed.
      // insert-then-delete cancels out (net change = 0); plain delete decrements.
      if (!isInsert) {
        size_count_--;
      }
      if (!TThread::is_multiversion()) {
        if (!isInsert) { // update
          assert(!(e->version() & invalid_bit));
          e->version() |= invalid_bit;
          fence();
        }

        key_write_value_type& s = item.template write_value<key_write_value_type>();
        bool success = remove(Str(s));
        // no one should be able to remove since we hold the lock
        (void)success;
        assert(success);
        return;
      }

      string v=string(1+mako::EXTRA_BITS_FOR_VALUE, 'B');
      MultiVersionValue::mvInstall(isInsert, isDelete,
                                   v,
                                   e,
                                   TThread::txn->get_current_term());
      e->set_length(v.length());
      return;
    }  // end of deletion

    if (!isInsert) { // update
        write_value_type& v = item.template write_value<write_value_type>();
        if (!TThread::is_multiversion()) {
          e->set_value(v);
          RESET_NODE_BY_E(e)
        } else {
          MultiVersionValue::mvInstall(isInsert, isDelete,
                                     v,
                                     e,
                                     TThread::txn->get_current_term());
          e->set_length(v.length());
        }
    }
    if (Opacity)  // false
      TransactionTid::set_version(e->version(), t.commit_tid());
    else if (isInsert) {  // insert
      size_count_++;
      Version v = e->version() & ~invalid_bit;
      fence();
      e->version() = v;
      if (TThread::is_multiversion())
        MultiVersionValue::mvInstall(isInsert, isDelete,
                                    "",
                                    e,
                                    TThread::txn->get_current_term());
    } else // update
      TransactionTid::inc_nonopaque_version(e->version());
      //RESET_NODE_BY_E(e)
  }

  void unlock(TransItem& item) override {
      unlock(item.key<versioned_value*>());
  }

  void cleanup(TransItem& item, bool committed) override {
      if (!committed && has_insert(item)) { // abort and for the insert value
        // we don't really need to deal with multi-version in this phase
        key_write_value_type& stdstr = item.template write_value<key_write_value_type>();
        // does not copy
        Str s(stdstr);
        bool success = remove(s);
        (void)success;
        assert(success);
    }
  }

  bool remove(const Str& key, threadinfo_type& ti = mythreadinfo) {
    cursor_type lp(table_, key);
    // finish() requires the original-node metadata initialized by
    // find_insert(), including when this operation only removes a row.
    bool found = lp.find_insert(*ti.ti);
    // Only deallocate when the key exists: on a miss the cursor's
    // value slot is uninitialized and dereferencing it is UB.
    if (found)
      lp.value()->deallocate_rcu(*ti.ti);
    lp.finish(found ? -1 : 0, *ti.ti);
    return found;
  }

protected:
  bool readMyWritesEnabled() const {
#if READ_MY_WRITES
    // This milestone covers local single-version Silo. Native Mako's remote
    // proxies and replicated multiversion participants have different staging
    // and lock-transfer protocols and must retain their prior behavior.
    return !is_remote && !TThread::is_multiversion();
#else
    return false;
#endif
  }

  template <typename ValueType>
  static const ValueType& storageValue(const ValueType& value) {
    return value;
  }

  // StringWrapper deliberately keeps the transaction's std::string out of
  // the transaction buffer. Normalize it once so sizing and in-place writes
  // use that same backing value without materializing a temporary string.
  static const std::string& storageValue(const StringWrapper& value) {
    return *value.value();
  }

  // called once we've checked our own writes for a found put()
  template <typename ValueType>
  void reallyHandlePutFound(TransProxy& item, versioned_value *e, Str key, const ValueType& value) {
    // resizing takes a lot of effort, so we first check if we'll need to
    // (values never shrink in size, so if we don't need to resize, we'll never need to)
    const auto& storage_value = storageValue(value);
    auto *new_location = e;
    bool needsResize = e->needsResize(storage_value);
    if (needsResize) {
#if READ_MY_WRITES
      // Relocation changes the transaction-set key from the old allocation to
      // its replacement. Preserve an earlier point read's observed version so
      // read-then-grow still detects an intervening writer, and retire every
      // read/write flag that could otherwise dereference the old allocation at
      // commit. An inserted row additionally carries its cleanup key forward.
      const bool preserve_ryw_state = readMyWritesEnabled();
      const bool relocate_insert = preserve_ryw_state && has_insert(item);
      const bool relocate_read = preserve_ryw_state && item.has_read();
      Version relocated_read_version{};
      if (relocate_read)
        relocated_read_version = item.template read_value<Version>();
#endif
      if (!has_insert(item)) {  // update
        // Allocate/copy while holding the old value lock, but do not invalidate
        // the published value until allocation has succeeded. In particular,
        // StandardMalloc now throws on OOM; invalidating first would leave a
        // permanently poisoned tree entry when that exception reached the ABI.
        lock(e);
        // we had a weird race condition and now this element is gone. just abort at this point
        if (e->version() & invalid_bit) {
          unlock(e);
          Sto::abort();
          return;
        }
        try {
          // Copies the old value into a larger, still-unpublished allocation.
          new_location = e->resizeIfNeeded(storage_value);
        } catch (...) {
          unlock(e);
          throw;
        }
        e->version() |= invalid_bit;
        // should be ok to unlock now because any attempted writes will be forced to abort
        unlock(e);
        // The copy was made while e carried its lock bit. Derive the replacement
        // version from the now-unlocked old entry and clear only invalid_bit.
        new_location->version() = e->version() & ~invalid_bit;
      } else {
        new_location = e->resizeIfNeeded(storage_value);
      }
      // e can't get bigger so this should always be true
      assert(new_location != e);
      cursor_type lp(table_, key);
      // TODO: not even trying to pass around threadinfo here
      // find_insert initializes the cursor bookkeeping consumed by finish().
      // find_locked alone leaves original_n_ unset, which is undefined
      // behavior even when we only replace an existing value pointer.
      bool found = lp.find_insert(*mythreadinfo.ti);
      (void)found;
      assert(found);
      lp.value() = new_location;
      lp.finish(0, *mythreadinfo.ti);
      // The active transaction's RCU epoch keeps the retired item reachable
      // to its old TransItem until abort/commit cleanup completes. Queue it
      // immediately so a later replacement-item allocation failure cannot
      // leak the now-unlinked allocation.
      e->deallocate_rcu(*mythreadinfo.ti);
#if READ_MY_WRITES
      if (preserve_ryw_state) {
        auto relocated_item = Sto::new_item(this, new_location);
        if (relocate_read)
          relocated_item.observe(tversion_type(relocated_read_version));
        if (relocate_insert)
          relocated_item.template add_write<key_write_value_type>(key)
              .add_flags(insert_bit);

        // Build the replacement item completely before relinquishing the old
        // insert's abort-cleanup ownership. If allocation or opacity checking
        // above fails, abort can still remove the newly published invalid row
        // by using the old item's retained key.
        if (item.has_write())
          item.clear_write();
        if (relocate_read)
          item.clear_read();
        if (relocate_insert)
          item.clear_flags(insert_bit);
        item = relocated_item;
      } else {
        item = Sto::new_item(this, new_location);
      }
#else
      item = Sto::new_item(this, new_location);
#endif
    }
#if READ_MY_WRITES
    if (readMyWritesEnabled() && has_insert(item)) {
      new_location->set_value(storage_value);
    } else
#endif
    {
      item.template add_write<write_value_type>(value); // write_value_type value0 = (write_value_type)value;

      item.add_extra(key) ;
    }
  }

  // returns true if already in tree, false otherwise
  // handles a transactional put when the given key is already in the tree
  template <bool INSERT, bool SET, typename ValueType>
  bool handlePutFound(versioned_value *e, Str key, const ValueType& value) {
    auto item = t_item(e);
    if (!validityCheck(item, e)) {
      Sto::abort();
      return false;
    }
#if READ_MY_WRITES
    if (readMyWritesEnabled() && has_delete(item)) {
      // delete-then-insert == update (technically v# would get set to 0, but this doesn't matter
      // if user can't read v#)
      if (INSERT) {
        item.clear_flags(delete_bit);
        assert(!has_delete(item));
        reallyHandlePutFound(item, e, key, value);
      } else {
        // delete-then-update == not found
        // delete will check for other deletes so we don't need to re-log that check
      }
      return false;
    }
#endif
    if (SET) {
      reallyHandlePutFound(item, e, key, value);
    }
    // Observe after reallyHandlePutFound so a first write that resized the
    // value records the replacement location. If there was an earlier read,
    // the relocation path already transferred its observation instead.
#if READ_MY_WRITES
    // make sure this item doesn't get deleted (we don't care about other updates to it though)
    if (!readMyWritesEnabled() ||
        (!item.has_read() && !has_insert(item)))
#endif
    {
      auto current_e = item.item().template key<versioned_value*>();
      Version v = current_e->version();
      fence();
      item.observe(tversion_type(v));
    }
    return true;
  }

  template <typename NODE, typename VERSION>
  void ensureNotFound(NODE n, VERSION v) {
    // TODO: could be more efficient to use fresh_item here, but that will also require more work for read-then-insert
    auto item = t_read_only_item(tag_inter(n));
    if (Opacity)
      item.add_read_opaque(v);
    else
      item.add_read(v);
  }

  template <typename NODE, typename VERSION>
  bool updateNodeVersion(NODE *node, VERSION prev_version, VERSION new_version) {
    if (auto node_item = Sto::check_item(this, tag_inter(node))) {
      if (node_item->has_read() &&
          prev_version == node_item->template read_value<VERSION>()) {
        node_item->update_read(node_item->template read_value<VERSION>(),
                               new_version);
        return true;
      }
    }
    return false;
  }

  template <typename T>
  TransProxy t_item(T e) {
    return Sto::item(this, e);
  }

  template <typename T>
  TransProxy t_read_only_item(T e) {
#if READ_MY_WRITES
    // RYW requires one transaction item per value. Sto::read_item may create
    // duplicate read items before the transaction's first write, which leaves
    // stale observations behind if a later growing write relocates the value.
    if (readMyWritesEnabled())
      return Sto::item(this, e);
#endif
    return Sto::fresh_item(this, e);
  }

  static bool has_insert(const TransItem& item) {
      return item.flags() & insert_bit;
  }
  static bool has_delete(const TransItem& item) {
      return item.flags() & delete_bit;
  }
  static bool has_invalidate(const TransItem& item) {
      return item.flags() & delete_bit;
  }

  bool validityCheck(const TransItem& item, versioned_value *e) const {
    bool v =  //likely(has_insert(item)) || !(e->version & invalid_bit);
      likely(!(e->version() & invalid_bit)) ||
      (readMyWritesEnabled() && has_insert(item));
    //Warning("validityCheck:%d,%d",!(e->version() & invalid_bit), has_insert(item));
    return v;
  }

  static constexpr Version invalid_bit = TransactionTid::user_bit;

  static constexpr uintptr_t internode_bit = 1<<0;

  static constexpr TransItem::flags_type insert_bit = TransItem::user0_bit;
  static constexpr TransItem::flags_type delete_bit = TransItem::user0_bit<<1;

  template <typename T>
  static T* tag_inter(T* p) {
    return (T*)((uintptr_t)p | internode_bit);
  }
  template <typename T>
  static T* untag_inter(T* p) {
    return (T*)((uintptr_t)p & ~internode_bit);
  }
  template <typename T>
  static bool is_inter(T* p) {
    return (uintptr_t)p & internode_bit;
  }
  static bool is_inter(const TransItem& t) {
      return is_inter(t.key<versioned_value*>());
  }

  static void check_opacity(Version& v) {
    Version v2 = v;
    fence();
    Sto::check_opacity(v2);
  }

  static bool is_locked(Version v) {
    return TransactionTid::is_locked(v);
  }
  static void lock(Version *v) {
    TransactionTid::lock(*v);
#if 0
    while (1) {
      Version cur = *v;
      if (!(cur&lock_bit) && bool_cmpxchg(v, cur, cur|lock_bit)) {
        break;
      }
      relax_fence();
    }
#endif
  }
  static void unlock(Version *v) {
    TransactionTid::unlock(*v);
#if 0
    assert(is_locked(*v));
    Version cur = *v;
    cur &= ~lock_bit;
    *v = cur;
#endif
  }

  static bool atomicRead(versioned_value *e, Version& vers, value_type& val) {
    Version v2;
    do {
      v2 = e->version();
      if (is_locked(v2)){
        Sto::abort_without_throw(); //Sto::abort();
        TThread::transget_without_throw=true;
        return false;
      }
	
      fence();
      assign_val(val, e->read_value());
      fence();
      vers = e->version();
      fence();
    } while (vers != v2);
    return true;
  }

  template <typename ValType>
  static void assign_val(ValType& val, const ValType& val_to_assign) {
    val = val_to_assign;
  }
  static void assign_val(std::string& val, Str val_to_assign) {
    val.assign(val_to_assign.data(), val_to_assign.length());
  }

  struct table_params : public Masstree::nodeparams<15,15> {
    typedef versioned_value* value_type;
    typedef Masstree::value_print<value_type> value_print_type;
    typedef threadinfo threadinfo_type;
  };
  typedef Masstree::basic_table<table_params> table_type;
  typedef Masstree::unlocked_tcursor<table_params> unlocked_cursor_type;
  typedef Masstree::tcursor<table_params> cursor_type;
  typedef Masstree::leaf<table_params> leaf_type;
  table_type table_;
  // @safe - approximate key count; updated at commit time (under lock), so no atomics needed.
  size_t size_count_{0};
};

template <typename V, typename Box, bool Opacity>
__thread typename MassTrans<V, Box, Opacity>::threadinfo_type MassTrans<V, Box, Opacity>::mythreadinfo;

template <typename V, typename Box, bool Opacity>
constexpr typename MassTrans<V, Box, Opacity>::Version MassTrans<V, Box, Opacity>::invalid_bit;
