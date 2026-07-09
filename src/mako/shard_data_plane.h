#pragma once

// The shard's migratable DATA PLANE, engine side (masstree TU) — see
// shard_data_plane.cc. cluster_bootstrap.cc (the rrr side) serves what this
// factory returns over the ShardDataService; the two sides stay in separate
// TUs because rrr/rrr.hpp and the Masstree headers compile pathologically in
// one -O3 TU. This header is the thin seam: strings + the cluster module's
// ShardData port only.

#include <string>

import cluster;   // janus::ShardData / janus::KvStore (module-owned)

class abstract_db;
class FullOrderedIndex;

namespace mako {

// Create the shard's migratable non-txn index (STANDALONE, with the given fixed
// table id — system tables must not consume per-process open_index id slots;
// see standalone_index.cc) and return a process-lifetime ShardData over it whose
// every op first ENGINE-REGISTERS the calling thread (Silo/mbta thread_init) —
// rrr service handler threads are not otherwise registered. Call from threads
// that are NOT already engine-registered workload threads (service/admin/seed
// threads). Returns nullptr if the index cannot be created.
janus::ShardData* make_engine_shard_data(abstract_db* db,
                                         const std::string& table_name,
                                         long table_id);

// Engine-register the calling thread with `db` (idempotent per thread). The
// admin Migrate handler calls this up front so config-store writes (the
// commit's cutover publish) are engine-safe even when no local data op ran
// first.
void engine_register_this_thread(abstract_db* db);

// A KvStore over `idx` whose every op engine-registers the calling thread
// first — the config-store twin of make_engine_shard_data. The ConfigWatcher
// poll thread and the ConfigKvService rrr handler thread both read the
// __mako_config__ mbta index without any prior engine setup; un-gated, those
// reads segfault the shard-0 leader. Returns a process-lifetime (leaked) store.
janus::KvStore* make_engine_kv_store(abstract_db* db, ::FullOrderedIndex* idx);

// Seed `count` demo rows ("d00".."dNN" -> "v<i>") into `sd` from a fresh,
// engine-registered thread (joined). Test/demo helper for the live migration
// bed (gated by env in the bootstrap); the main thread never touches sd.
void seed_shard_data(janus::ShardData* sd, int count);

}  // namespace mako
