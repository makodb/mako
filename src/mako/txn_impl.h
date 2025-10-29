#ifndef _NDB_TXN_IMPL_H_
#define _NDB_TXN_IMPL_H_

#include "txn.h"
#include "lockguard.h"

// base definitions

template <template <typename> class Protocol, typename Traits>
transaction<Protocol, Traits>::transaction(uint64_t flags, string_allocator_type &sa)
  : transaction_base(flags), sa(&sa)
{
  INVARIANT(rcu::s_instance.in_rcu_region());
#ifdef BTREE_LOCK_OWNERSHIP_CHECKING
  concurrent_btree::NodeLockRegionBegin();
#endif
}

template <template <typename> class Protocol, typename Traits>
transaction<Protocol, Traits>::~transaction()
{
  // transaction shouldn't fall out of scope w/o resolution
  // resolution means TXN_EMBRYO, TXN_COMMITED, and TXN_ABRT
  INVARIANT(state != TXN_ACTIVE);
  INVARIANT(rcu::s_instance.in_rcu_region());
  const unsigned cur_depth = rcu_guard_->sync()->depth();
  rcu_guard_.destroy();
  if (cur_depth == 1) {
    INVARIANT(!rcu::s_instance.in_rcu_region());
    cast()->on_post_rcu_region_completion();
  }
#ifdef BTREE_LOCK_OWNERSHIP_CHECKING
  concurrent_btree::AssertAllNodeLocksReleased();
#endif
}

template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::clear()
{
  // it's actually *more* efficient to not call clear explicitly on the
  // read/write/absent sets, and let the destructors do the clearing- this is
  // because the destructors can take shortcuts since it knows the obj doesn't
  // have to end in a valid state
}

template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::abort_impl(abort_reason reason)
{
  abort_trap(reason);
  switch (state) {
  case TXN_EMBRYO:
  case TXN_ACTIVE:
    break;
  case TXN_ABRT:
    return;
  case TXN_COMMITED:
    throw transaction_unusable_exception();
  }
  state = TXN_ABRT;
  this->reason = reason;

  // On abort, we need to go over all insert nodes and release the locks
  typename write_set_map::iterator write_iter = write_set.begin();
  typename write_set_map::iterator write_end = write_set.end();
  for (; write_iter != write_end; ++write_iter) {
    dbtuple * const current_tuple = write_iter->get_tuple();
    if (write_iter->is_insert()) {
      INVARIANT(current_tuple->is_locked());
      this->cleanup_inserted_tuple_marker(current_tuple, write_iter->get_key(), write_iter->get_btree());
      current_tuple->unlock();
    }
  }

  clear();
}

template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::cleanup_inserted_tuple_marker(
    dbtuple *marker, const std::string &key, concurrent_btree *btr)
{
  // XXX: this code should really live in txn_proto2_impl.h
  INVARIANT(marker->version == dbtuple::MAX_TID);
  INVARIANT(marker->is_locked());
  INVARIANT(marker->is_lock_owner());
  typename concurrent_btree::value_type removed = 0;
  const bool did_remove = btr->remove(varkey(key), &removed);
  if (unlikely(!did_remove)) {
#ifdef CHECK_INVARIANTS
    std::cerr << " *** could not remove key: " << util::hexify(key)  << std::endl;
#ifdef TUPLE_CHECK_KEY
    std::cerr << " *** original key        : " << util::hexify(marker->key) << std::endl;
#endif
#endif
    ALWAYS_ASSERT(false);
  }
  INVARIANT(removed == (typename concurrent_btree::value_type) marker);
  INVARIANT(marker->is_latest());
  marker->clear_latest();
  dbtuple::release(marker); // rcu free
}

namespace {
  inline const char *
  transaction_state_to_cstr(transaction_base::txn_state state)
  {
    switch (state) {
    case transaction_base::TXN_EMBRYO: return "TXN_EMBRYO";
    case transaction_base::TXN_ACTIVE: return "TXN_ACTIVE";
    case transaction_base::TXN_ABRT: return "TXN_ABRT";
    case transaction_base::TXN_COMMITED: return "TXN_COMMITED";
    }
    ALWAYS_ASSERT(false);
    return 0;
  }

  inline std::string
  transaction_flags_to_str(uint64_t flags)
  {
    bool first = true;
    std::ostringstream oss;
    if (flags & transaction_base::TXN_FLAG_LOW_LEVEL_SCAN) {
      oss << "TXN_FLAG_LOW_LEVEL_SCAN";
      first = false;
    }
    if (flags & transaction_base::TXN_FLAG_READ_ONLY) {
      if (first)
        oss << "TXN_FLAG_READ_ONLY";
      else
        oss << " | TXN_FLAG_READ_ONLY";
      first = false;
    }
    return oss.str();
  }
}

template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::dump_debug_info() const
{
  std::cerr << "Transaction (obj=" << util::hexify(this) << ") -- state "
       << transaction_state_to_cstr(state) << std::endl;
  std::cerr << "  Abort Reason: " << AbortReasonStr(reason) << std::endl;
  std::cerr << "  Flags: " << transaction_flags_to_str(flags) << std::endl;
  std::cerr << "  Read/Write sets:" << std::endl;

  std::cerr << "      === Read Set ===" << std::endl;
  // read-set
  for (typename read_set_map::const_iterator rs_it = read_set.begin();
       rs_it != read_set.end(); ++rs_it)
    std::cerr << *rs_it << std::endl;

  std::cerr << "      === Write Set ===" << std::endl;
  // write-set
  for (typename write_set_map::const_iterator ws_it = write_set.begin();
       ws_it != write_set.end(); ++ws_it)
    std::cerr << *ws_it << std::endl;

  std::cerr << "      === Absent Set ===" << std::endl;
  // absent-set
  for (typename absent_set_map::const_iterator as_it = absent_set.begin();
       as_it != absent_set.end(); ++as_it)
    std::cerr << "      B-tree Node " << util::hexify(as_it->first)
              << " : " << as_it->second << std::endl;

}

template <template <typename> class Protocol, typename Traits>
std::map<std::string, uint64_t>
transaction<Protocol, Traits>::get_txn_counters() const
{
  std::map<std::string, uint64_t> ret;

  // max_read_set_size
  ret["read_set_size"] = read_set.size();;
  ret["read_set_is_large?"] = !read_set.is_small_type();

  // max_absent_set_size
  ret["absent_set_size"] = absent_set.size();
  ret["absent_set_is_large?"] = !absent_set.is_small_type();

  // max_write_set_size
  ret["write_set_size"] = write_set.size();
  ret["write_set_is_large?"] = !write_set.is_small_type();

  return ret;
}

template <template <typename> class Protocol, typename Traits>
bool
transaction<Protocol, Traits>::handle_last_tuple_in_group(
    dbtuple_write_info &last,
    bool did_group_insert)
{
  if (did_group_insert) {
    // don't need to lock
    if (!last.is_insert())
      // we inserted the last run, and then we did 1+ more overwrites
      // to it, so we do NOT need to lock the node (again), but we DO
      // need to apply the latest write
      last.entry->set_do_write();
  } else {
    dbtuple *tuple = last.get_tuple();
    if (unlikely(tuple->version == dbtuple::MAX_TID)) {
      // if we race to put/insert w/ another txn which has inserted a new
      // record, we *must* abort b/c the other txn could try to put/insert
      // into a new record which we hold the lock on, so we must abort
      //
      // other ideas:
      // we could *not* abort if this txn did not insert any new records.
      // we could also release our insert locks and try to acquire them
      // again in sorted order
      return false; // signal abort
    }
    const dbtuple::version_t v = tuple->lock(true); // lock for write
    INVARIANT(dbtuple::IsLatest(v) == tuple->is_latest());
    last.mark_locked();
    if (unlikely(!dbtuple::IsLatest(v) ||
                 !cast()->can_read_tid(tuple->version))) {
      // XXX(stephentu): overly conservative (with the can_read_tid() check)
      return false; // signal abort
    }
    last.entry->set_do_write();
  }
  return true;
}

// Helper function to validate transaction state before commit
template <template <typename> class Protocol, typename Traits>
bool
transaction<Protocol, Traits>::validate_transaction_state(bool doThrow)
{
  switch (state) {
  case TXN_EMBRYO:
  case TXN_ACTIVE:
    return true; // Valid states for commit
  case TXN_COMMITED:
    return false; // Already committed, return success
  case TXN_ABRT:
    if (doThrow)
      throw transaction_abort_exception(reason);
    return false; // Already aborted
  }
  return true;
}

// Helper function to prepare write set for commit
template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::prepare_write_set(dbtuple_write_info_vec &write_tuples)
{
  if (write_set.empty())
    return;
    
  PERF_DECL(
      static std::string probe1_name(
        std::string(__PRETTY_FUNCTION__) + std::string(":lock_write_nodes:")));
  ANON_REGION(probe1_name.c_str(), &transaction_base::g_txn_commit_probe1_cg);
  INVARIANT(!is_snapshot());
  
  typename write_set_map::iterator write_iter = write_set.begin();
  typename write_set_map::iterator write_end = write_set.end();
  for (size_t position = 0; write_iter != write_end; ++write_iter, ++position) {
    INVARIANT(!write_iter->is_insert() || write_iter->get_tuple()->is_locked());
    write_tuples.emplace_back(write_iter->get_tuple(), &(*write_iter), write_iter->is_insert(), position);
  }
}

// Helper function to lock write nodes in sorted order
template <template <typename> class Protocol, typename Traits>
bool
transaction<Protocol, Traits>::lock_write_nodes(dbtuple_write_info_vec &write_tuples, std::pair<bool, tid_t> &commit_tid_pair)
{
  if (write_tuples.empty())
    return true;
    
  PERF_DECL(
      static std::string probe2_name(
        std::string(__PRETTY_FUNCTION__) + std::string(":lock_write_nodes:")));
  ANON_REGION(probe2_name.c_str(), &transaction_base::g_txn_commit_probe2_cg);
  
  // Sort write nodes for consistent locking order
  {
    PERF_DECL(
        static std::string probe6_name(
          std::string(__PRETTY_FUNCTION__) + std::string(":sort_write_nodes:")));
    ANON_REGION(probe6_name.c_str(), &transaction_base::g_txn_commit_probe6_cg);
    write_tuples.sort(); // in-place
  }
  
  typename dbtuple_write_info_vec::iterator tuple_iter = write_tuples.begin();
  typename dbtuple_write_info_vec::iterator tuple_end = write_tuples.end();
  dbtuple_write_info *previous_tuple_info = nullptr;
  bool inserted_in_last_run = false;
  
  for (; tuple_iter != tuple_end; previous_tuple_info = &(*tuple_iter), ++tuple_iter) {
    if (likely(previous_tuple_info && previous_tuple_info->tuple != tuple_iter->tuple)) {
      // On boundary between different tuples
      if (unlikely(!handle_last_tuple_in_group(*previous_tuple_info, inserted_in_last_run))) {
        abort_trap((reason = ABORT_REASON_WRITE_NODE_INTERFERENCE));
        return false;
      }
      inserted_in_last_run = false;
    }
    if (tuple_iter->is_insert()) {
      INVARIANT(!previous_tuple_info || previous_tuple_info->tuple != tuple_iter->tuple);
      INVARIANT(tuple_iter->is_locked());
      INVARIANT(tuple_iter->get_tuple()->is_locked());
      INVARIANT(tuple_iter->get_tuple()->is_lock_owner());
      tuple_iter->entry->set_do_write(); // all inserts are marked do-write
      inserted_in_last_run = true;
    } else {
      INVARIANT(!tuple_iter->is_locked());
    }
  }
  
  if (likely(previous_tuple_info) &&
      unlikely(!handle_last_tuple_in_group(*previous_tuple_info, inserted_in_last_run))) {
    abort_trap((reason = ABORT_REASON_WRITE_NODE_INTERFERENCE));
    return false;
  }
  
  commit_tid_pair.first = true;
  PERF_DECL(
      static std::string probe5_name(
        std::string(__PRETTY_FUNCTION__) + std::string(":gen_commit_tid:")));
  ANON_REGION(probe5_name.c_str(), &transaction_base::g_txn_commit_probe5_cg);
  commit_tid_pair.second = cast()->gen_commit_tid(write_tuples);
  VERBOSE(std::cerr << "commit tid: " << g_proto_version_str(commit_tid_pair.second) << std::endl);
  
  return true;
}

// Helper function to validate read set
template <template <typename> class Protocol, typename Traits>
bool
transaction<Protocol, Traits>::validate_read_set(const dbtuple_write_info_vec &write_tuples)
{
  PERF_DECL(
      static std::string probe3_name(
        std::string(__PRETTY_FUNCTION__) + std::string(":read_validation:")));
  ANON_REGION(probe3_name.c_str(), &transaction_base::g_txn_commit_probe3_cg);

  // Check the nodes we actually read are still the latest version
  if (!read_set.empty()) {
    typename read_set_map::iterator read_iter = read_set.begin();
    typename read_set_map::iterator read_end = read_set.end();
    for (; read_iter != read_end; ++read_iter) {
      VERBOSE(std::cerr << "validating dbtuple " << util::hexify(read_iter->get_tuple())
                        << " at snapshot_tid "
                        << g_proto_version_str(cast()->snapshot_tid())
                        << std::endl);

      const bool tuple_found = sorted_dbtuples_contains(write_tuples, read_iter->get_tuple());
      if (likely(tuple_found ?
            read_iter->get_tuple()->is_latest_version(read_iter->get_tid()) :
            read_iter->get_tuple()->stable_is_latest_version(read_iter->get_tid())))
        continue;

      VERBOSE(std::cerr << "validating dbtuple " << util::hexify(read_iter->get_tuple()) << " at snapshot_tid "
                        << g_proto_version_str(cast()->snapshot_tid()) << " FAILED" << std::endl
                        << "  txn read version: " << g_proto_version_str(read_iter->get_tid()) << std::endl
                        << "  tuple=" << *read_iter->get_tuple() << std::endl);

      abort_trap((reason = ABORT_REASON_READ_NODE_INTEREFERENCE));
      return false;
    }
  }

  // Check btree versions have not changed
  if (!absent_set.empty()) {
    typename absent_set_map::iterator absent_iter = absent_set.begin();
    typename absent_set_map::iterator absent_end = absent_set.end();
    for (; absent_iter != absent_end; ++absent_iter) {
      const uint64_t current_version = concurrent_btree::ExtractVersionNumber(absent_iter->first);
      if (unlikely(current_version != absent_iter->second.version)) {
        VERBOSE(std::cerr << "expected node " << util::hexify(absent_iter->first) << " at v="
                          << absent_iter->second.version << ", got v=" << current_version << std::endl);
        abort_trap((reason = ABORT_REASON_NODE_SCAN_READ_VERSION_CHANGED));
        return false;
      }
    }
  }
  
  return true;
}

// Helper function to commit write records
template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::commit_write_records(dbtuple_write_info_vec &write_tuples, tid_t commit_transaction_id)
{
  if (write_tuples.empty())
    return;
    
  PERF_DECL(
      static std::string probe4_name(
        std::string(__PRETTY_FUNCTION__) + std::string(":write_records:")));
  ANON_REGION(probe4_name.c_str(), &transaction_base::g_txn_commit_probe4_cg);
  
  typename write_set_map::iterator write_iter = write_set.begin();
  typename write_set_map::iterator write_end = write_set.end();
  for (; write_iter != write_end; ++write_iter) {
    if (unlikely(!write_iter->do_write()))
      continue;
      
    dbtuple * const current_tuple = write_iter->get_tuple();
    INVARIANT(current_tuple->is_locked());
    VERBOSE(std::cerr << "writing dbtuple " << util::hexify(current_tuple)
                      << " at commit_tid " << g_proto_version_str(commit_transaction_id)
                      << std::endl);
                      
    if (write_iter->is_insert()) {
      INVARIANT(current_tuple->version == dbtuple::MAX_TID);
      current_tuple->version = commit_transaction_id; // allows write_record_ret() to succeed w/o creating a new chain
    } else {
      current_tuple->prefetch();
      const dbtuple::write_record_ret write_result =
        current_tuple->write_record_at(
            cast(), commit_transaction_id,
            write_iter->get_value(), write_iter->get_writer());
      bool should_unlock_head = false;
      
      if (unlikely(write_result.head_ != current_tuple)) {
        // Tuple was replaced by write_result.head_
        INVARIANT(write_result.rest_ == current_tuple);
        // XXX: write_record_at() should acquire this lock
        write_result.head_->lock(true);
        should_unlock_head = true;
        
        // Need to unlink tuple from underlying btree, replacing with write_result.rest_ (atomically)
        typename concurrent_btree::value_type old_btree_value = 0;
        if (write_iter->get_btree()->insert(
              varkey(write_iter->get_key()), (typename concurrent_btree::value_type) write_result.head_, &old_btree_value, NULL))
          // Should already exist in tree
          INVARIANT(false);
        INVARIANT(old_btree_value == (typename concurrent_btree::value_type) current_tuple);
        // We don't RCU free this, because it is now part of the chain (the cleaners will take care of this)
        ++evt_dbtuple_latest_replacement;
      }
      
      if (unlikely(write_result.rest_))
        // Spill happened: schedule GC task
        cast()->on_dbtuple_spill(write_result.head_, write_result.rest_);
      if (!write_iter->get_value())
        // Logical delete happened: schedule GC task
        cast()->on_logical_delete(write_result.head_, write_iter->get_key(), write_iter->get_btree());
      if (unlikely(should_unlock_head))
        write_result.head_->unlock();
    }
    VERBOSE(std::cerr << "dbtuple " << util::hexify(current_tuple) << " is_locked? " << current_tuple->is_locked() << std::endl);
  }
  
  // Unlock all write tuples
  // NB: we can no longer unlock after doing the writes above
  for (typename dbtuple_write_info_vec::iterator tuple_iter = write_tuples.begin();
       tuple_iter != write_tuples.end(); ++tuple_iter) {
    if (tuple_iter->is_locked())
      tuple_iter->tuple->unlock();
    else
      INVARIANT(!tuple_iter->is_insert());
  }
}

// Helper function to handle commit abort cleanup
template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::handle_commit_abort(dbtuple_write_info_vec &write_tuples, 
                                                   std::pair<bool, tid_t> &commit_tid_pair, 
                                                   bool doThrow)
{
  // XXX: these values are possibly uninitialized
  if (this->is_snapshot())
    VERBOSE(std::cerr << "aborting txn @ snapshot_tid " << cast()->snapshot_tid() << std::endl);
  else
    VERBOSE(std::cerr << "aborting txn" << std::endl);

  for (typename dbtuple_write_info_vec::iterator tuple_iter = write_tuples.begin();
       tuple_iter != write_tuples.end(); ++tuple_iter) {
    if (tuple_iter->is_locked()) {
      if (tuple_iter->is_insert()) {
        INVARIANT(tuple_iter->entry);
        this->cleanup_inserted_tuple_marker(
            tuple_iter->tuple.get(), tuple_iter->entry->get_key(), tuple_iter->entry->get_btree());
      }
      // XXX: potential optimization: on unlock() for abort, we don't
      // technically need to change the version number
      tuple_iter->tuple->unlock();
    } else {
      INVARIANT(!tuple_iter->is_insert());
    }
  }

  state = TXN_ABRT;
  if (commit_tid_pair.first)
    cast()->on_tid_finish(commit_tid_pair.second);
  clear();
  if (doThrow)
    throw transaction_abort_exception(reason);
}

template <template <typename> class Protocol, typename Traits>
bool
transaction<Protocol, Traits>::commit(bool doThrow)
{
#ifdef TUPLE_MAGIC
  try {
#endif

  PERF_DECL(
      static std::string probe0_name(
        std::string(__PRETTY_FUNCTION__) + std::string(":total:")));
  ANON_REGION(probe0_name.c_str(), &transaction_base::g_txn_commit_probe0_cg);

  // Validate transaction state
  if (!validate_transaction_state(doThrow)) {
    if (state == TXN_COMMITED)
      return true;
    return false;
  }

  dbtuple_write_info_vec write_tuples;
  std::pair<bool, tid_t> commit_tid_pair(false, 0);

  // Prepare write set for commit
  prepare_write_set(write_tuples);

  // Read-only transactions require consistent snapshots
  INVARIANT(!is_snapshot() || read_set.empty());
  INVARIANT(!is_snapshot() || write_set.empty());
  INVARIANT(!is_snapshot() || absent_set.empty());
  
  if (!is_snapshot()) {
    // We don't have consistent tids, or not a read-only transaction

    // Lock write nodes in sorted order
    if (!lock_write_nodes(write_tuples, commit_tid_pair)) {
      goto do_abort;
    }
    
    if (write_tuples.empty()) {
      VERBOSE(std::cerr << "commit tid: <read-only>" << std::endl);
    }

    // Perform read validation
    if (!validate_read_set(write_tuples)) {
      goto do_abort;
    }

    // Commit actual records
    commit_write_records(write_tuples, commit_tid_pair.second);
  }
  
  // Transaction committed successfully
  state = TXN_COMMITED;
  if (commit_tid_pair.first)
    cast()->on_tid_finish(commit_tid_pair.second);
  clear();
  return true;

do_abort:
  // Handle transaction abort cleanup
  handle_commit_abort(write_tuples, commit_tid_pair, doThrow);
  return false;

#ifdef TUPLE_MAGIC
  } catch (dbtuple::magic_failed_exception &) {
    dump_debug_info();
    ALWAYS_ASSERT(false);
  }
#endif
}

template <template <typename> class Protocol, typename Traits>
std::pair< dbtuple *, bool >
transaction<Protocol, Traits>::try_insert_new_tuple(
    concurrent_btree &btree_index,
    const std::string *tuple_key,
    const void *tuple_value,
    dbtuple::tuple_writer_t tuple_writer)
{
  INVARIANT(tuple_key);
  const size_t required_size =
    tuple_value ? tuple_writer(dbtuple::TUPLE_WRITER_COMPUTE_NEEDED,
      tuple_value, nullptr, 0) : 0;

  // perf: ~900 tsc/alloc on istc11.csail.mit.edu
  dbtuple * const new_tuple = dbtuple::alloc_first(required_size, true);
  if (tuple_value)
    tuple_writer(dbtuple::TUPLE_WRITER_DO_WRITE,
        tuple_value, new_tuple->get_value_start(), 0);
  INVARIANT(find_read_set(new_tuple) == read_set.end());
  INVARIANT(new_tuple->is_latest());
  INVARIANT(new_tuple->version == dbtuple::MAX_TID);
  INVARIANT(new_tuple->is_locked());
  INVARIANT(new_tuple->is_write_intent());
#ifdef TUPLE_CHECK_KEY
  new_tuple->key.assign(tuple_key->data(), tuple_key->size());
  new_tuple->tree = (void *) &btree_index;
#endif

  // XXX: underlying btree api should return the existing value if insert
  // fails- this would allow us to avoid having to do another search
  typename concurrent_btree::insert_info_t btree_insert_info;
  if (unlikely(!btree_index.insert_if_absent(
          varkey(*tuple_key), (typename concurrent_btree::value_type) new_tuple, &btree_insert_info))) {
    VERBOSE(std::cerr << "insert_if_absent failed for key: " << util::hexify(tuple_key) << std::endl);
    new_tuple->clear_latest();
    new_tuple->unlock();
    dbtuple::release_no_rcu(new_tuple);
    ++transaction_base::g_evt_dbtuple_write_insert_failed;
    return std::pair< dbtuple *, bool >(nullptr, false);
  }
  VERBOSE(std::cerr << "insert_if_absent suceeded for key: " << util::hexify(tuple_key) << std::endl
                    << "  new dbtuple is " << util::hexify(new_tuple) << std::endl);
  // Update write_set
  // too expensive to be practical
  // INVARIANT(find_write_set(new_tuple) == write_set.end());
  write_set.emplace_back(new_tuple, tuple_key, tuple_value, tuple_writer, &btree_index, true);

  // Update node version numbers
  INVARIANT(btree_insert_info.node);
  if (!absent_set.empty()) {
    auto absent_iter = absent_set.find(btree_insert_info.node);
    if (absent_iter != absent_set.end()) {
      if (unlikely(absent_iter->second.version != btree_insert_info.old_version)) {
        abort_trap((reason = ABORT_REASON_WRITE_NODE_INTERFERENCE));
        return std::make_pair(new_tuple, true);
      }
      VERBOSE(std::cerr << "bump node=" << util::hexify(absent_iter->first) << " from v=" << btree_insert_info.old_version
                        << " -> v=" << btree_insert_info.new_version << std::endl);
      // Otherwise, bump the version
      absent_iter->second.version = btree_insert_info.new_version;
      SINGLE_THREADED_INVARIANT(concurrent_btree::ExtractVersionNumber(absent_iter->first) == absent_iter->second);
    }
  }
  return std::make_pair(new_tuple, false);
}

// Helper function to handle read validation failures with consistent abort pattern
template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::handle_read_validation_failure(transaction_base::abort_reason abort_reason)
{
  abort_impl(abort_reason);
  throw transaction_abort_exception(abort_reason);
}

template <template <typename> class Protocol, typename Traits>
template <typename ValueReader>
bool
transaction<Protocol, Traits>::do_tuple_read(
    const dbtuple *tuple, ValueReader &value_reader)
{
  INVARIANT(tuple);
  ++evt_local_search_lookups;

  const bool is_snapshot_transaction = is_snapshot();
  const transaction_base::tid_t snapshot_transaction_id = is_snapshot_transaction ?
    cast()->snapshot_tid() : static_cast<transaction_base::tid_t>(dbtuple::MAX_TID);
  transaction_base::tid_t read_start_tid = 0;

  if (Traits::read_own_writes) {
    // This is why read_own_writes is not performant, because we have to do linear scan
    auto write_set_iterator = find_write_set(const_cast<dbtuple *>(tuple));
    if (unlikely(write_set_iterator != write_set.end())) {
      ++evt_local_search_write_set_hits;
      if (!write_set_iterator->get_value())
        return false;
      const typename ValueReader::value_type * const written_value =
        reinterpret_cast<const typename ValueReader::value_type *>(
            write_set_iterator->get_value());
      value_reader.dup(*written_value, this->string_allocator());
      return true;
    }
  }

  // Perform the actual tuple read
  dbtuple::ReadStatus read_status;
  {
    PERF_DECL(static std::string probe0_name(std::string(__PRETTY_FUNCTION__) + std::string(":do_read:")));
    ANON_REGION(probe0_name.c_str(), &private_::txn_btree_search_probe0_cg);
    tuple->prefetch();
    read_status = tuple->stable_read(snapshot_transaction_id, read_start_tid, value_reader, this->string_allocator(), is_snapshot_transaction);
    if (unlikely(read_status == dbtuple::READ_FAILED)) {
      handle_read_validation_failure(transaction_base::ABORT_REASON_UNSTABLE_READ);
    }
  }
  if (unlikely(!cast()->can_read_tid(read_start_tid))) {
    handle_read_validation_failure(transaction_base::ABORT_REASON_FUTURE_TID_READ);
  }
  INVARIANT(read_status == dbtuple::READ_EMPTY ||
            read_status == dbtuple::READ_RECORD);
  const bool is_value_empty = (read_status == dbtuple::READ_EMPTY);
  if (is_value_empty)
    ++transaction_base::g_evt_read_logical_deleted_node_search;
  if (!is_snapshot_transaction)
    // Read-only transactions do not need read-set tracking
    // (because we know the values are consistent)
    read_set.emplace_back(tuple, read_start_tid);
  return !is_value_empty;
}

template <template <typename> class Protocol, typename Traits>
void
transaction<Protocol, Traits>::do_node_read(
    const typename concurrent_btree::node_opaque_t *btree_node, uint64_t node_version)
{
  INVARIANT(btree_node);
  if (is_snapshot())
    return;
  auto absent_set_iterator = absent_set.find(btree_node);
  if (absent_set_iterator == absent_set.end()) {
    absent_set[btree_node].version = node_version;
  } else if (absent_set_iterator->second.version != node_version) {
    handle_read_validation_failure(transaction_base::ABORT_REASON_NODE_SCAN_READ_VERSION_CHANGED);
  }
}

#endif /* _NDB_TXN_IMPL_H_ */
