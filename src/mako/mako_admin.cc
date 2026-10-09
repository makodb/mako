#include "native_sharding_host.h"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include "deptran/rcc_rpc.h"

namespace {
// @unsafe - command-line / generated srpc string boundary.
MakoShardBytes bytes(const std::string& value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

struct Connection {
    rusty::Arc<srpc::PollThread> poll;
    rusty::Arc<srpc::Client> client;
    // @unsafe - canonical srpc transport, no alternate TCP control protocol.
    Connection() : poll(srpc::PollThread::create()), client(srpc::Client::create(poll.clone())) {}
    // @unsafe - close the connection before joining its poll thread.
    ~Connection() { client->close(); poll->shutdown(); }
};

// @unsafe - Rust owns wire encoding/decoding; C++ copies only opaque bytes into
// the generated service request and returns the borrowed real response.
uint32_t invoke(void* context, uint32_t operation, MakoShardBytes payload,
                   MakoShardReplySink sink, void* sink_context) {
    try {
        auto& connection = *static_cast<Connection*>(context);
        janus::NativeShardingProxy proxy(const_cast<srpc::Client*>(connection.client.get()));
        janus::NativeShardingProxy::RpcInvokeRequest request;
        request.operation = operation;
        if (payload.len) request.payload.assign(reinterpret_cast<const char*>(payload.data), payload.len);
        auto response = proxy.Invoke(request);
        if (response.is_err()) return MAKO_SHARD_IO;
        auto result = response.unwrap();
        sink(sink_context, result.status, bytes(result.reply));
        return MAKO_SHARD_OK;
    } catch (...) {
        return MAKO_SHARD_IO;
    }
}

// @unsafe - parses only the CLI's decimal scalar syntax, not a wire protocol.
template <typename T>
bool number(const char* text, T& value) {
    const auto length = std::strlen(text);
    const auto result = std::from_chars(text, text + length, value);
    return length && result.ec == std::errc() && result.ptr == text + length;
}
// @safe - accepts binary range endpoints without text/empty/infinity ambiguity.
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// @unsafe - CLI byte syntax only; Rust constructs the actual migration request.
bool hex_bytes(std::string_view text, std::string& value) {
    if (text.size() % 2) return false;
    value.resize(text.size() / 2);
    for (size_t i = 0; i < value.size(); ++i) {
        const int hi = hex_digit(text[2 * i]);
        const int lo = hex_digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        value[i] = static_cast<char>((hi << 4) | lo);
    }
    return true;
}
// @safe - stable status names from the native ABI.
const char* status_name(uint32_t status) {
    switch (status) {
    case MAKO_SHARD_OK: return "ok";
    case MAKO_SHARD_NOT_FOUND: return "not-found";
    case MAKO_SHARD_RETRY: return "retry";
    case MAKO_SHARD_INVALID: return "invalid (also returned when live migration is disabled)";
    case MAKO_SHARD_BUSY: return "busy";
    case MAKO_SHARD_EXHAUSTED: return "exhausted";
    case MAKO_SHARD_IO: return "io-error";
    default: return "unknown-status";
    }
}
// @unsafe - user-facing usage goes to the process stderr.
int usage() {
    std::fprintf(stderr,
        "Usage:\n"
        "  mako_admin begin ADDRESS CLIENT SEQUENCE TABLE LO_HEX HI_HEX|inf SOURCE DESTINATION\n"
        "  mako_admin poll ADDRESS CLIENT SEQUENCE\n"
        "  mako_admin abort ADDRESS CLIENT SEQUENCE\n\n"
        "ADDRESS is the coordinator's configured shard port + 20000.\n"
        "CLIENT/SEQUENCE form a caller-owned nonce: preserve BOTH on every retry.\n"
        "TABLE is an existing canonical catalog name, not a physical table ID.\n"
        "Bounds are raw hexadecimal bytes; '' is empty, and only 'inf' is unbounded.\n"
        "A warehouse coordinate is its global ID as four big-endian bytes.\n"
        "Poll does not consume the outcome. No demo data or table is created.\n");
    std::fprintf(stderr, "Abort requests cancellation of an accepted nonce; a published commit cannot be undone.\n");
    return 2;
}
} // namespace

// @unsafe - thin CLI / srpc lifetime boundary. Native Rust retains nonce intent,
// drives the coordinator and interprets outcomes; no C++ migration state machine.
int main(int argc, char** argv) {
    const bool begin = argc > 1 && std::strcmp(argv[1], "begin") == 0;
    const bool poll = argc > 1 && std::strcmp(argv[1], "poll") == 0;
    const bool abort = argc > 1 && std::strcmp(argv[1], "abort") == 0;
    if ((!begin && !poll && !abort) || (begin && argc != 10)
        || ((poll || abort) && argc != 5)) return usage();
    MakoShardTxn nonce{};
    if (!number(argv[3], nonce.client) || !number(argv[4], nonce.sequence)) return usage();
    uint32_t source = 0, destination = 0;
    std::string table, lower, upper;
    const bool infinity = begin && std::strcmp(argv[7], "inf") == 0;
    if (begin) {
        table = argv[5];
        if (table.empty() || !hex_bytes(argv[6], lower)
            || (!infinity && !hex_bytes(argv[7], upper))
            || !number(argv[8], source) || !number(argv[9], destination)) return usage();
    }
    try {
        Connection connection;
        if (connection.client->connect(reinterpret_cast<const int8_t*>(argv[2]), false) != 0) {
            std::fprintf(stderr, "Cannot connect to native coordinator at %s\n", argv[2]);
            return 1;
        }
        MakoShardAdminResult result{};
        const uint32_t status = begin
            ? mako_sharding_admin_begin(nonce, bytes(table), bytes(lower), infinity ? 0 : 1,
                bytes(upper), source, destination, &connection, invoke, &result)
            : abort ? mako_sharding_admin_abort(nonce, &connection, invoke, &result)
                    : mako_sharding_admin_poll(nonce, &connection, invoke, &result);
        if (status != MAKO_SHARD_OK) {
            std::fprintf(stderr, "Native sharding %s failed: %s (%u); nonce=%llu:%llu\n",
                argv[1], status_name(status), status,
                static_cast<unsigned long long>(nonce.client),
                static_cast<unsigned long long>(nonce.sequence));
            if (status == MAKO_SHARD_IO || status == MAKO_SHARD_RETRY)
                std::fprintf(stderr, "The request may already be accepted. Poll or repeat begin with the SAME nonce and intent.\n");
            return 1;
        }
        const char* outcome = result.outcome == 0 ? "pending"
            : result.outcome == 1 ? "committed" : result.outcome == 2 ? "aborted" : "invalid";
        std::printf("nonce=%llu:%llu generation=%llu outcome=%s\n",
            static_cast<unsigned long long>(nonce.client),
            static_cast<unsigned long long>(nonce.sequence),
            static_cast<unsigned long long>(result.generation), outcome);
        return result.outcome > 1 ? 1 : 0;
    } catch (...) {
        std::fprintf(stderr, "Native sharding transport failed; retain nonce=%llu:%llu for retry\n",
            static_cast<unsigned long long>(nonce.client),
            static_cast<unsigned long long>(nonce.sequence));
        return 1;
    }
}
