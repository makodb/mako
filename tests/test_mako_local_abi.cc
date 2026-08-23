// Contract tests for the pure-C local STO/MassTrans boundary.

#include "mako/storage/mako_local_abi.h"
#include "mako/sto/Transaction.hh"

#include <atomic>
#include <barrier>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

std::atomic<uint64_t> next_table_id{10000};

struct HookObservation {
  int calls = 0;
  uint32_t timestamp = 0;
};

int accept_hook(void *context, uint32_t timestamp) {
  auto *observation = static_cast<HookObservation *>(context);
  observation->calls++;
  observation->timestamp = timestamp;
  return 1;
}

int reject_hook(void *context, uint32_t timestamp) {
  accept_hook(context, timestamp);
  return 0;
}

int throwing_hook(void *, uint32_t) {
  throw 7;
}

class LocalAbiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(mako_local_thread_attach(), MAKO_LOCAL_OK);
    ASSERT_EQ(mako_local_db_open(&db), MAKO_LOCAL_OK);
    ASSERT_NE(db, nullptr);
    open_table("primary", &primary);
  }

  void TearDown() override {
    if (txn_for_cleanup != nullptr) {
      EXPECT_EQ(mako_local_txn_destroy(txn_for_cleanup), MAKO_LOCAL_OK);
      txn_for_cleanup = nullptr;
    }
    if (db != nullptr) EXPECT_EQ(mako_local_db_close(db), MAKO_LOCAL_OK);
  }

  void open_table(const std::string &name, mako_local_table **out) {
    ASSERT_EQ(mako_local_table_open(
                  db, reinterpret_cast<const uint8_t *>(name.data()),
                  name.size(), next_table_id.fetch_add(1), out),
              MAKO_LOCAL_OK);
    ASSERT_NE(*out, nullptr);
  }

  mako_local_txn *begin() {
    mako_local_txn *txn = nullptr;
    EXPECT_EQ(mako_local_txn_begin(db, &txn), MAKO_LOCAL_OK);
    EXPECT_NE(txn, nullptr);
    txn_for_cleanup = txn;
    return txn;
  }

  static int put(mako_local_txn *txn, mako_local_table *table,
                 const std::string &key, const std::string &value,
                 uint8_t *created = nullptr) {
    uint8_t local_created = 0;
    return mako_local_txn_put(
        txn, table, reinterpret_cast<const uint8_t *>(key.data()), key.size(),
        reinterpret_cast<const uint8_t *>(value.data()), value.size(),
        created == nullptr ? &local_created : created);
  }

  static std::pair<int, std::optional<std::string>> get(
      mako_local_txn *txn, mako_local_table *table, const std::string &key) {
    uint8_t *bytes = nullptr;
    size_t len = 0;
    uint8_t found = 0;
    int status = mako_local_txn_get(
        txn, table, reinterpret_cast<const uint8_t *>(key.data()), key.size(),
        &bytes, &len, &found);
    std::optional<std::string> value;
    if (status == MAKO_LOCAL_OK && found)
      value.emplace(reinterpret_cast<const char *>(bytes), len);
    mako_local_bytes_free(bytes);
    return {status, std::move(value)};
  }

  void destroy_tracked(mako_local_txn *txn) {
    const int status = mako_local_txn_destroy(txn);
    EXPECT_EQ(status, MAKO_LOCAL_OK);
    if (status == MAKO_LOCAL_OK && txn_for_cleanup == txn)
      txn_for_cleanup = nullptr;
  }

  void abort_and_destroy(mako_local_txn *txn) {
    EXPECT_EQ(mako_local_txn_abort(txn), MAKO_LOCAL_OK);
    destroy_tracked(txn);
  }

  void commit_and_destroy(mako_local_txn *txn) {
    EXPECT_EQ(mako_local_txn_commit(txn), MAKO_LOCAL_OK);
    destroy_tracked(txn);
  }

  mako_local_db *db = nullptr;
  mako_local_table *primary = nullptr;
  mako_local_txn *txn_for_cleanup = nullptr;
};

TEST(MakoLocalAbiIdentity, VersionAndStatusStringsAreStable) {
  EXPECT_EQ(mako_local_abi_version(), MAKO_LOCAL_ABI_VERSION);
  const uint64_t features = mako_local_feature_bits();
  EXPECT_NE(features & MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS, 0U);
#if READ_MY_WRITES
  EXPECT_NE(features & MAKO_LOCAL_FEATURE_READ_MY_WRITES, 0U);
#else
  EXPECT_EQ(features & MAKO_LOCAL_FEATURE_READ_MY_WRITES, 0U);
#endif
#if STO_OPACITY
  EXPECT_NE(features & MAKO_LOCAL_FEATURE_OPACITY, 0U);
#else
  EXPECT_EQ(features & MAKO_LOCAL_FEATURE_OPACITY, 0U);
#endif
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_OK), "ok");
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_CONFLICT),
               "transaction conflict");
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_TXN_TOO_LARGE),
               "transaction exceeds the draft item budget");
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_VALUE_TOO_LARGE),
               "table name, key, or value exceeds the draft byte limit");
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_COMMIT_HOOK_REJECTED),
               "post-validation commit hook rejected transaction");
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_TIMESTAMP_EXHAUSTED),
               "Mako logical timestamp exhausted");
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(0),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(UINT32_MAX),
            MAKO_LOCAL_TIMESTAMP_EXHAUSTED);
  static_assert(MAKO_LOCAL_MAX_MAKO_TIMESTAMP ==
                (std::numeric_limits<uint32_t>::max() - 9) / 10);
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(
                MAKO_LOCAL_MAX_MAKO_TIMESTAMP),
            MAKO_LOCAL_TIMESTAMP_EXHAUSTED);
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(
                MAKO_LOCAL_MAX_MAKO_TIMESTAMP + 1),
            MAKO_LOCAL_TIMESTAMP_EXHAUSTED);
  EXPECT_EQ(mako_local_db_open(nullptr), MAKO_LOCAL_INVALID_ARGUMENT);

  auto *poison_table = reinterpret_cast<mako_local_table *>(uintptr_t{1});
  EXPECT_EQ(mako_local_table_open(nullptr, nullptr, 0, 0, &poison_table),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(poison_table, nullptr);
  auto *poison_txn = reinterpret_cast<mako_local_txn *>(uintptr_t{1});
  EXPECT_EQ(mako_local_txn_begin(nullptr, &poison_txn),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(poison_txn, nullptr);
  uint8_t result = 99;
  EXPECT_EQ(mako_local_txn_put(nullptr, nullptr, nullptr, 0, nullptr, 0,
                               &result),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(result, 0);
}

TEST(MakoLocalAbiIdentity, RecoveryLeavesTheFinalTimestampMintable) {
  auto &clock = sync_util::sync_logger::local_replica_id;
  const uint32_t saved = clock.exchange(1, std::memory_order_acq_rel);

  EXPECT_EQ(mako_local_advance_mako_timestamp_past(
                MAKO_LOCAL_MAX_MAKO_TIMESTAMP - 1),
            MAKO_LOCAL_OK);
  uint32_t timestamp = 0;
  EXPECT_TRUE(Transaction::try_allocate_mako_timestamp(timestamp));
  EXPECT_EQ(timestamp, MAKO_LOCAL_MAX_MAKO_TIMESTAMP);
  EXPECT_FALSE(Transaction::try_allocate_mako_timestamp(timestamp));
  EXPECT_EQ(timestamp, 0U);

  // This test owns the clock exclusively; restore the suite's real progress.
  clock.store(saved, std::memory_order_release);
}

TEST_F(LocalAbiTest, MultiKeyMultiTableCommitIsVisibleTogether) {
  mako_local_table *secondary = nullptr;
  open_table("secondary", &secondary);

  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "a", "one"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "b", "two"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, secondary, "c", "three"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "a"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("one")}));
  EXPECT_EQ(get(txn, primary, "b"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("two")}));
  EXPECT_EQ(get(txn, secondary, "c"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("three")}));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PostValidationHookCarriesMonotonicMakoTimestamp) {
  constexpr uint32_t recovered_max = UINT32_C(1) << 24;
  ASSERT_EQ(mako_local_advance_mako_timestamp_past(recovered_max),
            MAKO_LOCAL_OK);

  HookObservation first;
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "hook-first", "one"), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_txn_commit_with_hook(txn, accept_hook, &first),
            MAKO_LOCAL_OK);
  destroy_tracked(txn);
  EXPECT_EQ(first.calls, 1);
  EXPECT_GT(first.timestamp, recovered_max);
  EXPECT_LE(first.timestamp, MAKO_LOCAL_MAX_MAKO_TIMESTAMP);

  HookObservation second;
  txn = begin();
  ASSERT_EQ(put(txn, primary, "hook-second", "two"), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_txn_commit_with_hook(txn, accept_hook, &second),
            MAKO_LOCAL_OK);
  destroy_tracked(txn);
  EXPECT_EQ(second.calls, 1);
  EXPECT_GT(second.timestamp, first.timestamp);

  // Advancing to a smaller observed value is monotonic and harmless.
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(first.timestamp),
            MAKO_LOCAL_OK);

  HookObservation read_only;
  txn = begin();
  EXPECT_EQ(get(txn, primary, "hook-first").second,
            std::optional<std::string>("one"));
  EXPECT_EQ(mako_local_txn_commit_with_hook(txn, accept_hook, &read_only),
            MAKO_LOCAL_OK);
  destroy_tracked(txn);
  EXPECT_EQ(read_only.calls, 0);
}

TEST_F(LocalAbiTest, HookRejectionAndExceptionDefinitelyAbortBeforeInstall) {
  HookObservation rejected;
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "hook-reject", "invisible"), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_txn_commit_with_hook(txn, reject_hook, &rejected),
            MAKO_LOCAL_COMMIT_HOOK_REJECTED);
  destroy_tracked(txn);
  EXPECT_EQ(rejected.calls, 1);
  EXPECT_NE(rejected.timestamp, 0U);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "hook-reject").second.has_value());
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "hook-throw", "also-invisible"), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_txn_commit_with_hook(txn, throwing_hook, nullptr),
            MAKO_LOCAL_COMMIT_HOOK_REJECTED);
  destroy_tracked(txn);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "hook-throw").second.has_value());
  ASSERT_EQ(put(txn, primary, "after-hook-reject", "works"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, TableNamesAndNumericIdsAreBothUnique) {
  const std::string name = "identity";
  const std::string other_name = "identity-other";
  const uint64_t id = next_table_id.fetch_add(1);
  mako_local_table *first = nullptr;
  ASSERT_EQ(mako_local_table_open(
                db, reinterpret_cast<const uint8_t *>(name.data()), name.size(),
                id, &first),
            MAKO_LOCAL_OK);
  ASSERT_NE(first, nullptr);

  mako_local_table *same = nullptr;
  EXPECT_EQ(mako_local_table_open(
                db, reinterpret_cast<const uint8_t *>(name.data()), name.size(),
                id, &same),
            MAKO_LOCAL_OK);
  EXPECT_EQ(same, first);

  auto *poison = reinterpret_cast<mako_local_table *>(uintptr_t{1});
  EXPECT_EQ(mako_local_table_open(
                db, reinterpret_cast<const uint8_t *>(name.data()), name.size(),
                id + 1, &poison),
            MAKO_LOCAL_WRONG_DB_OR_TABLE);
  EXPECT_EQ(poison, nullptr);

  poison = reinterpret_cast<mako_local_table *>(uintptr_t{1});
  EXPECT_EQ(mako_local_table_open(
                db, reinterpret_cast<const uint8_t *>(other_name.data()),
                other_name.size(), id, &poison),
            MAKO_LOCAL_WRONG_DB_OR_TABLE);
  EXPECT_EQ(poison, nullptr);
}

TEST_F(LocalAbiTest, OversizedInputsAreNonterminalAndLeaveOutputsInitialized) {
  const std::string oversized_name(MAKO_LOCAL_MAX_TABLE_NAME_BYTES + 1, 'n');
  auto *table_poison = reinterpret_cast<mako_local_table *>(uintptr_t{1});
  EXPECT_EQ(mako_local_table_open(
                db, reinterpret_cast<const uint8_t *>(oversized_name.data()),
                oversized_name.size(), next_table_id.fetch_add(1),
                &table_poison),
            MAKO_LOCAL_VALUE_TOO_LARGE);
  EXPECT_EQ(table_poison, nullptr);

  const std::string oversized_key(MAKO_LOCAL_MAX_KEY_BYTES + 1, 'k');
  const std::string oversized_value(MAKO_LOCAL_MAX_VALUE_BYTES + 1, 'v');
  auto *txn = begin();
  uint8_t created = 99;
  EXPECT_EQ(put(txn, primary, oversized_key, "value", &created),
            MAKO_LOCAL_VALUE_TOO_LARGE);
  EXPECT_EQ(created, 0);
  created = 99;
  EXPECT_EQ(put(txn, primary, "large-value", oversized_value, &created),
            MAKO_LOCAL_VALUE_TOO_LARGE);
  EXPECT_EQ(created, 0);

  // VALUE_TOO_LARGE is validation-only: no native operation ran and the
  // transaction remains usable.
  EXPECT_EQ(put(txn, primary, "after-large", "works", &created),
            MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, WeightedItemBudgetAbortsBeforeTheStoHardLimit) {
  auto *txn = begin();
  for (size_t i = 0; i != MAKO_LOCAL_TXN_ITEM_BUDGET; ++i) {
    const auto read = get(txn, primary, "same-missing-key");
    ASSERT_EQ(read.first, MAKO_LOCAL_OK) << "read " << i;
    ASSERT_FALSE(read.second.has_value());
  }
  EXPECT_EQ(get(txn, primary, "same-missing-key").first,
            MAKO_LOCAL_TXN_TOO_LARGE);
  EXPECT_EQ(get(txn, primary, "same-missing-key").first,
            MAKO_LOCAL_TXN_FINISHED);
  destroy_tracked(txn);

  // A budget failure is terminal for that transaction, not for its worker.
  txn = begin();
  ASSERT_EQ(put(txn, primary, "after-budget", "works"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, AttachRefreshesAnIdleLegacyStoTransactionThreadId) {
  struct Result {
    bool legacy_commit = false;
    int attach = MAKO_LOCAL_INTERNAL;
    int attached_id = -1;
    int begin = MAKO_LOCAL_INTERNAL;
    int commit = MAKO_LOCAL_INTERNAL;
    int destroy = MAKO_LOCAL_INTERNAL;
  } result;

  std::thread worker([&] {
    // Simulate old direct STO code that left a reusable transaction object in
    // TLS without claiming the new shared runtime.
    TThread::set_mode(0);
    TThread::set_id(MAX_THREADS - 1);
    Sto::start_transaction();
    result.legacy_commit = Sto::try_commit_no_paxos();
    Transaction::rcu_quiesce();

    result.attach = mako_local_thread_attach();
    result.attached_id = TThread::id();
    mako_local_txn *txn = nullptr;
    if (result.attach == MAKO_LOCAL_OK)
      result.begin = mako_local_txn_begin(db, &txn);
    if (result.begin == MAKO_LOCAL_OK)
      result.commit = mako_local_txn_commit(txn);
    if (txn != nullptr)
      result.destroy = mako_local_txn_destroy(txn);
  });
  worker.join();

  EXPECT_TRUE(result.legacy_commit);
  EXPECT_EQ(result.attach, MAKO_LOCAL_OK);
  EXPECT_NE(result.attached_id, MAX_THREADS - 1);
  EXPECT_EQ(result.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(result.commit, MAKO_LOCAL_OK);
  EXPECT_EQ(result.destroy, MAKO_LOCAL_OK);
}

TEST_F(LocalAbiTest, AbortAndDestroyOfActiveTransactionBothRollBack) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "abort", "invisible"), MAKO_LOCAL_OK);
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "abort"),
            (std::pair<int, std::optional<std::string>>{MAKO_LOCAL_OK,
                                                        std::nullopt}));
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "drop", "also-invisible"), MAKO_LOCAL_OK);
  destroy_tracked(txn);
  txn = begin();
  EXPECT_FALSE(get(txn, primary, "drop").second.has_value());
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, MissingPresentEmptyAndBinaryValuesStayDistinct) {
  const std::string binary_key("k\0y", 3);
  const std::string binary_value("v\0x\xff", 4);

  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "empty", ""), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, binary_key, binary_value), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  auto missing = get(txn, primary, "missing");
  EXPECT_EQ(missing.first, MAKO_LOCAL_OK);
  EXPECT_FALSE(missing.second.has_value());
  auto empty = get(txn, primary, "empty");
  ASSERT_TRUE(empty.second.has_value());
  EXPECT_TRUE(empty.second->empty());
  EXPECT_EQ(get(txn, primary, binary_key).second,
            std::optional<std::string>(binary_value));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, EveryStagedWriteOwnsADistinctStableBuffer) {
  // Regression for the old Redis FFI, where every StringWrapper pointed at
  // one reusable std::string and all keys committed with the last value.
  const std::string short_value = "first";  // exercises SSO
  const std::string long_value(4096, 'z');
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "short", "old-short"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "long", "old-long"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "third", "old-third"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "short", short_value), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "long", long_value), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "third", "different"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "short").second,
            std::optional<std::string>(short_value));
  EXPECT_EQ(get(txn, primary, "long").second,
            std::optional<std::string>(long_value));
  EXPECT_EQ(get(txn, primary, "third").second,
            std::optional<std::string>("different"));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, AbortingALargerValueResizePreservesTheOldValue) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "resize-abort", "old"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "resize-abort", std::string(8192, 'n')),
            MAKO_LOCAL_OK);
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "resize-abort").second,
            std::optional<std::string>("old"));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PutInsertAndRemoveReportExistence) {
  uint8_t result = 0;
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "k", "one", &result), MAKO_LOCAL_OK);
  EXPECT_EQ(result, 1);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "k", "two", &result), MAKO_LOCAL_OK);
  EXPECT_EQ(result, 0);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(mako_local_txn_insert(
                txn, primary, reinterpret_cast<const uint8_t *>("k"), 1,
                reinterpret_cast<const uint8_t *>("ignored"), 7, &result),
            MAKO_LOCAL_OK);
  EXPECT_EQ(result, 0);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(mako_local_txn_remove(
                txn, primary, reinterpret_cast<const uint8_t *>("k"), 1,
                &result),
            MAKO_LOCAL_OK);
  EXPECT_EQ(result, 1);
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "k").second.has_value());
  commit_and_destroy(txn);
}

#if READ_MY_WRITES
TEST_F(LocalAbiTest, PointReadYourWritesSeesNewAndExistingPuts) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-put-existing", "old"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  uint8_t created = 99;
  ASSERT_EQ(put(txn, primary, "ryw-put-existing", "updated", &created),
            MAKO_LOCAL_OK);
  EXPECT_EQ(created, 0);
  EXPECT_EQ(get(txn, primary, "ryw-put-existing"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("updated")}));

  created = 99;
  ASSERT_EQ(put(txn, primary, "ryw-put-new", "created", &created),
            MAKO_LOCAL_OK);
  EXPECT_EQ(created, 1);
  EXPECT_EQ(get(txn, primary, "ryw-put-new"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("created")}));
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-put-existing").second,
            std::optional<std::string>("updated"));
  EXPECT_EQ(get(txn, primary, "ryw-put-new").second,
            std::optional<std::string>("created"));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointReadYourWritesNewPutAbortsCleanly) {
  auto *txn = begin();
  uint8_t created = 99;
  ASSERT_EQ(put(txn, primary, "ryw-put-abort", "temporary", &created),
            MAKO_LOCAL_OK);
  EXPECT_EQ(created, 1);
  EXPECT_EQ(get(txn, primary, "ryw-put-abort"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("temporary")}));
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-put-abort"),
            (std::pair<int, std::optional<std::string>>{MAKO_LOCAL_OK,
                                                        std::nullopt}));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointReadYourWritesSeesInsertedValuesBeforeCommitOrAbort) {
  uint8_t inserted = 99;
  auto *txn = begin();
  ASSERT_EQ(mako_local_txn_insert(
                txn, primary,
                reinterpret_cast<const uint8_t *>("ryw-insert-abort"), 16,
                reinterpret_cast<const uint8_t *>("temporary"), 9, &inserted),
            MAKO_LOCAL_OK);
  EXPECT_EQ(inserted, 1);
  EXPECT_EQ(get(txn, primary, "ryw-insert-abort").second,
            std::optional<std::string>("temporary"));
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-insert-abort"),
            (std::pair<int, std::optional<std::string>>{MAKO_LOCAL_OK,
                                                        std::nullopt}));
  commit_and_destroy(txn);

  inserted = 99;
  txn = begin();
  ASSERT_EQ(mako_local_txn_insert(
                txn, primary,
                reinterpret_cast<const uint8_t *>("ryw-insert-commit"), 17,
                reinterpret_cast<const uint8_t *>("durable"), 7, &inserted),
            MAKO_LOCAL_OK);
  EXPECT_EQ(inserted, 1);
  EXPECT_EQ(get(txn, primary, "ryw-insert-commit").second,
            std::optional<std::string>("durable"));
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-insert-commit").second,
            std::optional<std::string>("durable"));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointReadYourWritesHidesRemovedValuesBeforeCommitOrAbort) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-remove-abort", "keep"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "ryw-remove-commit", "delete"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  uint8_t existed = 99;
  txn = begin();
  ASSERT_EQ(mako_local_txn_remove(
                txn, primary,
                reinterpret_cast<const uint8_t *>("ryw-remove-abort"), 16,
                &existed),
            MAKO_LOCAL_OK);
  EXPECT_EQ(existed, 1);
  EXPECT_EQ(get(txn, primary, "ryw-remove-abort"),
            (std::pair<int, std::optional<std::string>>{MAKO_LOCAL_OK,
                                                        std::nullopt}));
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-remove-abort").second,
            std::optional<std::string>("keep"));
  commit_and_destroy(txn);

  existed = 99;
  txn = begin();
  ASSERT_EQ(mako_local_txn_remove(
                txn, primary,
                reinterpret_cast<const uint8_t *>("ryw-remove-commit"), 17,
                &existed),
            MAKO_LOCAL_OK);
  EXPECT_EQ(existed, 1);
  EXPECT_EQ(get(txn, primary, "ryw-remove-commit"),
            (std::pair<int, std::optional<std::string>>{MAKO_LOCAL_OK,
                                                        std::nullopt}));
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-remove-commit"),
            (std::pair<int, std::optional<std::string>>{MAKO_LOCAL_OK,
                                                        std::nullopt}));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointReadYourWritesPreservesEmptyAndBinaryValues) {
  const std::string binary_key("ryw\0key", 7);
  const std::string binary_value("value\0\xff", 7);

  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-empty", ""), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, binary_key, binary_value), MAKO_LOCAL_OK);

  auto empty = get(txn, primary, "ryw-empty");
  ASSERT_EQ(empty.first, MAKO_LOCAL_OK);
  ASSERT_TRUE(empty.second.has_value());
  EXPECT_TRUE(empty.second->empty());
  EXPECT_EQ(get(txn, primary, binary_key).second,
            std::optional<std::string>(binary_value));
  commit_and_destroy(txn);

  txn = begin();
  empty = get(txn, primary, "ryw-empty");
  ASSERT_TRUE(empty.second.has_value());
  EXPECT_TRUE(empty.second->empty());
  EXPECT_EQ(get(txn, primary, binary_key).second,
            std::optional<std::string>(binary_value));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointReadYourWritesHandlesLargeResizeOnCommitAndAbort) {
  const std::string aborted_value(8192, 'a');
  const std::string committed_value(16384, 'c');

  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-resize-abort", "old-abort"),
            MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "ryw-resize-commit", "old-commit"),
            MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-resize-abort", aborted_value),
            MAKO_LOCAL_OK);
  EXPECT_EQ(get(txn, primary, "ryw-resize-abort").second,
            std::optional<std::string>(aborted_value));
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-resize-abort").second,
            std::optional<std::string>("old-abort"));
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-resize-commit", committed_value),
            MAKO_LOCAL_OK);
  EXPECT_EQ(get(txn, primary, "ryw-resize-commit").second,
            std::optional<std::string>(committed_value));
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-resize-commit").second,
            std::optional<std::string>(committed_value));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointReadThenGrowingPutPreservesValidation) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-read-resize", "old"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  const std::string grown_value(8192, 'g');
  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-read-resize"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("old")}));
  EXPECT_EQ(get(txn, primary, "ryw-read-resize"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("old")}));
  ASSERT_EQ(put(txn, primary, "ryw-read-resize", grown_value),
            MAKO_LOCAL_OK);
  EXPECT_EQ(get(txn, primary, "ryw-read-resize").second,
            std::optional<std::string>(grown_value));
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-read-resize").second,
            std::optional<std::string>(grown_value));
  commit_and_destroy(txn);
}
#endif

TEST_F(LocalAbiTest, NestedBeginAndWrongDatabaseTableAreRejected) {
  auto *txn = begin();
  mako_local_txn *nested = nullptr;
  EXPECT_EQ(mako_local_txn_begin(db, &nested),
            MAKO_LOCAL_TXN_ALREADY_ACTIVE);
  EXPECT_EQ(nested, nullptr);

  mako_local_db *other_db = nullptr;
  mako_local_table *other_table = nullptr;
  ASSERT_EQ(mako_local_db_open(&other_db), MAKO_LOCAL_OK);
  const std::string name = "other";
  ASSERT_EQ(mako_local_table_open(
                other_db, reinterpret_cast<const uint8_t *>(name.data()),
                name.size(), next_table_id.fetch_add(1), &other_table),
            MAKO_LOCAL_OK);
  uint8_t created = 0;
  EXPECT_EQ(put(txn, other_table, "x", "y", &created),
            MAKO_LOCAL_WRONG_DB_OR_TABLE);
  abort_and_destroy(txn);
  EXPECT_EQ(mako_local_db_close(other_db), MAKO_LOCAL_OK);
}

TEST_F(LocalAbiTest, WrongThreadAndFinishedHandlesAreRejected) {
  auto *txn = begin();
  std::atomic<int> wrong_thread_status{MAKO_LOCAL_INTERNAL};
  std::thread other([&] {
    uint8_t *value = nullptr;
    size_t value_len = 0;
    uint8_t found = 0;
    wrong_thread_status.store(mako_local_txn_get(
        txn, primary, reinterpret_cast<const uint8_t *>("key"), 3, &value,
        &value_len, &found));
    mako_local_bytes_free(value);
  });
  other.join();
  EXPECT_EQ(wrong_thread_status.load(), MAKO_LOCAL_WRONG_THREAD);

  ASSERT_EQ(mako_local_txn_commit(txn), MAKO_LOCAL_OK);
  EXPECT_EQ(put(txn, primary, "after", "commit"), MAKO_LOCAL_TXN_FINISHED);
  EXPECT_EQ(mako_local_txn_abort(txn), MAKO_LOCAL_TXN_FINISHED);
  destroy_tracked(txn);
}

TEST_F(LocalAbiTest, DuplicateWritesAreRejectedWithoutCorruptingTheFirst) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "repeat", "first"), MAKO_LOCAL_OK);
  EXPECT_EQ(put(txn, primary, "repeat", "second"),
            MAKO_LOCAL_DUPLICATE_WRITE);
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "repeat").second,
            std::optional<std::string>("first"));
  commit_and_destroy(txn);

  txn = begin();
  uint8_t existed = 0;
  ASSERT_EQ(mako_local_txn_remove(
                txn, primary, reinterpret_cast<const uint8_t *>("repeat"), 6,
                &existed),
            MAKO_LOCAL_OK);
  ASSERT_EQ(existed, 1);
  EXPECT_EQ(put(txn, primary, "repeat", "unsafe-composition"),
            MAKO_LOCAL_DUPLICATE_WRITE);
  abort_and_destroy(txn);
}

TEST_F(LocalAbiTest, DatabaseCloseReportsBusyUntilTransactionEnds) {
  auto *txn = begin();
  EXPECT_EQ(mako_local_db_close(db), MAKO_LOCAL_BUSY);
  abort_and_destroy(txn);
}

#if !READ_MY_WRITES
TEST_F(LocalAbiTest, UnadvertisedReadYourWritesConflictLeavesWorkerReusable) {
  ASSERT_EQ(mako_local_feature_bits() & MAKO_LOCAL_FEATURE_READ_MY_WRITES, 0U);

  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "own-write", "value"), MAKO_LOCAL_OK);
  EXPECT_EQ(get(txn, primary, "own-write").first, MAKO_LOCAL_CONFLICT);
  destroy_tracked(txn);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "own-write").second.has_value());
  ASSERT_EQ(put(txn, primary, "after-conflict", "works"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}
#endif

TEST_F(LocalAbiTest, ConcurrentReadWriteConflictAbortsExactlyOneCommit) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "contended", "0"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  struct WorkerResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    int read = MAKO_LOCAL_INTERNAL;
    std::optional<std::string> read_value;
    int contended_put = MAKO_LOCAL_INTERNAL;
    int side_put = MAKO_LOCAL_INTERNAL;
    int commit = MAKO_LOCAL_INTERNAL;
    int destroy = MAKO_LOCAL_INTERNAL;
    HookObservation hook;
  } results[2];

  std::barrier ready(2);
  std::vector<std::thread> workers;
  for (int i = 0; i != 2; ++i) {
    workers.emplace_back([&, i] {
      WorkerResult &result = results[i];
      result.attach = mako_local_thread_attach();
      mako_local_txn *txn = nullptr;
      if (result.attach == MAKO_LOCAL_OK)
        result.begin = mako_local_txn_begin(db, &txn);
      if (result.begin == MAKO_LOCAL_OK) {
        auto read = get(txn, primary, "contended");
        result.read = read.first;
        result.read_value = std::move(read.second);
      }
      if (result.read == MAKO_LOCAL_OK &&
          result.read_value == std::optional<std::string>("0")) {
        result.contended_put =
            put(txn, primary, "contended", i == 0 ? "1" : "2");
      }
      if (result.contended_put == MAKO_LOCAL_OK) {
        result.side_put = put(txn, primary, "side-" + std::to_string(i),
                              i == 0 ? "left" : "right");
      }

      // Never return before rendezvous: a failed setup must not strand the
      // peer forever inside the barrier.
      ready.arrive_and_wait();

      if (result.side_put == MAKO_LOCAL_OK)
        result.commit =
            mako_local_txn_commit_with_hook(txn, accept_hook, &result.hook);
      if (txn != nullptr)
        result.destroy = mako_local_txn_destroy(txn);
    });
  }
  for (auto &worker : workers) worker.join();

  for (int i = 0; i != 2; ++i) {
    EXPECT_EQ(results[i].attach, MAKO_LOCAL_OK) << "worker " << i;
    EXPECT_EQ(results[i].begin, MAKO_LOCAL_OK) << "worker " << i;
    EXPECT_EQ(results[i].read, MAKO_LOCAL_OK) << "worker " << i;
    EXPECT_EQ(results[i].read_value, std::optional<std::string>("0"))
        << "worker " << i;
    EXPECT_EQ(results[i].contended_put, MAKO_LOCAL_OK) << "worker " << i;
    EXPECT_EQ(results[i].side_put, MAKO_LOCAL_OK) << "worker " << i;
    EXPECT_EQ(results[i].destroy, MAKO_LOCAL_OK) << "worker " << i;
  }

  const int commits = (results[0].commit == MAKO_LOCAL_OK) +
                      (results[1].commit == MAKO_LOCAL_OK);
  const int conflicts = (results[0].commit == MAKO_LOCAL_CONFLICT) +
                        (results[1].commit == MAKO_LOCAL_CONFLICT);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(conflicts, 1);
  EXPECT_EQ(results[0].hook.calls + results[1].hook.calls, 1);
  const int winner = results[0].commit == MAKO_LOCAL_OK ? 0 : 1;
  ASSERT_TRUE(winner == 0 || winner == 1);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, "contended").second,
            std::optional<std::string>(winner == 0 ? "1" : "2"));
  EXPECT_EQ(get(verify, primary, "side-0").second.has_value(),
            winner == 0);
  EXPECT_EQ(get(verify, primary, "side-1").second.has_value(),
            winner == 1);
  commit_and_destroy(verify);
}

}  // namespace
