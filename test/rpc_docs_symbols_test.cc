#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string load_book_contents() {
#ifdef SRPC_BOOK_PATH
    const char* path = SRPC_BOOK_PATH;
#else
    const char* path = "docs/srpc-book.md";
#endif

    std::ifstream in(path);
    if (!in.is_open()) {
        return "";
    }

    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

}  // namespace

TEST(SrpcBookApiSymbolsTest, ReliabilityApiNamesMatchShippingHeaders) {
    const std::string book = load_book_contents();
    ASSERT_FALSE(book.empty()) << "failed to read docs/srpc-book.md";

    const std::vector<std::string> required = {
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
        "proxy.get_user(1001, &user)",
        "### Implemented vs Planned (Shipping Status)",
        "| Connection state machine | Implemented |",
        "| Planned-only reliability APIs in this chapter | Planned |",
    };

    const std::vector<std::string> forbidden = {
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
        "UserInfo user = proxy.get_user(1001);",
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
