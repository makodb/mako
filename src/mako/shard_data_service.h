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

private:
    ShardData* resolve(const std::string& table) {
        return catalog_ != nullptr ? catalog_->get_or_create(table) : nullptr;
    }

    ShardDataCatalog* catalog_;        // non-owning (unless owned_single_)
    SingleTableCatalog* owned_single_; // set by the single-table ctor
    MigrationGuard* guard_;            // non-owning; this shard's freeze registry.
};

// @unsafe - a ShardData bound to ONE named table on a remote shard's
// ShardDataService; every op is one RPC carrying the table name. The proxy must
// outlive this. The coordinator attaches one per (shard, table) it migrates.
class RemoteShardData : public ShardData {
public:
    explicit RemoteShardData(ShardDataServiceProxy* proxy,
                             std::string table_name = std::string())
        : proxy_(proxy), table_(std::move(table_name)) {}

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
        // A full scan = repeated bounded remote scans (keeps each message small).
        std::vector<KvPair> out;
        std::string cur = lo;
        while (cur < hi) {
            std::vector<KvPair> batch = scan_range_limited(cur, hi, 1024);
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
    bool faulted_ = false;           // latched on the first RPC failure.
};

}  // namespace janus
