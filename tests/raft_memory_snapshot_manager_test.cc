#include <gtest/gtest.h>

#include "deptran/raft/memory_snapshot_manager.hpp"

using namespace janus::raft;

TEST(MemorySnapshotManagerTest, TakeLoadRoundTrip) {
  MemorySnapshotManager mgr;
  EXPECT_FALSE(mgr.GetLatestSnapshot().is_some());

  const std::string payload = "hello snapshot";
  ASSERT_TRUE(mgr.TakeSnapshot(/*last_index=*/42, /*last_term=*/7,
                               payload.data(), payload.size()));

  auto meta = mgr.GetLatestSnapshot();
  ASSERT_TRUE(meta.is_some());
  EXPECT_EQ(meta.unwrap().last_included_index, 42u);

  SnapshotMetadata meta_out{};
  std::string data_out;
  ASSERT_TRUE(mgr.LoadLatestSnapshot(&meta_out, &data_out));
  EXPECT_EQ(meta_out.last_included_term, 7u);
  EXPECT_EQ(data_out, payload);
  EXPECT_TRUE(mgr.HasSnapshotAtOrAfter(42));
  EXPECT_FALSE(mgr.HasSnapshotAtOrAfter(43));
}

TEST(MemorySnapshotManagerTest, WriterThenReader) {
  MemorySnapshotManager mgr;
  auto w = mgr.BeginSnapshot(100, 5);
  ASSERT_NE(w, nullptr);
  const char chunk1[] = "abc";
  const char chunk2[] = "def";
  ASSERT_TRUE(w->Write(chunk1, 3));
  ASSERT_TRUE(w->Write(chunk2, 3));
  ASSERT_TRUE(w->Finalize());

  auto r = mgr.BeginLoad(SnapshotMetadata{});
  ASSERT_NE(r, nullptr);
  char buf[16] = {};
  size_t got = 0;
  ASSERT_TRUE(r->Read(buf, sizeof(buf), &got));
  EXPECT_EQ(got, 6u);
  EXPECT_EQ(std::string(buf, got), "abcdef");
  EXPECT_TRUE(r->IsComplete());
}

TEST(MemorySnapshotManagerTest, PruneAndDelete) {
  MemorySnapshotManager mgr;
  mgr.TakeSnapshot(10, 1, "x", 1);
  EXPECT_EQ(mgr.PruneSnapshots(20), 1u);      // below threshold -> pruned
  EXPECT_FALSE(mgr.GetLatestSnapshot().is_some());
  mgr.TakeSnapshot(30, 2, "y", 1);
  EXPECT_EQ(mgr.DeleteAllSnapshots(), 1u);
  EXPECT_FALSE(mgr.GetLatestSnapshot().is_some());
}
