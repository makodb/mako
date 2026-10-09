#pragma once

#include <cstdint>

class abstract_db;

namespace janus {

// Catalog and warehouse opener registration occur before benchmark loading.
// Activate this thread's actual owner only AFTER its loaders have completed.
// MAKO_CLUSTER_CONFIG=1 enables the native runtime. Each same-process owner gets
// its own service and Rust queue; replicated workloads cannot request handoff.
// @unsafe - engine/runtime lifetime borrow, native Rust FFI and srpc startup.
void BootstrapClusterConfig(abstract_db* db);

// Stop acceptance and join native jobs before destroying any owner's indexes.
// Never call from a native worker or srpc poll thread. Calls are idempotent.
// @unsafe - native queue join and RPC resource teardown.
void ShutdownClusterConfig(uint32_t owner);
void ShutdownClusterConfig();

} // namespace janus
