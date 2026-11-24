#include "native_sharding_host.h"
#include "runtime/epoch.hh"
namespace mako {
namespace {
// @safe - Freeze precedes the shared nonwrapping epoch cut.
uint64_t epoch_cut(void*) { return EpochRuntime::epoch_cut(); }
// @safe - Drain observes worker announcements without entering a new epoch.
uint32_t epoch_drained(void*, uint64_t cut) {
    if (!cut || cut > EpochRuntime::epoch_current()) return MAKO_SHARD_INVALID;
    return EpochRuntime::epoch_drained(cut) ? MAKO_SHARD_OK : MAKO_SHARD_BUSY;
}
}
// @safe - Rust's configure callback fills every storage/worker slot before use.
MakoShardHost NativeShardingHost::callbacks() {
    return {this, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            native_sharding_peer_call, epoch_cut, epoch_drained};
}
}
