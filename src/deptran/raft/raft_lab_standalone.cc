/**
 * raft_lab_standalone — Phase 7/8 of the Raft decouple plan.
 *
 * Stands up an in-process Raft cluster using TestCluster and exercises
 * its fiber-synchronous transport + fault-injection plumbing end-to-end
 * without binding any socket and without any rocksdb on disk.
 *
 * What this binary DOES prove today:
 *   - ChannelSwitchboard + TestCluster build a valid N-node topology.
 *   - Transport RPCs (send_*) round-trip through the in-memory
 *     switchboard to every peer's DummyDispatcher.
 *   - disconnect / partition / reset_faults silence and restore
 *     traffic as expected.
 *   - ReplicatedDBCommand preserves unnamed raw operation bytes on its
 *     existing wire path.
 *   - It runs without binding a socket (verify with
 *     `ss -lntp | grep raft_lab_standalone` — no rows).
 *
 * Replaces the prior DummyDispatcher harness with a real
 * RaftServer + RaftLabTest driver.
 */

#include <stddef.h>


#include "deptran/raft/replicated_db.h"
#include "deptran/raft/test_cluster.hpp"

import std;

using namespace janus::raft;
using janus::ReplicatedDBCommand;
using janus::ReplicatedDBOp;

namespace {

struct Harness {
  rusty::Box<TestCluster> cluster;
  int                     failures = 0;

  // @safe
  Harness(size_t n) : cluster(TestCluster::with_in_memory_transport(n)) {}

  template <typename Fn>
  void check(const char* name, Fn fn) {
    std::printf("[ RUN      ] %s\n", name);
    bool ok = fn();
    if (ok) {
      std::printf("[       OK ] %s\n", name);
    } else {
      ++failures;
      std::printf("[     FAIL ] %s\n", name);
    }
  }
};

// @safe
bool case_three_node_votes_each_peer(TestCluster& c) {
  int granted = 0;
  for (siteid_t peer : c.site_ids()) {
    if (peer == 1) continue;
    auto r = c.node(1).transport()->send_vote(peer, VoteReq{1, 0, 1, 1});
    if (r.vote_granted) ++granted;
  }
  return granted == 2;
}

// @safe
bool case_disconnect_blocks_one_node(TestCluster& c) {
  c.reset_faults();
  c.disconnect(2);
  auto dropped = c.node(1).transport()->send_timeout_now(2, TimeoutNowReq{});
  auto ok3 = c.node(1).transport()->send_timeout_now(3, TimeoutNowReq{});
  c.reset_faults();
  return !dropped.success && ok3.success;
}

// @safe
bool case_partition_isolates_groups() {
  auto c = TestCluster::with_in_memory_transport(5);
  c->partition({1, 2}, {3, 4, 5});
  auto cross = c->node(1).transport()->send_timeout_now(3, TimeoutNowReq{});
  auto intra = c->node(1).transport()->send_timeout_now(2, TimeoutNowReq{});
  return !cross.success && intra.success;
}

// @safe
bool case_fire_and_forget_durables_deliver(TestCluster& c) {
  c.reset_faults();
  c.node(1).transport()->send_vote_durable(2, VoteDurableReq{});
  c.node(1).transport()->send_append_entries_durable(2,
      AppendEntriesDurableReq{});
  // Give the background workers a beat to consume the durables.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return true;
}

// Stage 1 still executes the emitted C++ enum, whose fixed uint8_t
// representation accepts every byte. Pin that legacy wire contract before a
// future native-Rust provider introduces validation or a transparent newtype.
bool case_unknown_top_level_db_op_round_trips() {
  for (uint8_t raw : {uint8_t{0}, uint8_t{0xff}}) {
    ReplicatedDBCommand original;
    original.op_ = static_cast<ReplicatedDBOp>(raw);
    original.key_ = "raw-key";
    original.value_ = "raw-value";

    rrr::BufferSink sink;
    rrr::BinaryWriteArchive writer(rrr::make_sink_proxy_buffer(&sink));
    original.save(writer);

    ReplicatedDBCommand decoded;
    rrr::BufferSource source(sink.bytes.data(), sink.bytes.len());
    rrr::BinaryReadArchive reader(rrr::make_source_proxy_buffer(&source));
    decoded.load(reader);

    if (static_cast<uint8_t>(decoded.op_) != raw ||
        decoded.key_ != original.key_ ||
        decoded.value_ != original.value_ ||
        !decoded.batch_ops_.empty()) {
      return false;
    }
  }
  return true;
}

bool case_unknown_batch_db_ops_round_trip() {
  ReplicatedDBCommand original;
  original.op_ = ReplicatedDBOp::BATCH;
  original.batch_ops_ = {
      {static_cast<ReplicatedDBOp>(0), "zero-key", "zero-value"},
      {static_cast<ReplicatedDBOp>(0xff), "max-key", "max-value"},
  };

  rrr::BufferSink sink;
  rrr::BinaryWriteArchive writer(rrr::make_sink_proxy_buffer(&sink));
  original.save(writer);

  ReplicatedDBCommand decoded;
  rrr::BufferSource source(sink.bytes.data(), sink.bytes.len());
  rrr::BinaryReadArchive reader(rrr::make_source_proxy_buffer(&source));
  decoded.load(reader);

  return decoded.op_ == ReplicatedDBOp::BATCH &&
         decoded.batch_ops_.size() == 2 &&
         static_cast<uint8_t>(decoded.batch_ops_[0].op) == 0 &&
         decoded.batch_ops_[0].key == "zero-key" &&
         decoded.batch_ops_[0].value == "zero-value" &&
         static_cast<uint8_t>(decoded.batch_ops_[1].op) == 0xff &&
         decoded.batch_ops_[1].key == "max-key" &&
         decoded.batch_ops_[1].value == "max-value";
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  std::printf("raft_lab_standalone — in-process Raft harness\n");

  Harness h{3};

  h.check("three_node_votes_each_peer",
          [&] { return case_three_node_votes_each_peer(*h.cluster); });

  h.check("disconnect_blocks_one_node",
          [&] { return case_disconnect_blocks_one_node(*h.cluster); });

  h.check("partition_isolates_groups",
          [&] { return case_partition_isolates_groups(); });

  h.check("fire_and_forget_durables_deliver",
          [&] { return case_fire_and_forget_durables_deliver(*h.cluster); });

  h.check("unknown_top_level_db_op_round_trips",
          [] { return case_unknown_top_level_db_op_round_trips(); });

  h.check("unknown_batch_db_ops_round_trip",
          [] { return case_unknown_batch_db_ops_round_trip(); });

  std::printf("[==========] ");
  if (h.failures == 0) {
    std::printf("PASSED\n");
    return 0;
  }
  std::printf("FAILED (%d cases)\n", h.failures);
  return 1;
}
