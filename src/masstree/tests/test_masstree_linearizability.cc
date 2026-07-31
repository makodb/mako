// Tier 3.2 of docs/masstree-test-plan.md — linearizability check for the
// concurrent Masstree on a small per-key keyspace.
//
// Each thread runs a short randomized op schedule (insert /
// insert_if_absent / remove / search) on a shared concurrent_btree.
// Every op records its invocation seq and response seq from a shared
// std::atomic counter, plus its arguments and observed return value.
//
// After joining, the recorded history is sliced per-key. Ops on
// different keys commute in a key-value store, so per-key
// linearizability is sufficient.
//
// For each key, a Wing/Gong-style brute-force backtrack searches for a
// total order over that key's ops such that:
//
//   1. The order is consistent with the wall-clock partial order
//      (op A precedes op B in the order iff A.end_seq < B.begin_seq,
//      using strict less-than).
//   2. Applied left-to-right to a rusty::Option<uint64_t> register
//      oracle, every op's observed return value matches the oracle's
//      predicted return.
//
// If no such order exists for any key, the implementation is
// non-linearizable and the test fails with the offending key, seed,
// and ops.
//
// Schedule sizing is conservative (≤ ~15 ops per key with light
// concurrency) so backtracking finishes well under the CTest timeout.

#include <stdint.h>
#include <stddef.h>

#include <gtest/gtest.h>

#include <rusty/option.hpp>
#include <rusty/thread.hpp>
#include <rusty/vec.hpp>

#include "masstree/kvthread.hh"
#include "mako/masstree_btree.h"
#include "mako/varkey.h"

import std;
import rusty;

volatile mrcu_epoch_type globalepoch = 1;

using TestTree = concurrent_btree;

namespace {

inline u64_varkey K(uint64_t i) { return u64_varkey(i); }

inline TestTree::value_type ToValue(uint64_t v) {
  return reinterpret_cast<TestTree::value_type>(static_cast<uintptr_t>(v));
}
inline uint64_t FromValue(TestTree::value_type v) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v));
}

enum class OpKind : int {
  Insert = 0,
  InsertIfAbsent = 1,
  Remove = 2,
  Search = 3,
};

struct Op {
  OpKind kind;
  uint64_t key;
  uint64_t value;        // input for Insert / InsertIfAbsent
  uint64_t begin_seq;
  uint64_t end_seq;
  int thread_id;
  bool ret_bool;         // recorded return
  uint64_t ret_value;    // recorded value for Search hits
};

using KeyState = rusty::Option<uint64_t>;

struct ApplyResult {
  bool ok;
  KeyState new_state;
};

ApplyResult Apply(const Op& op, KeyState state) {
  ApplyResult r{false, std::move(state)};
  switch (op.kind) {
    case OpKind::Insert: {
      const bool expected = !r.new_state.is_some();
      r.ok = (op.ret_bool == expected);
      r.new_state = op.value;
      break;
    }
    case OpKind::InsertIfAbsent: {
      const bool expected = !r.new_state.is_some();
      r.ok = (op.ret_bool == expected);
      if (expected) r.new_state = op.value;
      break;
    }
    case OpKind::Remove: {
      const bool expected = r.new_state.is_some();
      r.ok = (op.ret_bool == expected);
      r.new_state = rusty::None;
      break;
    }
    case OpKind::Search: {
      const bool expected_found = r.new_state.is_some();
      if (op.ret_bool != expected_found) {
        r.ok = false;
      } else if (op.ret_bool &&
                 std::as_const(r.new_state).unwrap() != op.ret_value) {
        r.ok = false;
      } else {
        r.ok = true;
      }
      // Search does not mutate state.
      break;
    }
  }
  return r;
}

bool TryLinearize(rusty::Vec<Op*>& pending, KeyState state) {
  if (pending.is_empty()) return true;

  for (size_t i = 0; i < pending.len(); ++i) {
    Op* candidate = pending[i];

    // Check candidate has no required predecessor among pending ops.
    bool has_predecessor = false;
    for (Op* other : pending) {
      if (other != candidate && other->end_seq < candidate->begin_seq) {
        has_predecessor = true;
        break;
      }
    }
    if (has_predecessor) continue;

    // state must be cloned because Apply takes by value (it constructs
    // ApplyResult.new_state from the move) and we still need the original
    // for sibling branches in the search.
    auto ar = Apply(*candidate, state.clone());
    if (!ar.ok) continue;

    // Remove candidate, recurse.
    std::swap(pending[i], pending.back());
    auto _ = pending.pop();
    if (TryLinearize(pending, std::move(ar.new_state))) return true;
    pending.push(candidate);
    std::swap(pending[i], pending.back());
  }
  return false;
}

bool CheckPerKeyLinearizability(rusty::Vec<Op>& ops_for_key) {
  // Sort by begin_seq for a more constrained (faster) search.
  std::sort(ops_for_key.begin(), ops_for_key.end(),
            [](const Op& a, const Op& b) { return a.begin_seq < b.begin_seq; });
  auto pending = rusty::Vec<Op*>::with_capacity(ops_for_key.len());
  for (auto& o : ops_for_key) pending.push(&o);
  return TryLinearize(pending, rusty::None);
}

const char* KindName(OpKind k) {
  switch (k) {
    case OpKind::Insert:         return "insert";
    case OpKind::InsertIfAbsent: return "insert_if_absent";
    case OpKind::Remove:         return "remove";
    case OpKind::Search:         return "search";
  }
  return "?";
}

std::string DumpOps(const rusty::Vec<Op>& ops) {
  std::ostringstream os;
  for (const auto& o : ops) {
    os << "  t" << o.thread_id << " [" << o.begin_seq << "," << o.end_seq << "] "
       << KindName(o.kind) << " k=" << o.key;
    if (o.kind == OpKind::Insert || o.kind == OpKind::InsertIfAbsent) {
      os << " v=" << o.value;
    }
    os << " -> " << (o.ret_bool ? "true" : "false");
    if (o.kind == OpKind::Search && o.ret_bool) os << "/" << o.ret_value;
    os << "\n";
  }
  return os.str();
}

void RunLinearizabilitySession(uint64_t seed) {
  std::mt19937_64 master_rng(seed);
  TestTree tree;

  std::atomic<uint64_t> clock{0};
  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 30;
  constexpr int kKeyspace = 8;

  auto per_thread = rusty::Vec<rusty::Vec<Op>>::with_capacity(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    per_thread.push(rusty::Vec<Op>::with_capacity(kOpsPerThread));
  }

  std::array<uint64_t, kThreads> thread_seeds{};
  for (auto& s : thread_seeds) s = master_rng();

  auto thread_body = [&](int tid) {
    std::mt19937_64 rng(thread_seeds[tid]);
    std::uniform_int_distribution<uint64_t> pick_key(0, kKeyspace - 1);
    std::uniform_int_distribution<int> pick_op(0, 3);
    std::uniform_int_distribution<uint64_t> any_val(1, 1ull << 32);

    auto& log = per_thread[tid];
    for (int i = 0; i < kOpsPerThread; ++i) {
      Op op{};
      op.thread_id = tid;
      op.key = pick_key(rng);
      op.kind = static_cast<OpKind>(pick_op(rng));
      if (op.kind == OpKind::Insert || op.kind == OpKind::InsertIfAbsent) {
        op.value = any_val(rng);
      }

      op.begin_seq = clock.fetch_add(1, std::memory_order_seq_cst);

      switch (op.kind) {
        case OpKind::Insert:
          op.ret_bool = tree.insert(K(op.key), ToValue(op.value));
          break;
        case OpKind::InsertIfAbsent:
          op.ret_bool = tree.insert_if_absent(K(op.key), ToValue(op.value));
          break;
        case OpKind::Remove:
          op.ret_bool = tree.remove(K(op.key));
          break;
        case OpKind::Search: {
          TestTree::value_type out = nullptr;
          op.ret_bool = tree.search(K(op.key), out);
          if (op.ret_bool) op.ret_value = FromValue(out);
          break;
        }
      }

      op.end_seq = clock.fetch_add(1, std::memory_order_seq_cst);
      log.push(op);
    }
  };

  auto threads = rusty::Vec<rusty::thread::JoinHandle<rusty::thread::Unit>>::with_capacity(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.push(rusty::thread::spawn(thread_body, t));
  }
  for (auto& th : threads) { auto _ = th.join(); }

  // Bucket ops by key.
  auto per_key = rusty::BTreeMap<uint64_t, rusty::Vec<Op>>::new_();
  for (auto& pt : per_thread) {
    for (auto& op : pt) {
      if (!per_key.contains_key(op.key)) {
        per_key.insert(op.key, rusty::Vec<Op>::new_());
      }
      per_key.get_mut(op.key).unwrap().push(op);
    }
  }

  rusty::Vec<uint64_t> keys;
  {
    auto key_it = per_key.iter();
    while (true) {
      auto key_e = key_it.next();
      if (key_e.is_none()) break;
      keys.push(std::get<0>(key_e.unwrap()));
    }
  }
  for (size_t ki = 0; ki < keys.len(); ++ki) {
    const uint64_t k = keys[ki];
    rusty::Vec<Op>& ops = per_key.get_mut(k).unwrap();
    const bool ok = CheckPerKeyLinearizability(ops);
    ASSERT_TRUE(ok)
        << "non-linearizable history at key=" << k
        << " seed=0x" << std::hex << seed << std::dec
        << " ops=" << ops.len() << "\n"
        << DumpOps(ops);
  }
}

}  // namespace

class MasstreeLinearizability : public ::testing::TestWithParam<uint64_t> {};

INSTANTIATE_TEST_SUITE_P(
    Seeds, MasstreeLinearizability,
    ::testing::Values<uint64_t>(0xC0FFEEull, 0xDEADBEEFull, 0xFEEDFACEull,
                                0xCAFEBABEull, 0xBADDCAFEull, 0x5EED1234ull,
                                0xA11C0DEull, 0x1234567890ABCDEFull),
    [](const ::testing::TestParamInfo<uint64_t>& info) {
      char buf[24];
      std::snprintf(buf, sizeof(buf), "seed_%016llx",
                    static_cast<unsigned long long>(info.param));
      return std::string(buf);
    });

TEST_P(MasstreeLinearizability, PerKeyIsLinearizable) {
  RunLinearizabilitySession(GetParam());
}
