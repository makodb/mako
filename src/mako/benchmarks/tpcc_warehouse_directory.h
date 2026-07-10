#pragma once

// Warehouse table directory -- the single resolution point for "which index
// handle serves (logical table, warehouse) RIGHT NOW".
//
// Before this, every worker baked each warehouse's location into pointers at
// startup: per table a vector of LOCAL index handles (valid only while this
// shard owns the warehouse) and a vector of REMOTE proxies with a nullptr
// hole for the shard's own warehouses. Ownership was frozen at open time; a
// migrated warehouse left the old owner dereferencing a stale local handle
// and the new owner with nothing but a proxy pointing away.
//
// The directory replaces both vectors. Resolution consults the partition
// table (tpcc_route_shard_for_warehouse; the T3 mixed strategy) with the
// static layout as the ungoverned fallback:
//   owner == my shard -> the local handle; if the warehouse was ADOPTED
//     (moved here) and no local handle exists yet, an empty local index is
//     created on demand -- the migration data plane fills it before the
//     cutover publishes.
//   owner != my shard -> the remote proxy; if the warehouse DEPARTED (moved
//     away from its static home here) and no proxy exists, one is created on
//     demand. Proxies route per-op through compute_shard_for_key (the T3
//     routing alias), so one proxy per warehouse suffices no matter where
//     the warehouse lives later.
//
// Hot path: two atomic loads + one routing consult per table access; handle
// creation is a mutex-guarded slow path taken only when ownership diverges
// from what has been materialized (i.e., around migrations). If the routing
// consult ever shows up in profiles, a config-version-stamped per-thread
// cache can land inside tpcc_route_shard_for_warehouse without touching
// callers.
//
// Plain C++ on purpose: included by tpcc.cc (a masstree TU that cannot
// import the cluster module); all cluster access goes through the plain
// tpcc_sharding.h bridge.

#include <stddef.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "benchmarks/tpcc_sharding.h"      // tpcc_route_shard_for_warehouse

// NOTE: index handles are deliberately OPAQUE here (forward declaration, never
// dereferenced). Pulling storage/abstract_ordered_index.h into a TU that also
// does gtest + import cluster tips the clang-22 frontend into a non-terminating
// compile once ANY further header joins (reproduced on scratch TUs: +<mutex>
// or +table_registry.h each hang; the modules-pathology family). The directory
// therefore stores pointers only; everything needing the complete type -- the
// engine open and the routing-alias registration -- lives in the caller's
// Opener. Revisit when the toolchain moves.
class FullOrderedIndex;

namespace mako {

// Per-table slot array, one entry per GLOBAL warehouse. Returned once by
// table_slots() and then indexed lock-free on the hot path.
struct WarehouseTableSlots {
    struct Entry {
        std::atomic<FullOrderedIndex*> local{nullptr};
        std::atomic<FullOrderedIndex*> remote{nullptr};
    };
    std::string table;            // logical name ("customer")
    std::vector<Entry> entries;   // size == num_warehouses_total

    WarehouseTableSlots(const std::string& t, size_t n) : table(t), entries(n) {}
    WarehouseTableSlots(const WarehouseTableSlots&) = delete;
    WarehouseTableSlots& operator=(const WarehouseTableSlots&) = delete;
};

// @unsafe - owns no indexes; handles live as long as the db (process lifetime).
class TpccWarehouseDirectory {
public:
    // Creates an index at runtime for (logical_table, global_wid): opens the
    // physical index named `name` with `shard` as the id-range/is_remote hint
    // (mbta's open_index marks handles for foreign shards as RPC proxies) AND
    // registers its routing alias (register_route(idx->get_table_id(),
    // logical_table, warehouse_route_key(global_wid))) -- the directory
    // cannot, since handles are opaque here (see the header NOTE). Plain
    // fn-pointer + context; a capture-free lambda converts implicitly.
    struct Opener {
        FullOrderedIndex* (*fn)(void* ctx, const std::string& logical_table,
                                int global_wid, const std::string& name,
                                int shard) = nullptr;
        void* ctx = nullptr;
        FullOrderedIndex* operator()(const std::string& logical_table,
                                     int global_wid, const std::string& name,
                                     int shard) const {
            return fn(ctx, logical_table, global_wid, name, shard);
        }
        explicit operator bool() const { return fn != nullptr; }
    };

    // Called once from init_tables, before workers/loaders construct. Shard
    // identity is deliberately NOT stored: resolve() takes the caller's shard
    // (TThread identity), so multi-shard single-process runners share one
    // directory correctly -- each runner overlays its own warehouse block.
    // @unsafe - not thread-safe; single-threaded setup only.
    void init(int warehouses_per_shard, int num_warehouses_total, Opener opener) {
        wps_ = warehouses_per_shard;
        total_ = num_warehouses_total;
        opener_ = opener;
    }

    // @unsafe - setup only (init_tables), before any resolve() runs.
    void register_local(const std::string& table, int global_wid,
                        FullOrderedIndex* idx) {
        slots_for(table)->entries[global_wid - 1].local.store(
            idx, std::memory_order_release);
    }
    // @unsafe - setup only (init_tables), before any resolve() runs.
    void register_remote(const std::string& table, int global_wid,
                         FullOrderedIndex* idx) {
        slots_for(table)->entries[global_wid - 1].remote.store(
            idx, std::memory_order_release);
    }

    // The per-table slot array; workers bind it once at construction so the
    // hot path never does a string lookup.
    // @unsafe - creates the slot array on first use (setup-order safe: called
    // from mixin constructors, which run after init()).
    WarehouseTableSlots* table_slots(const std::string& table) {
        return slots_for(table);
    }

    // Hot path: the handle serving `global_wid` of `slots->table` right now,
    // from the perspective of `my_shard` (the calling worker's shard).
    // @unsafe - returns a borrowed handle owned by the storage engine.
    FullOrderedIndex* resolve(WarehouseTableSlots* slots, int global_wid,
                                    int my_shard) {
        WarehouseTableSlots::Entry& e = slots->entries[global_wid - 1];
        int owner = tpcc_route_shard_for_warehouse(slots->table, global_wid);
        if (owner < 0) owner = (global_wid - 1) / wps_;   // ungoverned: static layout
        if (owner == my_shard) {
            FullOrderedIndex* idx = e.local.load(std::memory_order_acquire);
            if (idx != nullptr) return idx;
            return materialize_local(slots, global_wid, my_shard);
        }
        FullOrderedIndex* idx = e.remote.load(std::memory_order_acquire);
        if (idx != nullptr) return idx;
        return materialize_remote(slots, global_wid, owner);
    }

    // Convenience overload for cold callers (tests, data plane): resolves the
    // slot array by name first.
    FullOrderedIndex* resolve(const std::string& table, int global_wid,
                                    int my_shard) {
        return resolve(table_slots(table), global_wid, my_shard);
    }

    // Test hook: drop every slot array (handles are engine-owned, not freed).
    // @unsafe - only between tests, never while workers run.
    void reset() {
        std::lock_guard<std::mutex> guard(mu_);
        tables_.clear();
        wps_ = 1; total_ = 0;
        opener_ = Opener();
    }

private:
    WarehouseTableSlots* slots_for(const std::string& table) {
        std::lock_guard<std::mutex> guard(mu_);
        auto it = tables_.find(table);
        if (it == tables_.end()) {
            it = tables_.emplace(table,
                     new WarehouseTableSlots(table, static_cast<size_t>(total_)))
                     .first;
        }
        return it->second;
    }

    // Slow path: an ADOPTED warehouse (owned here, never materialized). The
    // opener creates an empty local index (the migration data plane fills it
    // before the cutover flips routing) and registers its routing alias.
    // @unsafe - engine open (via opener) under the directory mutex.
    FullOrderedIndex* materialize_local(WarehouseTableSlots* slots,
                                              int global_wid, int my_shard) {
        std::lock_guard<std::mutex> guard(mu_);
        WarehouseTableSlots::Entry& e = slots->entries[global_wid - 1];
        FullOrderedIndex* idx = e.local.load(std::memory_order_acquire);
        if (idx != nullptr) return idx;   // lost the race: someone materialized
        idx = opener_(slots->table, global_wid,
                      slots->table + "_adopted_" + std::to_string(global_wid),
                      my_shard);
        e.local.store(idx, std::memory_order_release);
        return idx;
    }

    // Slow path: a DEPARTED warehouse (statically ours, now owned elsewhere;
    // the startup layout opened no proxy for it). The opener creates one +
    // registers its alias; per-op routing chases the owner from then on.
    // @unsafe - engine open (via opener) under the directory mutex.
    FullOrderedIndex* materialize_remote(WarehouseTableSlots* slots,
                                               int global_wid, int owner) {
        std::lock_guard<std::mutex> guard(mu_);
        WarehouseTableSlots::Entry& e = slots->entries[global_wid - 1];
        FullOrderedIndex* idx = e.remote.load(std::memory_order_acquire);
        if (idx != nullptr) return idx;
        idx = opener_(slots->table, global_wid,
                      slots->table + "_remote_" + std::to_string(global_wid),
                      owner);
        e.remote.store(idx, std::memory_order_release);
        return idx;
    }

    int wps_ = 1;
    int total_ = 0;
    Opener opener_;
    // Guards slot-array creation and handle materialization; never taken on
    // the resolve fast path. std::mutex, not rusty::Mutex -- see header NOTE.
    std::mutex mu_;
    std::map<std::string, WarehouseTableSlots*> tables_;   // leaked: process lifetime
};

// @safe - returns reference to static instance
inline TpccWarehouseDirectory& get_warehouse_directory() {
    static TpccWarehouseDirectory instance;
    return instance;
}

}  // namespace mako
