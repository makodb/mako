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

#include <cstdint>
#include <map>
#include <string>
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
    ~ShardDataServiceImpl() { delete owned_single_; }

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
    // Freeze / unfreeze [lo,hi) of `table` on this shard for a migration. The
    // entry carries the REAL table name; the non-txn write handler resolves its
    // op's table name and queries the guard with it (a "" entry still fences
    // every table -- see MigrationGuard).
    void DoFreezeRange(const std::string& table, const std::string& lo,
                       const std::string& hi) {
        if (guard_) guard_->freeze(table, lo, hi);
    }
    void DoUnfreezeRange(const std::string& table, const std::string& lo,
                         const std::string& hi) {
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
    void Checksum(const RpcChecksumRequest& req, RpcChecksumResponse& resp,
                  rrr::DeferredReply defer) override {
        resp.checksum = static_cast<rrr::i64>(DoChecksum(req.table_name, req.lo, req.hi));
        defer.reply();
    }
    void VerifyRange(const RpcVerifyRangeRequest& req, RpcVerifyRangeResponse& resp,
                     rrr::DeferredReply defer) override {
        resp.ok = DoVerifyRange(req.table_name, req.lo, req.hi,
                                static_cast<uint64_t>(req.expected)) ? 1 : 0;
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
        const long copied = DoPullRange(req.table_name, req.lo, req.hi,
                                        req.src_addr, req.src_table);
        resp.ok = copied >= 0 ? 1 : 0;
        resp.copied = static_cast<rrr::i64>(copied >= 0 ? copied : 0);
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

    ShardDataCatalog* catalog_;        // non-owning (unless owned_single_)
    SingleTableCatalog* owned_single_; // set by the single-table ctor
    MigrationGuard* guard_;            // non-owning; this shard's freeze registry.
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
    void copy_range_from(ShardData* source, const std::string& lo,
                         const std::string& hi) override {
        const std::string src_addr = source->service_addr();
        if (src_addr.empty()) {
            ShardData::copy_range_from(source, lo, hi);
            return;
        }
        ShardDataServiceProxy::RpcPullRangeRequest req;
        req.table_name = table_;
        req.lo = lo; req.hi = hi;
        req.src_addr = src_addr;
        req.src_table = source->service_table();
        auto r = proxy_->PullRange(req);
        if (r.is_err() || r.unwrap().ok == 0) faulted_ = true;
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
        std::vector<KvPair> out;
        ShardDataServiceProxy::RpcScanRangeRequest req;
        req.table_name = table_;
        req.lo = lo; req.hi = hi; req.limit = static_cast<rrr::i32>(limit);
        auto r = proxy_->ScanRange(req);
        if (r.is_err()) { faulted_ = true; return out; }
        for (auto& kv : r.unwrap().rows) out.emplace_back(kv.first, kv.second);
        return out;   // rows arrive sorted (a std::map) -> ascending vector
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
    uint64_t checksum(const std::string& lo, const std::string& hi) override {
        ShardDataServiceProxy::RpcChecksumRequest req;
        req.table_name = table_; req.lo = lo; req.hi = hi;
        auto r = proxy_->Checksum(req);
        if (r.is_err()) { faulted_ = true; return 0; }
        return static_cast<uint64_t>(r.unwrap().checksum);
    }
    bool verify_range(const std::string& lo, const std::string& hi,
                      uint64_t expected) override {
        ShardDataServiceProxy::RpcVerifyRangeRequest req;
        req.table_name = table_;
        req.lo = lo; req.hi = hi; req.expected = static_cast<rrr::i64>(expected);
        auto r = proxy_->VerifyRange(req);
        if (r.is_err()) { faulted_ = true; return false; }
        return r.unwrap().ok != 0;
    }
    void drop_range(const std::string& lo, const std::string& hi) override {
        ShardDataServiceProxy::RpcDropRangeRequest req;
        req.table_name = table_; req.lo = lo; req.hi = hi;
        if (proxy_->DropRange(req).is_err()) faulted_ = true;
    }

    // Write drain on the REMOTE shard, as BEGIN/POLL: the first call bumps
    // the remote watermark once and returns the parity; subsequent calls
    // poll it with a short server budget, each staying under rrr's ~1s
    // request timeout. Loops to a 30s deadline (2PC tails under migration
    // load reach ~14s). A POLL that errors does NOT fault-latch: polls are
    // idempotent reads, and under migration load they queue behind the data
    // service's scan traffic and trip rrr's ~1s client timeout -- that is
    // congestion, not a dead participant (observed live: customer's drain
    // aborted "participant unreachable" while the shard served fine). Only
    // the BEGIN call (phase < 0, the one generation bump) latches on error,
    // since retrying it blindly would double-bump the watermark.
    bool drain_writes() override {
        rrr::i32 phase = -1;
        for (int waited_ms = 0; waited_ms < 30000; ) {
            ShardDataServiceProxy::RpcDrainWritesRequest req;
            req.table_name = table_;
            req.phase = phase;
            auto r = proxy_->DrainWrites(req);
            if (r.is_err()) {
                if (phase < 0) { faulted_ = true; return false; }
                usleep(200 * 1000);   // congested: back off, re-poll
                waited_ms += 700;
                continue;
            }
            auto resp = r.unwrap();
            if (resp.ok != 0) return true;
            if (resp.parity < 0) return false;   // no split drain remotely; it said no
            phase = resp.parity;
            usleep(100 * 1000);   // server budget ~400ms + this = ~2 polls/sec
            waited_ms += 500;
        }
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
    if (dest == nullptr) return -1;
    ShardDataServiceProxy* sp = SourceProxy(src_addr);
    if (sp == nullptr) return -1;
    RemoteShardData src(sp, src_table);
    static const size_t kCopyChunk = 512;
    long copied = 0;
    std::string cur = lo;
    while (cur < hi) {
        std::vector<ShardData::KvPair> batch =
            src.scan_range_limited(cur, hi, kCopyChunk);
        if (batch.empty()) break;
        for (const auto& kv : batch) dest->put(kv.first, kv.second);
        copied += static_cast<long>(batch.size());
        cur = batch.back().first;
        cur.push_back('\0');   // resume strictly after the last key copied
    }
    if (src.faulted()) return -1;   // dead source: report failure, not a short copy
    pull_range_calls++;
    Log_info("ShardDataPlane: pulled %ld rows of '%s' [%s,%s) directly from %s",
             copied, table.c_str(), lo.c_str(), hi.c_str(), src_addr.c_str());
    return copied;
}

}  // namespace janus
