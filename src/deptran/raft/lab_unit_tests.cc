// The three RaftLab cases that are unit tests of C++ classes.
//
// testSnapshotMetadataCreation, testSnapshotFormatRoundTrip and
// testSnapshotManagerSaveLoad construct a SnapshotMetadata, call
// SnapshotFormat's statics, and exercise a MemorySnapshotManager on the stack.
// They never name a RaftServer, so moving them to Rust would add roughly ten
// kernels and delete none of the 41 lab exports
// (docs/migration/raft/lab-harness-to-rust-plan.md, Phase 4). They test C++
// classes; they stay C++.
//
// The Rust suite calls them through raft_lab_cpp_unit_tests. They are
// transcribed from test.cc rather than shared with it, because test.cc is
// deleted at Phase 4 and these are not.

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "server.h"
#include "snapshot_manager.hpp"
#include "snapshot_format.hpp"
#include "memory_snapshot_manager.hpp"

import std;
import rusty;

namespace janus {

#ifdef RAFT_TEST_CORO

namespace {

int lab_unit_test_id = 0;

// testconf.h's Print/Init/Passed/Failed, self-contained: ci.sh greps for
// `^TEST [0-9]* Passed`, so the markers must match byte for byte.
void UnitInit(int test_id, const char* description) {
  fprintf(stderr, "TEST %d: %s\n", test_id, description);
  lab_unit_test_id = test_id;
}
void UnitPassed() {
  fprintf(stderr, "TEST %d Passed\n", lab_unit_test_id);
}
void UnitFailed(const char* msg) {
  fprintf(stderr, "TEST %d Failed: %s\n", lab_unit_test_id, msg);
}

#define UnitAssert(expr, msg) \
  if (!(expr)) { UnitFailed(msg); return 1; }

// Test 50 -- SnapshotMetadata creation and field access
int TestSnapshotMetadataCreation() {
  UnitInit(50, "Snapshot metadata creation and field access");

  janus::raft::SnapshotMetadata meta;
  UnitAssert(meta.last_included_index == 0, "Default last_included_index should be 0");
  UnitAssert(meta.last_included_term == 0, "Default last_included_term should be 0");
  UnitAssert(meta.size_bytes == 0, "Default size_bytes should be 0");
  UnitAssert(!meta.is_valid(), "Default metadata should not be valid");

  meta.last_included_index = 42;
  meta.last_included_term = 3;
  meta.size_bytes = 1024;
  meta.timestamp_ms = 1234567890;
  UnitAssert(meta.is_valid(), "Metadata with index > 0 should be valid");
  UnitAssert(meta.last_included_index == 42, "last_included_index should be 42");
  UnitAssert(meta.last_included_term == 3, "last_included_term should be 3");

  auto str = meta.to_string();
  UnitAssert(str.find("42") != std::string::npos, "to_string should contain index 42");
  UnitAssert(str.find("1024") != std::string::npos, "to_string should contain size 1024");

  UnitPassed();
  return 0;
}

// Test 51 -- SnapshotFormat serialize/deserialize round-trip
int TestSnapshotFormatRoundTrip() {
  UnitInit(51, "Snapshot format serialize/deserialize round-trip");

  std::string test_data = "hello snapshot world! This is state machine data.";
  const uint64_t test_index = 100;
  const uint64_t test_term = 5;

  std::string serialized;
  bool ok = janus::raft::SnapshotFormat::Serialize(
      test_index, test_term, test_data.data(), test_data.size(), &serialized);
  UnitAssert(ok, "Serialize should succeed");
  UnitAssert(serialized.size() > sizeof(janus::raft::SnapshotHeader),
             "Serialized data should be larger than header");

  janus::raft::SnapshotHeader header;
  ok = janus::raft::SnapshotFormat::GetHeader(
      serialized.data(), serialized.size(), &header);
  UnitAssert(ok, "GetHeader should succeed");
  UnitAssert(header.last_index == test_index, "Header last_index mismatch");
  UnitAssert(header.last_term == test_term, "Header last_term mismatch");
  UnitAssert(header.data_size == test_data.size(), "Header data_size mismatch");

  uint64_t out_index = 0;
  uint64_t out_term = 0;
  std::string out_data;
  ok = janus::raft::SnapshotFormat::Deserialize(
      serialized.data(), serialized.size(), &out_index, &out_term, &out_data);
  UnitAssert(ok, "Deserialize should succeed");
  UnitAssert(out_index == test_index, "Deserialized index mismatch");
  UnitAssert(out_term == test_term, "Deserialized term mismatch");
  UnitAssert(out_data == test_data, "Deserialized data should match original");

  std::string empty_serialized;
  ok = janus::raft::SnapshotFormat::Serialize(1, 1, nullptr, 0, &empty_serialized);
  UnitAssert(ok, "Serialize with empty data should succeed");
  ok = janus::raft::SnapshotFormat::Deserialize(
      empty_serialized.data(), empty_serialized.size(),
      &out_index, &out_term, &out_data);
  UnitAssert(ok, "Deserialize empty data should succeed");
  UnitAssert(out_data.empty(), "Empty snapshot data should deserialize to empty string");

  std::string corrupted = serialized;
  corrupted[sizeof(janus::raft::SnapshotHeader) + 5] ^= 0xFF;  // flip a data byte
  ok = janus::raft::SnapshotFormat::Deserialize(
      corrupted.data(), corrupted.size(), &out_index, &out_term, &out_data);
  UnitAssert(!ok, "Deserialize of corrupted data should fail");

  UnitPassed();
  return 0;
}

// Test 52 -- MemorySnapshotManager save/load round-trip
int TestSnapshotManagerSaveLoad() {
  UnitInit(52, "SnapshotManager save/load round-trip");

  janus::raft::MemorySnapshotManager mgr;
  UnitAssert(!mgr.HasSnapshotAtOrAfter(1), "Should have no snapshots initially");
  UnitAssert(mgr.GetLatestSnapshot().is_none(), "Latest should be None initially");

  std::string data1 = "state machine data at index 10";
  bool ok = mgr.TakeSnapshot(10, 2, data1.data(), data1.size());
  UnitAssert(ok, "TakeSnapshot should succeed");

  UnitAssert(mgr.HasSnapshotAtOrAfter(1), "Should have snapshot after save");
  UnitAssert(mgr.HasSnapshotAtOrAfter(10), "Should have snapshot at index 10");
  UnitAssert(!mgr.HasSnapshotAtOrAfter(11), "Should not have snapshot at index 11");

  janus::raft::SnapshotMetadata loaded_meta;
  std::string loaded_data;
  ok = mgr.LoadLatestSnapshot(&loaded_meta, &loaded_data);
  UnitAssert(ok, "LoadLatestSnapshot should succeed");
  UnitAssert(loaded_meta.last_included_index == 10, "Loaded index should be 10");
  UnitAssert(loaded_meta.last_included_term == 2, "Loaded term should be 2");
  UnitAssert(loaded_data == data1, "Loaded data should match saved data");

  std::string data2 = "state machine data at index 25";
  ok = mgr.TakeSnapshot(25, 3, data2.data(), data2.size());
  UnitAssert(ok, "Second TakeSnapshot should succeed");

  ok = mgr.LoadLatestSnapshot(&loaded_meta, &loaded_data);
  UnitAssert(ok, "LoadLatestSnapshot after second save should succeed");
  UnitAssert(loaded_meta.last_included_index == 25, "Latest should be index 25");
  UnitAssert(loaded_data == data2, "Latest data should be the second snapshot");

  mgr.DeleteAllSnapshots();
  UnitPassed();
  return 0;
}

}  // namespace

// The three, in the order RaftLabTest::Run runs them. 0 on success.
extern "C" int raft_lab_cpp_unit_tests() {
  int result = TestSnapshotMetadataCreation();
  if (result != 0) return result;
  result = TestSnapshotFormatRoundTrip();
  if (result != 0) return result;
  return TestSnapshotManagerSaveLoad();
}

#endif  // RAFT_TEST_CORO

}  // namespace janus
