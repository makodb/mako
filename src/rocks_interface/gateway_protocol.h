#pragma once
#include <stddef.h>
#include <stdint.h>

// Versioned gateway protocol. All operations are independently committed;
// Begin/Commit/Rollback open/close a session, not an atomic multi-op transaction.
// Client IDs are administratively unique high-bit IDs, never reused after restart.
// A sequence is consumed by every operation, including session control. Retries
// MUST send the identical request. Unknown outcomes MUST NOT advance the stream.
enum MakoGatewayKind : uint32_t {
    MAKO_GATEWAY_BEGIN = 20, MAKO_GATEWAY_COMMIT = 21,
    MAKO_GATEWAY_ROLLBACK = 22, MAKO_GATEWAY_PUT = 23,
    MAKO_GATEWAY_GET = 24, MAKO_GATEWAY_DELETE = 25,
    MAKO_GATEWAY_ROUTE = 27, MAKO_GATEWAY_INSERT = 28
};
enum MakoGatewayOutcome : uint32_t {
    MAKO_GATEWAY_UNKNOWN = 0, MAKO_GATEWAY_COMMITTED = 1,
    MAKO_GATEWAY_ABORTED = 2, MAKO_GATEWAY_REJECTED = 3,
    MAKO_GATEWAY_IN_FLIGHT = 4
};
static constexpr uint32_t MAKO_GATEWAY_VERSION = 1;
static constexpr size_t MAKO_GATEWAY_VALUE_LIMIT = 8000;
struct MakoGatewayRequest {
    uint32_t version;
    uint32_t kind;
    uint64_t client;
    uint64_t sequence;
    uint64_t session;
    uint64_t table;
    uint64_t epoch;
    uint32_t owner;
    uint32_t physical_table;
    uint32_t route_known;
    uint32_t fixed_coordinate;
    uint32_t coordinate_length;
    uint32_t key_length;
    uint32_t value_length;
    uint32_t name_length;
    uint8_t coordinate[64];
    uint8_t key[64];
    uint8_t name[128];
    uint8_t value[MAKO_GATEWAY_VALUE_LIMIT];
};
struct MakoGatewayResponse {
    uint64_t client;
    uint64_t sequence;
    uint64_t table;
    uint64_t epoch;
    uint32_t owner;
    uint32_t physical_table;
    uint32_t fixed_coordinate;
    uint32_t coordinate_length;
    int32_t status;
    uint32_t outcome;
    uint32_t op_result;
    uint32_t value_length;
    uint8_t coordinate[64];
    uint8_t value[MAKO_GATEWAY_VALUE_LIMIT];
};
static_assert(offsetof(MakoGatewayRequest, value) == 336);
static_assert(offsetof(MakoGatewayResponse, value) == 128);

extern "C" {
struct MakoGatewayClient;
// @unsafe - opaque Rust-owned state; no C++ sequencing or retained-result map.
MakoGatewayClient* mako_gateway_client_new(uint64_t client, uint64_t first_sequence);
void mako_gateway_client_free(MakoGatewayClient* client);
uint32_t mako_gateway_prepare(MakoGatewayClient* client, const MakoGatewayRequest* intent,
                             MakoGatewayRequest* request);
uint32_t mako_gateway_set_route(MakoGatewayClient* client, const MakoGatewayResponse* route,
                               MakoGatewayRequest* request);
uint32_t mako_gateway_complete(MakoGatewayClient* client, const MakoGatewayResponse* response);
// Called with no gateway state lock held. The only production callback is the
// annotated ShardReceiver bridge to RunNontxnOp, not arbitrary handler authority.
typedef void (*MakoGatewayExecute)(void*, const MakoGatewayRequest*, MakoGatewayResponse*);
void mako_gateway_execute(uint32_t participant, const MakoGatewayRequest* request,
                          void* context, MakoGatewayExecute execute,
                          MakoGatewayResponse* response);
}
