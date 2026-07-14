#pragma once

// ShardDataService — the RPC transport for a shard's data plane, so a migration
// coordinator can drive a SOURCE or DESTINATION participant that lives in
// ANOTHER process.
//
//   ShardDataCatalog      : the per-shard registry of named migratable tables.
//                           Every RPC names its table; the service resolves it
//                           here, creating the table on first use — ONE service
//                           per shard covers all of its migratable tables. The
//                           production catalog (engine-backed standalone
//                           indexes) lives mako-side in shard_data_plane.cc.
//   ShardDataServiceImpl  : server side, runs ON a shard. Handler logic is
//                           factored into Do* methods (unit-testable with no
//                           socket); the RPC handlers just marshal, exactly like
//                           src/deptran/config_kv_service.h.
//   RemoteShardData       : client side -- a ShardData bound to ONE named table
//                           on the remote shard; every op is one RPC carrying
//                           that name. The coordinator attaches one per
//                           (shard, table) it migrates, so cross-process online
//                           migration reuses the SAME coordinator logic with
//                           zero changes. The range ops (checksum / verify_range
//                           / drop_range) are ONE RPC each -- the rows never
//                           cross the wire for those; only ScanRange (the bulk
//                           copy) ships pairs.

#include "rcc_rpc.h"        // ShardDataServiceService / ShardDataServiceProxy
import cluster;             // ShardData + ShardDataCatalog ports + MigrationGuard

#include <rusty/mutex.hpp>  // guards the pull-source connection cache

#include <unistd.h>   // usleep (drain poll pacing)

#include <condition_variable>   // fold worker handoff (see FoldJobStep)
#include <cstdint>
#include <map>
#include <mutex>    // fold job slot (guards state shared with its worker)
#include <string>
#include <thread>   // persistent fold compute worker (see FoldJobStep)
#include <utility>
#include <vector>

namespace janus {

// @safe - thin handler over an injected catalog (the shard's data planes).
class ShardDataServiceImpl : public ShardDataServiceService {
public:
    // `guard` defaults to this shard's process-global MigrationGuard (production);
    // tests inject their own so they can inspect the freeze state directly.
    explicit ShardDataServiceImpl(ShardDataCatalog* catalog,
                                  MigrationGuard* guard = nullptr)
        : catalog_(catalog),
          owned_single_(nullptr),
          guard_(guard != nullptr ? guard : &get_migration_guard()) {}
    // Single-table convenience (the RPC harness / gating tests): serve exactly
    // this ShardData for every table name.
    explicit ShardDataServiceImpl(ShardData* shard, MigrationGuard* guard = nullptr)
        : catalog_(nullptr),
          owned_single_(new SingleTableCatalog(shard)),
          guard_(guard != nullptr ? guard : &get_migration_guard()) {
        catalog_ = owned_single_;
    }
    ~ShardDataServiceImpl() {
        // @unsafe { stop and reap the fold + pull workers before members die }
        {
            std::lock_guard<std::mutex> g(ck_mu_);
            ck_stop_ = true;
        }
        ck_cv_.notify_one();
        if (ck_thread_.joinable()) ck_thread_.join();
        {
            std::lock_guard<std::mutex> g(pl_mu_);
            pl_stop_ = true;
        }
        pl_cv_.notify_one();
        if (pl_thread_.joinable()) pl_thread_.join();
        delete owned_single_;
    }

    // ---- handler logic, factored out so it unit-tests without the socket ----
    std::map<std::string, std::string>
    DoScanRange(const std::string& table, const std::string& lo,
                const std::string& hi, int limit) {
        std::map<std::string, std::string> rows;
        ShardData* shard = resolve(table);
        if (shard == nullptr) return rows;
        for (auto& kv : shard->scan_range_limited(lo, hi, static_cast<size_t>(limit)))
            rows.emplace(std::move(kv.first), std::move(kv.second));
        return rows;
    }
    uint64_t DoChecksum(const std::string& table, const std::string& lo,
                        const std::string& hi) {
        ShardData* shard = resolve(table);
        return shard != nullptr ? shard->checksum(lo, hi) : 0;
    }
    bool DoVerifyRange(const std::string& table, const std::string& lo,
                       const std::string& hi, uint64_t expected) {
        ShardData* shard = resolve(table);
        return shard != nullptr && shard->verify_range(lo, hi, expected);
    }
    void DoPut(const std::string& table, const std::string& key,
               const std::string& value) {
        ShardData* shard = resolve(table);
        if (shard != nullptr) shard->put(key, value);
    }
    void DoRemove(const std::string& table, const std::string& key) {
        ShardData* shard = resolve(table);
        if (shard != nullptr) shard->remove(key);
    }
    void DoDropRange(const std::string& table, const std::string& lo,
                     const std::string& hi) {
        ShardData* shard = resolve(table);
        if (shard != nullptr) shard->drop_range(lo, hi);
    }
    bool DoGet(const std::string& table, const std::string& key, std::string* out) {
        ShardData* shard = resolve(table);
        return shard != nullptr && out != nullptr && shard->get(key, *out);
    }
    // Freeze / unfreeze [lo,hi) of `table` on this shard for a migration.
    // Resolve through the catalog and let the PARTICIPANT freeze itself: it
    // knows its PHYSICAL table name -- the staging fence's lookup key
    // (t->get_table_name()) -- and its range semantics (wh-spec participants
    // widen to the whole index). Freezing the raw wire name here fenced
    // NOTHING for spec-named tables ('wh:6:customer' entry vs 'customer_1'
    // queries): every post-drain write sailed through, so the big-table
    // catch-up copy was always one write behind and the checksum gate
    // diverged (observed live across every customer/stock attempt). The raw
    // guard entry remains only for tables without a data plane here.
    void DoFreezeRange(const std::string& table, const std::string& lo,
                       const std::string& hi) {
        ShardData* shard = resolve(table);
        if (shard != nullptr) shard->freeze_range(lo, hi);
        // ALSO install the raw wire-name entry: the RPC's documented guard
        // contract (and the only fence for fakes whose freeze_range doesn't
        // touch the process guard). Unfreeze mirrors both, so the pair stays
        // symmetric.
        if (guard_) guard_->freeze(table, lo, hi);
    }
    void DoUnfreezeRange(const std::string& table, const std::string& lo,
                         const std::string& hi) {
        ShardData* shard = resolve(table);
        if (shard != nullptr) shard->unfreeze_range(lo, hi);
        if (guard_) guard_->unfreeze(table, lo, hi);
    }
    // Destination-driven bulk copy: THIS shard pulls [lo,hi) of src_table
    // directly from the source's ShardDataService at src_addr into its local
    // `table` -- the coordinator sent one control RPC and no row transits it.
    // Runs the whole chunked pull inside this handler (the coordinator's
    // Migrate call is synchronous anyway). Returns rows pulled, or -1 if the
    // local table or the source connection cannot be set up. Defined after
    // RemoteShardData below (it constructs one over the pull source).
    long DoPullRange(const std::string& table, const std::string& lo,
                     const std::string& hi, const std::string& src_addr,
                     const std::string& src_table);
    // Observable in tests: how many destination-driven pulls this service ran.
    int pull_range_calls = 0;

    // ---- RPC handlers (generated signatures; delegate to Do*) ----
    void ScanRange(const RpcScanRangeRequest& req, RpcScanRangeResponse& resp,
                   rrr::DeferredReply defer) override {
        resp.rows = DoScanRange(req.table_name, req.lo, req.hi, req.limit);
        defer.reply();
    }
    // Shared fold-job step for Checksum AND VerifyRange (see rcc_rpc.rpc):
    // both are the same full-range fold on one compute thread; VerifyRange
    // just compares the folded value to `expected` at reply time. BEGIN
    // (phase < 0) starts the fold, or joins an in-flight one for the same
    // range (other range: busy -- migrations are serialized, so that is a
    // stale prior job; the caller backs off and re-begins). POLL reports.
    // Returns done: 0 running, 1 *value valid, -1 stale id / busy.
    int FoldJobStep(const std::string& table, const std::string& lo,
                    const std::string& hi, rrr::i32 phase,
                    rrr::i32* job, uint64_t* value) {
        std::lock_guard<std::mutex> g(ck_mu_);
        const std::string key = table + '\0' + lo + '\0' + hi;
        *value = 0;
        if (phase < 0) {
            if (ck_running_) {
                *job = ck_id_;
                return ck_key_ == key ? 0 : -1;
            }
            // ONE persistent compute worker, started on first use, fed by the
            // slot -- NOT a thread per fold: fold threads engine-register on
            // their first storage op, and a mid-run stream of REGISTER ->
            // compute -> EXIT threads leaves the engine's epoch machinery
            // churning (lead suspect for a live crash decoding a value from
            // freed memory: reclamation racing a reader). A single long-lived
            // worker matches the process-lifetime service-thread model the
            // rest of the data plane uses.
            if (!ck_worker_started_) {
                ck_worker_started_ = true;
                // @unsafe { std::thread kernel: one process-lifetime compute
                //   worker, joined in the dtor }
                ck_thread_ = std::thread([this]() {
                    std::unique_lock<std::mutex> lk(ck_mu_);
                    while (true) {
                        ck_cv_.wait(lk, [this] { return ck_pending_ || ck_stop_; });
                        if (ck_stop_) return;
                        ck_pending_ = false;
                        const int id = ck_pending_id_;
                        const std::string t = ck_t_, l = ck_lo_, h = ck_hi_;
                        lk.unlock();
                        const uint64_t v = DoChecksum(t, l, h);
                        lk.lock();
                        if (id == ck_id_) {
                            ck_value_ = v; ck_has_ = true; ck_running_ = false;
                        }
                    }
                });
            }
            ck_id_++; ck_key_ = key; ck_running_ = true; ck_has_ = false;
            ck_pending_ = true;
            ck_pending_id_ = ck_id_;
            ck_t_ = table; ck_lo_ = lo; ck_hi_ = hi;
            ck_cv_.notify_one();
            *job = ck_id_;
            return 0;
        }
        *job = ck_id_;
        if (phase != ck_id_) return -1;   // stale/preempted job id
        if (!ck_has_) return 0;
        *value = ck_value_;
        return 1;
    }
    void Checksum(const RpcChecksumRequest& req, RpcChecksumResponse& resp,
                  rrr::DeferredReply defer) override {
        uint64_t v = 0;
        resp.done = FoldJobStep(req.table_name, req.lo, req.hi, req.phase,
                                &resp.job, &v);
        resp.checksum = static_cast<rrr::i64>(v);
        defer.reply();
    }
    void VerifyRange(const RpcVerifyRangeRequest& req, RpcVerifyRangeResponse& resp,
                     rrr::DeferredReply defer) override {
        uint64_t v = 0;
        resp.done = FoldJobStep(req.table_name, req.lo, req.hi, req.phase,
                                &resp.job, &v);
        resp.ok = (resp.done == 1 &&
                   v == static_cast<uint64_t>(req.expected)) ? 1 : 0;
        defer.reply();
    }
    void PutKey(const RpcPutKeyRequest& req, RpcPutKeyResponse& resp,
                rrr::DeferredReply defer) override {
        DoPut(req.table_name, req.key, req.value); resp.ok = 1; defer.reply();
    }
    void RemoveKey(const RpcRemoveKeyRequest& req, RpcRemoveKeyResponse& resp,
                   rrr::DeferredReply defer) override {
        DoRemove(req.table_name, req.key); resp.ok = 1; defer.reply();
    }
    void DropRange(const RpcDropRangeRequest& req, RpcDropRangeResponse& resp,
                   rrr::DeferredReply defer) override {
        DoDropRange(req.table_name, req.lo, req.hi); resp.ok = 1; defer.reply();
    }
    void GetKey(const RpcGetKeyRequest& req, RpcGetKeyResponse& resp,
                rrr::DeferredReply defer) override {
        std::string v;
        const bool found = DoGet(req.table_name, req.key, &v);
        resp.found = found ? 1 : 0;
        if (found) resp.value = std::move(v);
        defer.reply();
    }
    void FreezeRange(const RpcFreezeRangeRequest& req, RpcFreezeRangeResponse& resp,
                     rrr::DeferredReply defer) override {
        DoFreezeRange(req.table_name, req.lo, req.hi); resp.ok = 1; defer.reply();
    }
    void UnfreezeRange(const RpcUnfreezeRangeRequest& req, RpcUnfreezeRangeResponse& resp,
                       rrr::DeferredReply defer) override {
        DoUnfreezeRange(req.table_name, req.lo, req.hi); resp.ok = 1; defer.reply();
    }
    void PullRange(const RpcPullRangeRequest& req, RpcPullRangeResponse& resp,
                   rrr::DeferredReply defer) override {
        // BEGIN/POLL job (see rcc_rpc.rpc): the pull mirrors a whole range
        // server-side -- minutes for big tables -- on ONE persistent worker;
        // every call here replies immediately, under rrr's ~1s timeout.
        std::lock_guard<std::mutex> g(pl_mu_);
        const std::string key = req.table_name + '\0' + req.lo + '\0' +
                                req.hi + '\0' + req.src_addr + '\0' +
                                req.src_table;
        resp.copied = 0;
        resp.ok = 0;
        if (req.phase < 0) {
            if (pl_running_) {
                resp.done = (pl_key_ == key) ? 0 : -1;
                resp.job = pl_id_;
                defer.reply(); return;
            }
            if (!pl_worker_started_) {
                pl_worker_started_ = true;
                // @unsafe { std::thread kernel: one process-lifetime pull
                //   worker, joined in the dtor }
                pl_thread_ = std::thread([this]() {
                    std::unique_lock<std::mutex> lk(pl_mu_);
                    while (true) {
                        pl_cv_.wait(lk, [this] { return pl_pending_ || pl_stop_; });
                        if (pl_stop_) return;
                        pl_pending_ = false;
                        const int id = pl_pending_id_;
                        const std::string t = pl_t_, lo = pl_lo_, hi = pl_hi_;
                        const std::string sa = pl_src_addr_, st = pl_src_table_;
                        lk.unlock();
                        const long copied = DoPullRange(t, lo, hi, sa, st);
                        lk.lock();
                        if (id == pl_id_) {
                            pl_copied_ = copied; pl_has_ = true; pl_running_ = false;
                        }
                    }
                });
            }
            pl_id_++; pl_key_ = key; pl_running_ = true; pl_has_ = false;
            pl_pending_ = true;
            pl_pending_id_ = pl_id_;
            pl_t_ = req.table_name; pl_lo_ = req.lo; pl_hi_ = req.hi;
            pl_src_addr_ = req.src_addr; pl_src_table_ = req.src_table;
            pl_cv_.notify_one();
            resp.done = 0; resp.job = pl_id_;
            defer.reply(); return;
        }
        resp.job = pl_id_;
        if (req.phase != pl_id_)  resp.done = -1;   // stale/preempted job id
        else if (pl_has_) {
            resp.done = 1;
            resp.ok = pl_copied_ >= 0 ? 1 : 0;
            resp.copied = static_cast<rrr::i64>(pl_copied_ >= 0 ? pl_copied_ : 0);
        } else                    resp.done = 0;
        defer.reply();
    }
    void DrainWrites(const RpcDrainWritesRequest& req, RpcDrainWritesResponse& resp,
                     rrr::DeferredReply defer) override {
        // BEGIN/POLL protocol (see rcc_rpc.rpc): the server never blocks a
        // call on the full wait -- rrr requests carry a ~1s client timeout.
        ShardData* shard = resolve(req.table_name);
        if (shard == nullptr) {
            resp.ok = 0; resp.parity = -1; defer.reply(); return;
        }
        int parity = req.phase;
        if (parity < 0) {
            parity = shard->drain_begin();
            if (parity < 0) {
                // No split drain on this participant: fall back to the
                // blocking wait (in-memory doubles answer instantly).
                resp.ok = shard->drain_writes() ? 1 : 0;
                resp.parity = -1;
                defer.reply();
                return;
            }
        }
        resp.ok = shard->drain_poll(parity) ? 1 : 0;
        resp.parity = static_cast<rrr::i32>(parity);
        defer.reply();
    }

private:
    ShardData* resolve(const std::string& table) {
        return catalog_ != nullptr ? catalog_->get_or_create(table) : nullptr;
    }

    // Cached rrr connection to a PULL source (per address; leaked,
    // process-lifetime). One shared poll thread for all of them.
    // @unsafe - rrr client wiring, same shape as the bootstrap's.
    ShardDataServiceProxy* SourceProxy(const std::string& addr) {
        auto lk = src_mu_.lock().unwrap();
        auto it = src_proxies_.find(addr);
        if (it != src_proxies_.end()) return it->second;
        if (src_poll_ == nullptr) {
            src_poll_ = new rusty::Arc<rrr::PollThread>(rrr::PollThread::create());
        }
        auto* client = new rusty::Arc<rrr::Client>(
            rrr::Client::create(src_poll_->clone()));
        if ((*client)->connect(addr.c_str(), false) != 0) return nullptr;
        auto* proxy = new ShardDataServiceProxy(
            const_cast<rrr::Client*>(client->get()));
        src_proxies_.emplace(addr, proxy);
        return proxy;
    }
    // Drop a cached pull-source proxy whose connection went bad: the cache
    // is keyed by address and otherwise lives forever, so one dead
    // connection would poison every later pull from that source.
    void EvictSourceProxy(const std::string& addr) {
        auto lk = src_mu_.lock().unwrap();
        auto it = src_proxies_.find(addr);
        if (it != src_proxies_.end()) {
            // Proxy (and its client Arc) leak by design elsewhere in this
            // file; the map entry is what matters for freshness.
            src_proxies_.erase(it);
        }
    }

    ShardDataCatalog* catalog_;        // non-owning (unless owned_single_)
    SingleTableCatalog* owned_single_; // set by the single-table ctor
    MigrationGuard* guard_;            // non-owning; this shard's freeze registry.

    // Fold job slot (see FoldJobStep): one compute at a time on ONE
    // persistent worker, matching the serialized-migrations invariant.
    // @unsafe { std::thread + std::mutex + condvar kernel: the worker must
    // be stopped and reaped; rusty wrappers don't cover this handoff shape }
    std::mutex ck_mu_;
    std::condition_variable ck_cv_;
    std::thread ck_thread_;
    bool ck_worker_started_ = false;
    bool ck_stop_ = false;
    bool ck_pending_ = false;
    int ck_pending_id_ = 0;
    std::string ck_t_, ck_lo_, ck_hi_;
    int ck_id_ = 0;
    std::string ck_key_;
    bool ck_running_ = false;
    bool ck_has_ = false;
    uint64_t ck_value_ = 0;

    // Pull job slot: same shape as the fold slot, its own persistent worker
    // (a pull and a fold can overlap across the two sides of one migration).
    std::mutex pl_mu_;
    std::condition_variable pl_cv_;
    std::thread pl_thread_;
    bool pl_worker_started_ = false;
    bool pl_stop_ = false;
    bool pl_pending_ = false;
    int pl_pending_id_ = 0;
    std::string pl_t_, pl_lo_, pl_hi_, pl_src_addr_, pl_src_table_;
    int pl_id_ = 0;
    std::string pl_key_;
    bool pl_running_ = false;
    bool pl_has_ = false;
    long pl_copied_ = 0;
    rusty::Mutex<int> src_mu_{0};      // guards the pull-source connection cache
    std::map<std::string, ShardDataServiceProxy*> src_proxies_;
    rusty::Arc<rrr::PollThread>* src_poll_ = nullptr;   // lazy; leaked
};

// @unsafe - a ShardData bound to ONE named table on a remote shard's
// ShardDataService; every op is one RPC carrying the table name. The proxy must
// outlive this. The coordinator attaches one per (shard, table) it migrates.
class RemoteShardData : public ShardData {
public:
    // `addr` is the remote service's own host:port; it lets this participant
    // IDENTIFY itself as a pull source (service_addr) and lets the coordinator
    // delegate copies to it (copy_range_from -> one PullRange control RPC).
    explicit RemoteShardData(ShardDataServiceProxy* proxy,
                             std::string table_name = std::string(),
                             std::string addr = std::string())
        : proxy_(proxy), table_(std::move(table_name)), addr_(std::move(addr)) {}

    std::string service_addr() override { return addr_; }
    std::string service_table() override { return table_; }

    // Destination-driven copy: if the SOURCE is network-reachable, tell THIS
    // (remote destination) shard to pull the range directly from it -- one
    // control RPC, no row transits the coordinator. An unreachable source
    // (in-memory test double) falls back to the coordinator-side default.
    // BEGIN/POLL (see rcc_rpc.rpc): the destination shard mirrors the whole
    // range server-side -- minutes for the big tables -- so the caller polls
    // rather than waiting on one ~1s-budgeted request (the old synchronous
    // form timed out after exactly one request and latched faulted; the
    // return leg's first migration never copied a row). Deadline sized for
    // a 300k-row mirror under load; poll timeouts don't latch.
    size_t copy_range_from(ShardData* source, const std::string& lo,
                           const std::string& hi) override {
        const std::string src_addr = source->service_addr();
        if (src_addr.empty()) {
            return ShardData::copy_range_from(source, lo, hi);
        }
        rrr::i32 job = -1;
        bool any_reply = false;
        int errs = 0;
        for (int waited_ms = 0; waited_ms < 600000; ) {
            // A destination that has replied to NOTHING in ~15s is not
            // congested -- its cached connection is dead (observed live: the
            // handle minted during the forward leg went 462 straight
            // timeouts, zero replies, against a service that had just
            // answered hundreds of forward RPCs). Fail fast and latch: the
            // coordinator evicts the faulted handle and the retry attempt
            // reconnects fresh.
            if (!any_reply && waited_ms > 15000) break;
            ShardDataServiceProxy::RpcPullRangeRequest req;
            req.table_name = table_;
            req.lo = lo; req.hi = hi;
            req.src_addr = src_addr;
            req.src_table = source->service_table();
            req.phase = job;
            auto r = proxy_->PullRange(req);
            if (r.is_err()) {
                errs++;
                usleep(300 * 1000);
                waited_ms += 1300;   // ~1s timeout + backoff
                continue;
            }
            any_reply = true;
            auto resp = r.unwrap();
            if (resp.done == 1) {
                if (resp.ok == 0) {
                    // @unsafe { stderr diagnostics }
                    fprintf(stderr,
                            "pull '%s': server pull FAILED (resolve/source), "
                            "errs=%d waited=%dms\n",
                            table_.c_str(), errs, waited_ms);
                    faulted_ = true;
                    return 0;
                }
                return static_cast<size_t>(resp.copied);
            }
            if (resp.done == -1) {
                if (job >= 0) {
                    // @unsafe { stderr diagnostics }
                    fprintf(stderr, "pull '%s': job preempted, errs=%d waited=%dms\n",
                            table_.c_str(), errs, waited_ms);
                    faulted_ = true;
                    return 0;
                }
                usleep(300 * 1000);
                waited_ms += 300;
                continue;
            }
            job = resp.job;
            usleep(300 * 1000);
            waited_ms += 300;
        }
        // @unsafe { stderr diagnostics }
        fprintf(stderr, "pull '%s': deadline, any_reply=%d errs=%d\n",
                table_.c_str(), any_reply ? 1 : 0, errs);
        faulted_ = true;
        return 0;
    }

    void put(const std::string& key, const std::string& value) override {
        ShardDataServiceProxy::RpcPutKeyRequest req;
        req.table_name = table_; req.key = key; req.value = value;
        if (proxy_->PutKey(req).is_err()) faulted_ = true;
    }
    bool get(const std::string& key, std::string& out) override {
        ShardDataServiceProxy::RpcGetKeyRequest req;
        req.table_name = table_; req.key = key;
        auto r = proxy_->GetKey(req);
        if (r.is_err()) { faulted_ = true; return false; }
        auto resp = r.unwrap();
        if (!resp.found) return false;
        out = std::move(resp.value);
        return true;
    }
    void remove(const std::string& key) override {
        ShardDataServiceProxy::RpcRemoveKeyRequest req;
        req.table_name = table_; req.key = key;
        if (proxy_->RemoveKey(req).is_err()) faulted_ = true;
    }
    std::vector<KvPair> scan_range_limited(const std::string& lo,
                                           const std::string& hi,
                                           size_t limit) override {
        // Idempotent read; retried on timeout like checksum/verify above (a
        // dropped chunk would otherwise silently truncate a copy).
        std::vector<KvPair> out;
        for (int attempt = 0; attempt < 8; attempt++) {
            ShardDataServiceProxy::RpcScanRangeRequest req;
            req.table_name = table_;
            req.lo = lo; req.hi = hi; req.limit = static_cast<rrr::i32>(limit);
            auto r = proxy_->ScanRange(req);
            if (!r.is_err()) {
                for (auto& kv : r.unwrap().rows) out.emplace_back(kv.first, kv.second);
                return out;   // rows arrive sorted (a std::map) -> ascending vector
            }
            usleep(300 * 1000);   // congested: back off, retry
        }
        faulted_ = true;
        return out;
    }
    std::vector<KvPair> scan_range(const std::string& lo,
                                   const std::string& hi) override {
        // A full scan = repeated bounded remote scans (each response ~1MB max;
        // sized with copy_range_from's kCopyChunk for big workload tables).
        std::vector<KvPair> out;
        std::string cur = lo;
        while (cur < hi) {
            std::vector<KvPair> batch = scan_range_limited(cur, hi, 4096);
            if (batch.empty()) break;
            std::string last = batch.back().first;
            for (auto& kv : batch) out.push_back(std::move(kv));
            cur = last; cur.push_back('\0');
        }
        return out;
    }
    // Range ops as ONE RPC each -- the range's rows never cross the wire.
    // Checksum / verify are IDEMPOTENT reads whose server-side work (chunked
    // scans of a whole warehouse index, ~22 x 4096 rows) can exceed rrr's ~1s
    // client timeout under migration load. A timeout is congestion, not a
    // dead participant: retry within a deadline before latching. (Observed
    // live: the customer migration's src checksum arrived as 0 -- a 90k-row
    // fold is never 0 -- and every historical "checksum divergence" traces to
    // this timeout, not to data.)
    // BEGIN/POLL (see rcc_rpc.rpc): the server folds the range on ONE compute
    // thread; every RPC here replies fast. The old synchronous form retried
    // the whole fold on each ~1s timeout, stacking full-table scans on a
    // loaded shard -- a self-inflicted saturation spiral (observed live:
    // customer's drain stragglers stretched past budget while 8 piled folds
    // ran). Poll timeouts don't latch (congestion, not death); only a begin
    // that can't even start after several tries does.
    uint64_t checksum(const std::string& lo, const std::string& hi) override {
        rrr::i32 job = -1;
        bool any_reply = false;
        for (int waited_ms = 0; waited_ms < 90000; ) {
            ShardDataServiceProxy::RpcChecksumRequest req;
            req.table_name = table_; req.lo = lo; req.hi = hi;
            req.phase = job;
            auto r = proxy_->Checksum(req);
            if (r.is_err()) {
                // Congestion, not death: retry to the deadline (begin included
                // -- restarting/joining the fold is idempotent; see the drain
                // comment). Latch only on total silence below.
                usleep(300 * 1000);
                waited_ms += 1300;   // ~1s timeout + backoff
                continue;
            }
            any_reply = true;
            auto resp = r.unwrap();
            if (resp.done == 1) return static_cast<uint64_t>(resp.checksum);
            if (resp.done == -1) {
                // Slot busy with another range (begin) or our job was
                // preempted (poll). Migrations are serialized, so a stale
                // prior job is the only expected cause: re-begin; fail the
                // vote only if polling and preempted.
                if (job >= 0) return 0;
                usleep(300 * 1000);
                waited_ms += 300;
                continue;
            }
            job = resp.job;
            usleep(300 * 1000);
            waited_ms += 300;
        }
        if (!any_reply) faulted_ = true;   // 90s of silence: participant is gone
        return 0;   // deadline: fail this vote (a live-but-slow shard doesn't latch)
    }
    // BEGIN/POLL like checksum (the same fold, on the REMOTE destination --
    // the return-leg's vote): see the checksum comment above for why the
    // synchronous form self-destructs under load.
    bool verify_range(const std::string& lo, const std::string& hi,
                      uint64_t expected) override {
        rrr::i32 job = -1;
        bool any_reply = false;
        for (int waited_ms = 0; waited_ms < 90000; ) {
            ShardDataServiceProxy::RpcVerifyRangeRequest req;
            req.table_name = table_;
            req.lo = lo; req.hi = hi; req.expected = static_cast<rrr::i64>(expected);
            req.phase = job;
            auto r = proxy_->VerifyRange(req);
            if (r.is_err()) {
                // Congestion, not death: retry to the deadline (see checksum).
                usleep(300 * 1000);
                waited_ms += 1300;   // ~1s timeout + backoff
                continue;
            }
            any_reply = true;
            auto resp = r.unwrap();
            if (resp.done == 1) return resp.ok != 0;
            if (resp.done == -1) {
                if (job >= 0) return false;   // preempted mid-poll: fail the vote
                usleep(300 * 1000);
                waited_ms += 300;
                continue;
            }
            job = resp.job;
            usleep(300 * 1000);
            waited_ms += 300;
        }
        if (!any_reply) faulted_ = true;   // 90s of silence: participant is gone
        return false;   // deadline: fail the vote (a live-but-slow shard doesn't latch)
    }
    void drop_range(const std::string& lo, const std::string& hi) override {
        ShardDataServiceProxy::RpcDropRangeRequest req;
        req.table_name = table_; req.lo = lo; req.hi = hi;
        if (proxy_->DropRange(req).is_err()) faulted_ = true;
    }

    // Write drain on the REMOTE shard, as BEGIN/POLL: the first call bumps
    // the remote watermark once and returns the bucket cookie; subsequent
    // calls poll it with a short server budget, each staying under rrr's ~1s
    // request timeout. Loops to a 30s deadline (2PC tails under migration
    // load reach ~14s). No call fault-latches on a timeout: under migration
    // load both begin and polls queue behind the data service's scan traffic
    // and trip rrr's ~1s client timeout -- congestion, not a dead participant
    // (observed live twice: polls first, then the BEGIN itself latching
    // "write drain timed out" while the shard served fine). Retrying BEGIN is
    // safe under the 16-generation-bucket watermark: an extra bump just burns
    // a bucket, and the newest cookie's wait covers every older one -- unlike
    // the old two-parity scheme, where a double bump recycled the waited
    // parity (the comment that used to forbid this). Cap begin retries so a
    // truly dead participant still fails fast.
    bool drain_writes() override {
        rrr::i32 phase = -1;
        bool any_reply = false;
        // 60s deadline: the gen-bucket watermark now (correctly) waits out
        // real pre-fence stragglers -- cross-shard 2PC tails observed at
        // 6-14s under migration load, longer right after a bulk copy. No
        // fixed retry cap on BEGIN: a bulk copy saturates the participant's
        // rrr workers for 10s+ stretches and every control RPC times out
        // (observed live: a whole run's tail failed on begins that never got
        // through, zero participant-side drain logs) -- and re-beginning is
        // SAFE under the gen-bucket watermark (an extra bump burns a bucket;
        // the newest cookie waits every older one). Latch faulted only on
        // TOTAL silence for the whole window.
        for (int waited_ms = 0; waited_ms < 60000; ) {
            ShardDataServiceProxy::RpcDrainWritesRequest req;
            req.table_name = table_;
            req.phase = phase;
            auto r = proxy_->DrainWrites(req);
            if (r.is_err()) {
                usleep(200 * 1000);   // congested: back off, retry/re-poll
                waited_ms += 1200;    // ~1s client timeout + backoff
                continue;
            }
            any_reply = true;
            auto resp = r.unwrap();
            if (resp.ok != 0) return true;
            if (resp.parity < 0) return false;   // no split drain remotely; it said no
            phase = resp.parity;
            usleep(100 * 1000);   // server budget ~400ms + this = ~2 polls/sec
            waited_ms += 500;
        }
        if (!any_reply) faulted_ = true;   // 60s of silence: participant is gone
        return false;
    }

    // Migration write fence on the REMOTE shard (ShardData overrides): one RPC to
    // the remote service, which freezes ITS process-global MigrationGuard under
    // this table's name. The coordinator's lock_range() calls this on the source.
    void freeze_range(const std::string& lo, const std::string& hi) override {
        ShardDataServiceProxy::RpcFreezeRangeRequest req;
        req.table_name = table_; req.lo = lo; req.hi = hi;
        if (proxy_->FreezeRange(req).is_err()) faulted_ = true;
    }
    void unfreeze_range(const std::string& lo, const std::string& hi) override {
        ShardDataServiceProxy::RpcUnfreezeRangeRequest req;
        req.table_name = table_; req.lo = lo; req.hi = hi;
        if (proxy_->UnfreezeRange(req).is_err()) faulted_ = true;
    }

    // Any RPC error on this participant (dead peer, connection loss) latches the
    // fault bit; the coordinator refuses to vote prepared over a faulted
    // participant (see ShardData::faulted).
    bool faulted() override { return faulted_; }

private:
    ShardDataServiceProxy* proxy_;   // non-owning; connected to the remote shard.
    std::string table_;              // the one table this participant is bound to.
    std::string addr_;               // the remote service's own host:port ("" = unknown).
    bool faulted_ = false;           // latched on the first RPC failure.
};

// @unsafe - rrr client wiring + storage writes (see the in-class declaration).
inline long ShardDataServiceImpl::DoPullRange(const std::string& table,
                                              const std::string& lo,
                                              const std::string& hi,
                                              const std::string& src_addr,
                                              const std::string& src_table) {
    ShardData* dest = resolve(table);
    if (dest == nullptr) {
        Log_warn("ShardDataPlane: pull FAILED resolving local '%s'", table.c_str());
        return -1;
    }
    ShardDataServiceProxy* sp = SourceProxy(src_addr);
    if (sp == nullptr) {
        Log_warn("ShardDataPlane: pull FAILED connecting source %s for '%s'",
                 src_addr.c_str(), table.c_str());
        return -1;
    }
    RemoteShardData src(sp, src_table);
    // The DESTINATION participant drives the pull: its copy_range_from knows
    // its own range semantics (wh-spec participants widen to the whole
    // index -- a hand-rolled loop here scanned from the publish-range lo and
    // read nothing) and mirrors (put + delete-extras) rather than just puts.
    // dest is local to this process, so the base mirror runs right here with
    // local puts/removes and chunked RPC scans of the source.
    const size_t copied = dest->copy_range_from(&src, lo, hi);
    if (src.faulted()) {
        // Dead source: report failure, not a short copy -- and evict the
        // cached connection so the coordinator's retry pulls over a fresh
        // one instead of the same dead socket.
        Log_warn("ShardDataPlane: pull of '%s' FAULTED against source %s "
                 "table '%s' (scans unanswered); evicting cached source proxy",
                 table.c_str(), src_addr.c_str(), src_table.c_str());
        EvictSourceProxy(src_addr);
        return -1;
    }
    pull_range_calls++;
    Log_info("ShardDataPlane: pulled %zu rows of '%s' [%s,%s) directly from %s",
             copied, table.c_str(), lo.c_str(), hi.c_str(), src_addr.c_str());
    return static_cast<long>(copied);
}

}  // namespace janus
