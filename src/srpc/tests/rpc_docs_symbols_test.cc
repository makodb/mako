#include <gtest/gtest.h>


import std;

namespace {

std::string load_file_contents(const char* path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return "";
    }

    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

#ifdef SRPC_BOOK_PATH
constexpr const char* kSrpcBookPath = SRPC_BOOK_PATH;
#else
constexpr const char* kSrpcBookPath = "docs/srpc-book.md";
#endif

#ifdef SRPC_MIGRATION_GUIDE_PATH
constexpr const char* kMigrationGuidePath = SRPC_MIGRATION_GUIDE_PATH;
#else
constexpr const char* kMigrationGuidePath = "docs/rpc/migration-guide.md";
#endif

}  // namespace

TEST(SrpcBookApiSymbolsTest, ReliabilityApiNamesMatchShippingHeaders) {
    const std::string book = load_file_contents(kSrpcBookPath);
    ASSERT_FALSE(book.empty()) << "failed to read " << kSrpcBookPath;

    const std::vector<std::string> required = {
        // Reliability API symbols
        "LoadBalancingStrategy::ROUND_ROBIN",
        "keepalive.idle_sec",
        "keepalive.interval_sec",
        "policy.initial_delay_ms",
        "policy.jitter_enabled",
        "cb.timeout_ms",
        "buffering.max_pending",
        "buffering.default_ttl_ms",
        "client.add_on_connected",
        "client.add_on_disconnected",
        "client.add_on_error",
        "client.add_on_reconnecting",
        "client.add_on_reconnected",
        "UNKNOWN_RPC_ID",
        "MARSHALLING_ERROR",
        "CONNECT_TIMEOUT",
        "client->request(",
        "server.reg_service(",
        "server.graceful_shutdown(",
        "const ConnectionMetrics& metrics = client.metrics();",
        "metrics.in_flight_requests()",
        "metrics.reconnect_count()",
        "### Implemented vs Planned (Shipping Status)",
        "| Connection state machine | Implemented |",
        "| Planned-only reliability APIs in this chapter | Planned |",
        // Typed request/response API symbols
        "Rpc<MethodPascalCase>Request",
        "Rpc<MethodPascalCase>Response",
        "Result<MethodResponse, srpc::i32> Method(const MethodRequest&)",
        "async_Method(const MethodRequest&",
        "Server::reg_service_typed(Box<T>)",
        "RpcResult<GetUserResponse> get_user(const GetUserRequest& req)",
    };

    const std::vector<std::string> forbidden = {
        // Stale reliability API symbols
        "set_load_balancing(",
        "LoadBalancing::",
        "keepalive.idle_time",
        "keepalive.interval =",
        "policy.base_delay_ms",
        "policy.jitter_factor",
        "cb.half_open_timeout_ms",
        "buffering.max_queue_size",
        "buffering.ttl_ms",
        "callbacks.on_connected",
        "callbacks.on_disconnected",
        "callbacks.on_error",
        "callbacks.on_reconnecting",
        "callbacks.on_reconnected",
        "metrics.requests_in_flight()",
        "metrics.get_reconnect_count()",
        "CONNECTION_TIMEOUT",
        "CONNECTION_LOST",
        "UNKNOWN_METHOD",
        "MARSHAL_ERROR",
        "SERVICE_ERROR",
        "HANDLER_EXCEPTION",
        "QUEUE_FULL",
        "ClientConnection conn(reactor,",
        "begin_request(",
        "end_request()",
        "Future* fu =",
        "fu->Wait()",
        "server.add_service(",
        "server.stop()",
        "__reg_to__(Server* server)",
        "__dispatch__(Request* req)",
        // Stale legacy-compat / pointer-style API symbols
        "--legacy-compat",
        "SRPC_LEGACY_COMPAT",
        "[[deprecated(",
        "RPCGEN_COMPAT_FLAG",
    };

    for (const auto& needle : required) {
        EXPECT_NE(book.find(needle), std::string::npos)
            << "missing required API symbol in srpc-book.md: " << needle;
    }

    for (const auto& needle : forbidden) {
        EXPECT_EQ(book.find(needle), std::string::npos)
            << "stale API symbol still present in srpc-book.md: " << needle;
    }
}

TEST(SrpcBookApiSymbolsTest, MigrationGuideBackwardCompatibilityNotesAreCurrent) {
    const std::string guide = load_file_contents(kMigrationGuidePath);
    ASSERT_FALSE(guide.empty()) << "failed to read " << kMigrationGuidePath;

    const std::vector<std::string> required = {
        "## Backward Compatibility Notes (Wire/API)",
        "kResponseHeaderExtFlag",
        "Upgrade clients first.",
        "Upgrade servers after client rollout is complete.",
        "policy.initial_delay_ms",
        "client->add_on_connected(",
        "client->add_on_disconnected(",
        "client->add_on_reconnected([](bool success)",
    };

    const std::vector<std::string> forbidden = {
        "policy.base_delay_ms",
        "client->set_on_connected(",
        "client->set_on_disconnected(",
        "client->set_on_reconnected(",
    };

    for (const auto& needle : required) {
        EXPECT_NE(guide.find(needle), std::string::npos)
            << "missing required migration-guide symbol/text: " << needle;
    }

    for (const auto& needle : forbidden) {
        EXPECT_EQ(guide.find(needle), std::string::npos)
            << "stale migration-guide symbol/text still present: " << needle;
    }
}
