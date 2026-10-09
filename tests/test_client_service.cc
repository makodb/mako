// Tests call the production Rust SDK state ABI, not a simulated counter/handler.
// Real engine and retained-result coverage lives in test_mako_nontxn_distributed.
#include "rocks_interface/gateway_protocol.h"
#include <gtest/gtest.h>
#include <cstring>
#include <limits>

namespace {
constexpr uint64_t kClient = UINT64_C(0x8000000000000101);
class GatewayClientTest : public ::testing::Test {
protected:
    MakoGatewayClient* client = nullptr;
    // @unsafe - owns one opaque production native client stream per test.
    void SetUp() override { client = mako_gateway_client_new(kClient, 1); }
    void TearDown() override { mako_gateway_client_free(client); }
    // @safe - zero-initialized transport intent, without allocating an identity.
    static MakoGatewayRequest Intent(uint32_t kind, uint64_t session = 0) {
        MakoGatewayRequest request{};
        request.version = MAKO_GATEWAY_VERSION;
        request.kind = kind;
        request.session = session;
        return request;
    }
};

// @unsafe - calls the actual native client constructor.
TEST(GatewayNamespace, RejectsWorkerNamespaceAndZeroSequence) {
    EXPECT_EQ(mako_gateway_client_new(1, 1), nullptr);
    EXPECT_EQ(mako_gateway_client_new(kClient, 0), nullptr);
}

// @unsafe - production Rust assigns and retains the caller's begin identity.
TEST_F(GatewayClientTest, LostBeginReplyRetainsFullIdentity) {
    auto intent = Intent(MAKO_GATEWAY_BEGIN);
    MakoGatewayRequest first{}, retry{};
    ASSERT_EQ(mako_gateway_prepare(client, &intent, &first), 0u);
    ASSERT_EQ(mako_gateway_prepare(client, &intent, &retry), 0u);
    EXPECT_EQ(first.client, kClient);
    EXPECT_EQ(first.sequence, 1u);
    EXPECT_EQ(first.session, first.sequence);
    EXPECT_EQ(std::memcmp(&first, &retry, sizeof(first)), 0);
}

// @unsafe - the production pending-operation state owns the selected grant.
TEST_F(GatewayClientTest, UnknownWriteKeepsGrantAndCannotBecomeDifferentOperation) {
    auto intent = Intent(MAKO_GATEWAY_PUT);
    intent.physical_table = 7;
    intent.key_length = 1; intent.key[0] = 'k';
    intent.value_length = 1; intent.value[0] = 'v';
    MakoGatewayRequest request{}, retry{};
    ASSERT_EQ(mako_gateway_prepare(client, &intent, &request), 0u);
    MakoGatewayResponse route{};
    route.client = request.client; route.sequence = request.sequence;
    route.table = 100; route.owner = 3; route.epoch = 99;
    route.physical_table = 7; route.coordinate_length = 1; route.coordinate[0] = 'k';
    ASSERT_EQ(mako_gateway_set_route(client, &route, &request), 0u);
    ASSERT_EQ(mako_gateway_prepare(client, &intent, &retry), 0u);
    EXPECT_EQ(retry.epoch, 99u);
    EXPECT_EQ(retry.owner, 3u);
    EXPECT_EQ(retry.table, 100u);
    EXPECT_EQ(std::memcmp(&request, &retry, sizeof(request)), 0);
    route.outcome = MAKO_GATEWAY_UNKNOWN;
    EXPECT_NE(mako_gateway_complete(client, &route), 0u);
    intent.kind = MAKO_GATEWAY_DELETE;
    EXPECT_NE(mako_gateway_prepare(client, &intent, &retry), 0u);
    route.epoch = 100;
    EXPECT_NE(mako_gateway_set_route(client, &route, &retry), 0u);
}

// @unsafe - completing the actual terminal response alone advances the stream.
TEST_F(GatewayClientTest, BeginPutCommitHaveSeparateSequentialIdentities) {
    auto begin = Intent(MAKO_GATEWAY_BEGIN);
    MakoGatewayRequest request{};
    ASSERT_EQ(mako_gateway_prepare(client, &begin, &request), 0u);
    const auto session = request.session;
    MakoGatewayResponse response{};
    response.client = kClient; response.sequence = request.sequence;
    response.outcome = MAKO_GATEWAY_COMMITTED;
    ASSERT_EQ(mako_gateway_complete(client, &response), 0u);
    auto put = Intent(MAKO_GATEWAY_PUT, session);
    ASSERT_EQ(mako_gateway_prepare(client, &put, &request), 0u);
    EXPECT_EQ(request.sequence, 2u);
    EXPECT_EQ(request.session, session);
    response.sequence = request.sequence;
    ASSERT_EQ(mako_gateway_complete(client, &response), 0u);
    auto commit = Intent(MAKO_GATEWAY_COMMIT, session);
    ASSERT_EQ(mako_gateway_prepare(client, &commit, &request), 0u);
    EXPECT_EQ(request.sequence, 3u);
    EXPECT_EQ(request.session, session);
}

// @unsafe - the final u64 sequence is usable once and never wraps to zero.
TEST(GatewayNamespace, SequenceExhaustionFailsClosed) {
    auto* client = mako_gateway_client_new(kClient, std::numeric_limits<uint64_t>::max());
    ASSERT_NE(client, nullptr);
    MakoGatewayRequest intent{}, request{};
    intent.version = MAKO_GATEWAY_VERSION;
    intent.kind = MAKO_GATEWAY_BEGIN;
    ASSERT_EQ(mako_gateway_prepare(client, &intent, &request), 0u);
    MakoGatewayResponse reply{};
    reply.client = request.client; reply.sequence = request.sequence;
    reply.outcome = MAKO_GATEWAY_COMMITTED;
    ASSERT_EQ(mako_gateway_complete(client, &reply), 0u);
    EXPECT_NE(mako_gateway_prepare(client, &intent, &request), 0u);
    mako_gateway_client_free(client);
}
} // namespace
