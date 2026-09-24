/**
 * Documentation guard for the Rust book and C++ companion.
 *
 * The book is prose, so nothing in the build catches it drifting from the
 * canonical Rust in rpc/, reactor/, base/ and misc/. This test greps the book
 * for two kinds of string:
 *
 *   required  - an API spelling that must appear, or a caveat sentence that
 *               must stay next to an API the book documents but that is not
 *               actually wired up. Deleting the caveat is as much a
 *               regression as misspelling the API, so both are guarded.
 *   forbidden - a spelling that exists nowhere in this repo. Every entry was
 *               checked against the Rust sources, pylib/ and srpc.hpp: none
 *               of them name anything real, so none can be tripped by a
 *               correct edit.
 *
 * Rule for adding entries: a forbidden string must be wrong under *every*
 * reading, not just wrong for one type. `base_delay_ms`, `jitter_factor` and
 * `ttl_ms` are real fields (on RequestOptions, RequestOptions and
 * CompletionTrackerConfig/IdempotencyConfig respectively), so forbidding them
 * behind a variable-name prefix -- `policy.base_delay_ms`, `buffering.ttl_ms`
 * -- guards a variable name rather than an API, and fires on correct prose.
 * Those are covered instead by *requiring* the correct field names.
 */

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

// CMake passes an absolute path; the relative default is for running the test
// binary from the repository root by hand.
#ifdef SRPC_BOOK_PATH
constexpr const char* kSrpcBookPath = SRPC_BOOK_PATH;
#else
constexpr const char* kSrpcBookPath = "docs/srpc-book.md";
#endif
#ifdef SRPC_CPP_BOOK_PATH
constexpr const char* kSrpcCppBookPath = SRPC_CPP_BOOK_PATH;
#else
constexpr const char* kSrpcCppBookPath = "docs/srpc-cpp-book.md";
#endif

}  // namespace

TEST(SrpcBookApiSymbolsTest, ReliabilityApiNamesMatchShippingHeaders) {
    const std::string rust_book = load_file_contents(kSrpcBookPath);
    const std::string cpp_book = load_file_contents(kSrpcCppBookPath);
    ASSERT_FALSE(rust_book.empty()) << "failed to read " << kSrpcBookPath;
    ASSERT_FALSE(cpp_book.empty()) << "failed to read " << kSrpcCppBookPath;
    const std::string book = rust_book + "\n" + cpp_book;

    const std::vector<std::string> required = {
        // --- Reliability API symbols (chapter 11) ---------------------------
        // rpc/load_balancer.rs: enum LoadBalancingStrategy { RANDOM,
        // ROUND_ROBIN, LEAST_CONNECTIONS, LEAST_LATENCY }.
        "LoadBalancingStrategy::ROUND_ROBIN",
        // rpc/reconnect_policy.rs: initial_delay_ms / jitter_enabled. These
        // are the entries that keep the book off `base_delay_ms` and
        // `jitter_factor`, which belong to RequestOptions, not ReconnectPolicy.
        "policy.initial_delay_ms",
        "policy.jitter_enabled",
        // rpc/circuit_breaker.rs: CircuitBreakerConfig::timeout_ms.
        "breaker.timeout_ms",

        // Buffering encodes once and replays through the canonical channel path.
        "buffering.max_pending",
        "buffering.default_ttl_ms",
        "Reconnect replays those bytes FIFO and retains the original future.",

        // TCP applies the configured options; other transports report unsupported.
        "int32_t idle_sec;",
        "int32_t interval_sec;",
        "| TCP keepalive | Native socket options are applied through the TCP channel |",

        // rpc/callbacks.rs: add_* append, they do not replace.
        "client.add_on_connected",
        "client.add_on_disconnected",
        "client.add_on_error",
        "client.add_on_reconnecting",
        "client.add_on_reconnected",

        // rpc/errors.rs: RpcError variant spellings, one per band.
        "UNKNOWN_RPC_ID",
        "MARSHALLING_ERROR",
        "CONNECT_TIMEOUT",

        "client.request(",
        // rpc/server.rs: graceful_shutdown(drain_timeout_ms).
        "server.graceful_shutdown(",

        // Client and ClientConnection share the same live metrics owner.
        "`client.metrics()` and its live connection share the same counter storage",
        "in_flight_requests()",
        "reconnect_count()",
        "Counters use relaxed atomics",

        // The shipping-status table itself. These rows are the book's honest
        // account of what is finished; losing them is the regression.
        "### Shipping status",
        "| State transitions | Validated by the synchronized state machine; forced transitions bypass validation |",
        "| Latency metrics | Need explicit latency instrumentation |",
        "| Disconnected request queue | Active for future-based requests when buffering is enabled |",
        "| Protocol heartbeat | Server replies and receive accounting work; no automatic client timer drives probes |",

        // --- Typed request/response API symbols (C++ chapters 2 and 4) --------
        // pylib/simplerpcgen/lang_cpp.py: typed_struct_name() emits
        // "Rpc" + PascalCase(method split on '_') + "Request"/"Response".
        // Rpc<M>Request/Rpc<M>Response is the book's placeholder notation;
        // RpcDotProd* is the worked example that pins the '_' split.
        "Rpc<M>Request",
        "Rpc<M>Response",
        "RpcDotProdRequest",
        "RpcDotProdResponse",
        // Handler signatures. typed_result_type() is
        // rusty::Result<Rpc<M>Response, srpc::i32>; the `async` attribute
        // wraps it in rusty::Task.
        "rusty::Result<Rpc<M>Response, srpc::i32> m(const Rpc<M>Request&) const",
        "rusty::Task<rusty::Result<Rpc<M>Response, srpc::i32>> m(const Rpc<M>Request&) const",
        "rusty::Result<RpcSumResponse, srpc::i32> sum(const RpcSumRequest& req) const",
        // Proxy side: async_<method> takes the request struct.
        "async_<method>",
        "demo.async_sum(req)",
        // rpc/server.rs: reg_service_typed<T: Service>(Box<T>) wraps T in
        // ServiceBoxShim<T>. A generated service class has no base class, so
        // it cannot be passed to reg_service(Box<dyn Service>) -- requiring
        // the typed call and the caveat is the point of these three.
        "template<class T> void reg_service_typed(rusty::Box<T> svc);",
        "svr.reg_service_typed(rusty::make_box<",
        "Register with `reg_service_typed`, not `reg_service`",
        "The generated service class has **no base class**",
    };

    const std::vector<std::string> forbidden = {
        // Stale reliability API symbols. None of these names exist in
        // rpc/*.rs, pylib/ or srpc.hpp under any spelling.
        "set_load_balancing(",
        "LoadBalancing::",
        "keepalive.idle_time",
        "keepalive.interval =",
        "cb.half_open_timeout_ms",
        "buffering.max_queue_size",
        "callbacks.on_connected",
        "callbacks.on_disconnected",
        "callbacks.on_error",
        "callbacks.on_reconnecting",
        "callbacks.on_reconnected",
        // ConnectionMetrics spells these in_flight_requests() and
        // reconnect_count(); no receiver prefix, so a variable rename in the
        // book cannot dodge the guard.
        "requests_in_flight()",
        "get_reconnect_count()",
        // RpcError variants that do not exist (the real ones are
        // CONNECT_TIMEOUT, CONNECTION_CLOSED, UNKNOWN_RPC_ID,
        // MARSHALLING_ERROR, SERVICE_UNAVAILABLE).
        "CONNECTION_TIMEOUT",
        "CONNECTION_LOST",
        "UNKNOWN_METHOD",
        "MARSHAL_ERROR",
        "SERVICE_ERROR",
        "HANDLER_EXCEPTION",
        "QUEUE_FULL",
        // Pointer-style / pre-rusty call shapes.
        "ClientConnection conn(reactor,",
        "begin_request(",
        "end_request()",
        "Future* fu =",
        "fu->Wait()",
        "server.add_service(",
        "server.stop()",
        "__reg_to__(Server* server)",
        "__dispatch__(Request* req)",
        // Stale legacy-compat symbols: rpcgen has no compat flag and emits
        // no deprecation attributes.
        "--legacy-compat",
        "SRPC_LEGACY_COMPAT",
        "[[deprecated(",
        "RPCGEN_COMPAT_FLAG",
    };

    for (const auto& needle : required) {
        EXPECT_NE(book.find(needle), std::string::npos)
            << "missing required API symbol in SRPC documentation: " << needle;
    }

    for (const auto& needle : forbidden) {
        EXPECT_EQ(book.find(needle), std::string::npos)
            << "stale API symbol still present in SRPC documentation: " << needle;
    }
}
