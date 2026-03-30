/**
 * Unit tests for Transaction Timeout Configuration.
 *
 * Tests ShardFailureController and timeout-related constants.
 */
#include "gtest/gtest.h"
#include "deptran/constants.h"
#include "mako/benchmarks/shard_failure_controller.h"

namespace janus {

class TxnTimeoutTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test: TXN_TIMEOUT constant is defined
TEST_F(TxnTimeoutTest, TxnTimeoutConstantDefined) {
    EXPECT_EQ(TXN_TIMEOUT, -30);
}

// Test: ShardFailureController creation
TEST_F(TxnTimeoutTest, ShardFailureControllerCreation) {
    ShardFailureController controller(4);
    EXPECT_EQ(controller.num_shards(), 4);
    EXPECT_EQ(controller.failed_shard_count(), 0);
}

// Test: ShardFailureController fail and recover shard
TEST_F(TxnTimeoutTest, ShardFailureControllerFailRecover) {
    ShardFailureController controller(3);

    // Initially all shards are healthy
    EXPECT_FALSE(controller.is_shard_failed(0));
    EXPECT_FALSE(controller.is_shard_failed(1));
    EXPECT_FALSE(controller.is_shard_failed(2));
    EXPECT_EQ(controller.failed_shard_count(), 0);

    // Fail shard 1
    controller.fail_shard(1);
    EXPECT_FALSE(controller.is_shard_failed(0));
    EXPECT_TRUE(controller.is_shard_failed(1));
    EXPECT_FALSE(controller.is_shard_failed(2));
    EXPECT_EQ(controller.failed_shard_count(), 1);

    // Recover shard 1
    controller.recover_shard(1);
    EXPECT_FALSE(controller.is_shard_failed(1));
    EXPECT_EQ(controller.failed_shard_count(), 0);
}

// Test: ShardFailureController fail_all and recover_all
TEST_F(TxnTimeoutTest, ShardFailureControllerFailAllRecoverAll) {
    ShardFailureController controller(4);

    controller.fail_all_shards();
    EXPECT_EQ(controller.failed_shard_count(), 4);
    EXPECT_TRUE(controller.is_shard_failed(0));
    EXPECT_TRUE(controller.is_shard_failed(3));

    controller.recover_all_shards();
    EXPECT_EQ(controller.failed_shard_count(), 0);
    EXPECT_FALSE(controller.is_shard_failed(0));
    EXPECT_FALSE(controller.is_shard_failed(3));
}

// Test: ShardFailureController invalid index handling
TEST_F(TxnTimeoutTest, ShardFailureControllerInvalidIndex) {
    ShardFailureController controller(2);

    // Invalid index should not crash, just log error and return false
    EXPECT_FALSE(controller.is_shard_failed(100));

    // fail/recover on invalid index should not crash
    controller.fail_shard(100);
    controller.recover_shard(100);

    // Original shards should still work
    EXPECT_EQ(controller.failed_shard_count(), 0);
}

// Test: Global is_shard_failed with no controller set
TEST_F(TxnTimeoutTest, GlobalIsShardFailedNoController) {
    // When no controller is set, should return false
    g_shard_failure_controller = nullptr;
    EXPECT_FALSE(is_shard_failed(0));
    EXPECT_FALSE(is_shard_failed(1));
}

// Test: Global is_shard_failed with controller set
TEST_F(TxnTimeoutTest, GlobalIsShardFailedWithController) {
    ShardFailureController controller(3);
    g_shard_failure_controller = &controller;

    EXPECT_FALSE(is_shard_failed(0));

    controller.fail_shard(1);
    EXPECT_TRUE(is_shard_failed(1));

    controller.recover_shard(1);
    EXPECT_FALSE(is_shard_failed(1));

    // Clean up
    g_shard_failure_controller = nullptr;
}

// Test: Multiple failures at once
TEST_F(TxnTimeoutTest, ShardFailureControllerMultipleFailures) {
    ShardFailureController controller(5);

    controller.fail_shard(1);
    controller.fail_shard(3);
    controller.fail_shard(4);

    EXPECT_FALSE(controller.is_shard_failed(0));
    EXPECT_TRUE(controller.is_shard_failed(1));
    EXPECT_FALSE(controller.is_shard_failed(2));
    EXPECT_TRUE(controller.is_shard_failed(3));
    EXPECT_TRUE(controller.is_shard_failed(4));
    EXPECT_EQ(controller.failed_shard_count(), 3);
}

// Test: Repeated fail/recover cycles
TEST_F(TxnTimeoutTest, ShardFailureControllerRepeatedCycles) {
    ShardFailureController controller(2);

    for (int i = 0; i < 10; i++) {
        controller.fail_shard(0);
        EXPECT_TRUE(controller.is_shard_failed(0));
        EXPECT_EQ(controller.failed_shard_count(), 1);

        controller.recover_shard(0);
        EXPECT_FALSE(controller.is_shard_failed(0));
        EXPECT_EQ(controller.failed_shard_count(), 0);
    }
}

} // namespace janus
