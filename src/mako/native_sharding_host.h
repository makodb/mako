#ifndef MAKO_NATIVE_SHARDING_HOST_H
#define MAKO_NATIVE_SHARDING_HOST_H

#include "cluster/native_sharding.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All byte spans are borrowed for the duration of the callback only. */
typedef void (*MakoShardRowSink)(void*, MakoShardBytes coordinate,
                                  MakoShardBytes key, MakoShardBytes value);
typedef void (*MakoShardReplySink)(void*, uint32_t status, MakoShardBytes reply);

/* Native I/O only: Rust owns ordering, authorization and migration state.
 * Storage callbacks run on a thread entered below, under the Rust participant
 * guard. They MUST NOT call ordinary lease admission. Their image consists of
 * logical row bytes in the selected range, not engine metadata trailers.
 * A successful put/delete is atomic and changes only the named physical key;
 * an error has no committed effect. Unrelated ranges may change concurrently.
 */
typedef struct MakoShardHost {
    void* context;
    uint32_t (*thread_enter)(void*, uint32_t owner);
    void (*thread_leave)(void*);
    /* Rust serializes creation and supplies the authoritative catalog binding.
     * A proxy belongs to the origin owner and is never a physical reverse alias. */
    uint32_t (*open)(void*, uint64_t table, MakoShardBytes name, uint32_t owner,
                     uint32_t proxy, uint32_t kind, uintptr_t* handle);
    /* Exactly one sink call on OK; NOT_FOUND alone certifies true EOF. Cursor
     * ordering is strict (coordinate,key), never coordinate+NUL. Every warehouse
     * coordinate intersecting the range is considered, including unopened ones. */
    uint32_t (*scan_next)(void*, uint64_t table, MakoShardBytes lo,
                          uint32_t has_hi, MakoShardBytes hi, uint32_t has_after,
                          MakoShardBytes after_coordinate, MakoShardBytes after_key,
                          MakoShardRowSink, void* sink_context);
    uint32_t (*put)(void*, uint64_t table, MakoShardBytes coordinate,
                    MakoShardBytes key, MakoShardBytes value);
    uint32_t (*remove)(void*, uint64_t table, MakoShardBytes coordinate,
                       MakoShardBytes key);
    /* Runs off the poll thread. OK means exactly one callback with the remote
     * application status; transport failures return IO without a callback. */
    uint32_t (*peer_call)(void*, uint32_t owner, uint32_t operation,
                          MakoShardBytes payload, MakoShardReplySink, void*);
} MakoShardHost;

/* start copies the vtable; the context and DB outlive stop. The catalog and
 * warehouse opener must already be seeded, and loaders must have finished.
 * stop is invoked off native workers, closes admission and joins all native
 * jobs before returning. Replicated nodes pass allow_migration=0. */
uint32_t mako_sharding_start_node(uint32_t owner, uint32_t nshards,
                                  uint32_t allow_migration, const MakoShardHost*);
uint32_t mako_sharding_dispatch(uint32_t owner, uint32_t operation,
                                MakoShardBytes payload, MakoShardReplySink, void*);
void mako_sharding_stop_node(uint32_t owner);
/* dispatch copies payload before returning. OK transfers callback ownership:
 * completion fires exactly once, including shutdown. Non-OK never fires it. */

/* CLI bridge: Rust encodes requests and decodes retained begin/poll outcomes.
 * The client supplies only a real connected srpc I/O callback. Reusing a nonce
 * never consumes the outcome or authorizes a second, different request. */
typedef uint32_t (*MakoShardAdminRpc)(void*, uint32_t operation,
                                      MakoShardBytes, MakoShardReplySink, void*);
typedef struct MakoShardAdminResult {
    uint64_t generation;
    uint32_t outcome; /* 0 pending, 1 committed, 2 aborted */
} MakoShardAdminResult;
uint32_t mako_sharding_admin_begin(MakoShardTxn nonce, MakoShardBytes table_name,
                                   MakoShardBytes lo, uint32_t has_hi,
                                   MakoShardBytes hi, uint32_t source,
                                   uint32_t destination, void* rpc_context,
                                   MakoShardAdminRpc, MakoShardAdminResult*);
uint32_t mako_sharding_admin_poll(MakoShardTxn nonce, void* rpc_context,
                                  MakoShardAdminRpc, MakoShardAdminResult*);
uint32_t mako_sharding_admin_abort(MakoShardTxn nonce, void* rpc_context,
                                   MakoShardAdminRpc, MakoShardAdminResult*);
#ifdef __cplusplus
}

#include <string>
#include <rusty/rusty.hpp>
#include "srpc/srpc.hpp"
import rusty;

class abstract_db;
class SiloRuntime;
namespace transport { class Configuration; }

namespace mako {

// @unsafe - borrowed engine/runtime outlive the host; owning RPC objects are
// dropped only after service acceptance stops and Rust workers are joined.
struct NativeShardingHost {
    abstract_db* db;
    const uint32_t owner;
    SiloRuntime* runtime;
    rusty::Arc<srpc::PollThread> poll;
    rusty::Option<rusty::Box<srpc::Server>> server;
    rusty::Mutex<rusty::Vec<rusty::Option<rusty::Arc<srpc::Client>>>> peers;

    NativeShardingHost(abstract_db*, uint32_t owner, uint32_t nshards, SiloRuntime*);
    ~NativeShardingHost();
    MakoShardHost callbacks();
    uint32_t start_service();
    void stop_accepting();
};

// @unsafe - shared YAML transport-address boundary, not DepTran partition zero.
std::string native_sharding_address(const transport::Configuration&, uint32_t owner,
                                     bool bind);
// @unsafe - synchronous srpc kernel; never called by a poll-thread handler.
uint32_t native_sharding_peer_call(void*, uint32_t owner, uint32_t operation,
                                    MakoShardBytes, MakoShardReplySink, void*);

} // namespace mako
#endif
#endif
