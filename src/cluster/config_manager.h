#pragma once
#include "kv_store.h"
#include <string>
#include <vector>
#include <cstdint>
#include <charconv>

// deref_if_pointer_like (raw-pointer field method calls) used by the
// DSL-generated bodies below. rusty, not rrr.
#include <rusty/slice.hpp>

namespace janus {

/**
 * ConfigManager - Typed configuration management over a KvStore.
 *
 * Wraps a KvStore port (get/put/remove). In production the port is
 * bound to Mako's unified FullOrderedIndex — the __mako_config__ system
 * table on shard 0 — via the OrderedIndexKvStore adapter; tests bind an
 * in-memory fake. cluster depends only on the port, so it compiles and
 * unit-tests with no storage-engine headers. The metadata really lives
 * in the unified store; this is a decoupling seam, not a parallel one.
 *
 * Consistency: multi-key writes are applied as a sequence of point puts
 * with the __version__ key written LAST (BumpVersion, called after the
 * data writes), so a reader that observes a new __version__ is
 * guaranteed to see all keys of that version. This is not atomic across
 * keys the way a transaction would be, but config is single-writer
 * (shard 0's leader) and low-frequency; a reader that races an in-flight
 * change self-heals on its next version poll, and a transient misroute
 * is caught by the WrongShard-retry path. (Under the "shard 0 never
 * fails" assumption we don't replicate this table.)
 *
 * Key schema:
 *   __version__             — monotonically increasing config version (uint64)
 *   shard_count             — total number of shards (uint32)
 *   shard/<id>/replicas     — comma-separated list of replica site IDs
 *   shard/<id>/leader       — current leader site name
 *   shard/<id>/status       — active, draining, adding, removing, dead
 *   shard/<id>/replacement  — for status=dead: taker shard id
 *   epoch                   — global speculative epoch number (uint64)
 *   node/<site>/addr        — node network address
 *   node/<site>/status      — alive, dead, decommissioning
 *   sharding/mode           — default routing mode "hash" or "range"
 *   sharding/policy/<table> — opaque serialized TableShardingPolicy bytes
 *   sharding/policy_tables  — comma-separated list of tables with a policy
 *
 * Authored in the inline-Rust DSL (docs/storage-interface.md): the
 * `#if RUSTYCPP_RUST` block is the source of truth; the GEN block is the
 * generated C++. Regenerate with scripts/regen_storage_dsl.sh. The kv
 * field is a borrowed raw pointer; method calls go through it with the
 * `(*(*self).kv).m()` deref form. Three operations the DSL can't spell
 * stay as small C++ kernels below: a no-throw integer parse (the old
 * code caught std::stoull exceptions), a null-pointer test for the
 * borrowed store, and the CSV split (find/npos iteration).
 */

// @safe - no-throw parse; returns 0 on failure (matches the old
// std::stoull/std::stoul + catch(...) -> 0 behaviour).
inline uint64_t cm_parse_u64(const std::string& s) {
    uint64_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

// @safe - null test for the borrowed KvStore pointer.
inline bool cm_kv_absent(const KvStore* kv) { return kv == nullptr; }

// @safe - split a comma-separated list into a vector; "" -> empty.
inline std::vector<std::string> cm_split_csv(const std::string& csv) {
    std::vector<std::string> result;
    if (csv.empty()) return result;
    size_t start = 0;
    size_t pos = csv.find(',');
    while (pos != std::string::npos) {
        result.push_back(csv.substr(start, pos - start));
        start = pos + 1;
        pos = csv.find(',', start);
    }
    result.push_back(csv.substr(start));
    return result;
}

#if RUSTYCPP_RUST
pub struct ConfigManager {
    kv: *mut KvStore,
}
impl ConfigManager {
    // ---- private helpers -------------------------------------------------
    // Join replicas into a comma-separated string.
    fn JoinReplicas(&self, replicas: &std::vector<std::string>) -> std::string {
        let mut result: std::string = std::string("");
        let mut i: usize = 0;
        while i < (*replicas).size() {
            if i > 0 { result = result + std::string(","); }
            result = result + (*replicas)[i];
            i = i + 1;
        }
        result
    }
    // Write key=value then bump __version__ (written last).
    fn PutVersioned(&mut self, key: &std::string, value: &std::string) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        unsafe { (*(*self).kv).put(key, value); }
        self.BumpVersion();
        true
    }
    // Read __version__, write it back incremented (the last visible write).
    fn BumpVersion(&mut self) {
        let vkey: std::string = std::string("__version__");
        let mut vs: std::string = std::string("");
        let mut cur: u64 = 0;
        if unsafe { (*(*self).kv).get(&vkey, &mut vs) } {
            cur = unsafe { cm_parse_u64(&vs) };
        }
        let nv: std::string = std::to_string(cur + 1);
        unsafe { (*(*self).kv).put(&vkey, &nv); }
    }

    // ---- version ---------------------------------------------------------
    fn GetVersion(&mut self) -> u64 {
        if unsafe { cm_kv_absent((*self).kv) } { return 0; }
        let vkey: std::string = std::string("__version__");
        let mut value: std::string = std::string("");
        if unsafe { (*(*self).kv).get(&vkey, &mut value) } {
            return unsafe { cm_parse_u64(&value) };
        }
        0
    }

    // ---- shard management ------------------------------------------------
    fn GetShardCount(&mut self) -> u32 {
        if unsafe { cm_kv_absent((*self).kv) } { return 0; }
        let key: std::string = std::string("shard_count");
        let mut value: std::string = std::string("");
        if unsafe { (*(*self).kv).get(&key, &mut value) } {
            return unsafe { cm_parse_u64(&value) } as u32;
        }
        0
    }
    fn SetShardCount(&mut self, count: u32) -> bool {
        let key: std::string = std::string("shard_count");
        let val: std::string = std::to_string(count);
        self.PutVersioned(&key, &val)
    }
    fn GetShardReplicas(&mut self, shard_id: u32) -> std::vector<std::string> {
        let mut value: std::string = std::string("");
        if !unsafe { cm_kv_absent((*self).kv) } {
            let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/replicas");
            unsafe { (*(*self).kv).get(&key, &mut value); }
        }
        unsafe { cm_split_csv(&value) }
    }
    fn SetShardReplicas(&mut self, shard_id: u32, replicas: &std::vector<std::string>) -> bool {
        let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/replicas");
        let val: std::string = self.JoinReplicas(replicas);
        self.PutVersioned(&key, &val)
    }
    fn GetShardLeader(&mut self, shard_id: u32) -> std::string {
        if unsafe { cm_kv_absent((*self).kv) } { return std::string(""); }
        let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/leader");
        let mut value: std::string = std::string("");
        if unsafe { (*(*self).kv).get(&key, &mut value) } { return value; }
        std::string("")
    }
    fn SetShardLeader(&mut self, shard_id: u32, leader: &std::string) -> bool {
        let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/leader");
        self.PutVersioned(&key, leader)
    }
    fn GetShardStatus(&mut self, shard_id: u32) -> std::string {
        if unsafe { cm_kv_absent((*self).kv) } { return std::string(""); }
        let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/status");
        let mut value: std::string = std::string("");
        if unsafe { (*(*self).kv).get(&key, &mut value) } { return value; }
        std::string("")
    }
    fn SetShardStatus(&mut self, shard_id: u32, status: &std::string) -> bool {
        let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/status");
        self.PutVersioned(&key, status)
    }

    // ---- shard lifecycle -------------------------------------------------
    fn AddShard(&mut self, shard_id: u32, replicas: &std::vector<std::string>) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        let count: u32 = self.GetShardCount();
        let rkey: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/replicas");
        let rval: std::string = self.JoinReplicas(replicas);
        unsafe { (*(*self).kv).put(&rkey, &rval); }
        let skey: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/status");
        let sval: std::string = std::string("active");
        unsafe { (*(*self).kv).put(&skey, &sval); }
        let ckey: std::string = std::string("shard_count");
        let cval: std::string = std::to_string(count + 1);
        unsafe { (*(*self).kv).put(&ckey, &cval); }
        self.BumpVersion();
        true
    }
    fn RemoveShard(&mut self, shard_id: u32) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        let count: u32 = self.GetShardCount();
        if count == 0 { return false; }
        let rkey: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/replicas");
        unsafe { (*(*self).kv).remove(&rkey); }
        let lkey: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/leader");
        unsafe { (*(*self).kv).remove(&lkey); }
        let skey: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/status");
        unsafe { (*(*self).kv).remove(&skey); }
        let ckey: std::string = std::string("shard_count");
        let cval: std::string = std::to_string(count - 1);
        unsafe { (*(*self).kv).put(&ckey, &cval); }
        self.BumpVersion();
        true
    }
    fn GetShardReplacement(&mut self, shard_id: u32) -> u32 {
        if unsafe { cm_kv_absent((*self).kv) } { return 0; }
        let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/replacement");
        let mut value: std::string = std::string("");
        if unsafe { (*(*self).kv).get(&key, &mut value) } {
            return unsafe { cm_parse_u64(&value) } as u32;
        }
        0
    }
    fn KillShard(&mut self, dead_id: u32, taker_id: u32) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        if dead_id == taker_id { return false; }
        if self.GetShardStatus(dead_id).empty() { return false; }
        if self.GetShardReplicas(taker_id).empty() { return false; }
        let epoch: u64 = self.GetEpoch() + 1;
        let dkey: std::string = std::string("shard/") + std::to_string(dead_id) + std::string("/status");
        let dval: std::string = std::string("dead");
        unsafe { (*(*self).kv).put(&dkey, &dval); }
        let pkey: std::string = std::string("shard/") + std::to_string(dead_id) + std::string("/replacement");
        let pval: std::string = std::to_string(taker_id);
        unsafe { (*(*self).kv).put(&pkey, &pval); }
        let rkey: std::string = std::string("shard/") + std::to_string(dead_id) + std::string("/replicas");
        let empty: std::string = std::string("");
        unsafe { (*(*self).kv).put(&rkey, &empty); }
        let ekey: std::string = std::string("epoch");
        let eval: std::string = std::to_string(epoch);
        unsafe { (*(*self).kv).put(&ekey, &eval); }
        self.BumpVersion();
        true
    }

    // ---- epoch -----------------------------------------------------------
    fn GetEpoch(&mut self) -> u64 {
        if unsafe { cm_kv_absent((*self).kv) } { return 0; }
        let key: std::string = std::string("epoch");
        let mut value: std::string = std::string("");
        if unsafe { (*(*self).kv).get(&key, &mut value) } {
            return unsafe { cm_parse_u64(&value) };
        }
        0
    }
    fn AdvanceEpoch(&mut self) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        let epoch: u64 = self.GetEpoch() + 1;
        let key: std::string = std::string("epoch");
        let val: std::string = std::to_string(epoch);
        self.PutVersioned(&key, &val)
    }

    // ---- node management -------------------------------------------------
    fn GetNodeAddr(&mut self, site: &std::string) -> std::string {
        if unsafe { cm_kv_absent((*self).kv) } { return std::string(""); }
        let key: std::string = (std::string("node/") + *site) + std::string("/addr");
        let mut value: std::string = std::string("");
        if unsafe { (*(*self).kv).get(&key, &mut value) } { return value; }
        std::string("")
    }
    fn SetNodeAddr(&mut self, site: &std::string, addr: &std::string) -> bool {
        let key: std::string = (std::string("node/") + *site) + std::string("/addr");
        self.PutVersioned(&key, addr)
    }
    fn GetNodeStatus(&mut self, site: &std::string) -> std::string {
        if unsafe { cm_kv_absent((*self).kv) } { return std::string(""); }
        let key: std::string = (std::string("node/") + *site) + std::string("/status");
        let mut value: std::string = std::string("");
        if unsafe { (*(*self).kv).get(&key, &mut value) } { return value; }
        std::string("")
    }
    fn SetNodeStatus(&mut self, site: &std::string, status: &std::string) -> bool {
        let key: std::string = (std::string("node/") + *site) + std::string("/status");
        self.PutVersioned(&key, status)
    }

    // ---- sharding policy (opaque bytes) ----------------------------------
    fn GetShardingMode(&mut self) -> std::string {
        if unsafe { cm_kv_absent((*self).kv) } { return std::string(""); }
        let key: std::string = std::string("sharding/mode");
        let mut value: std::string = std::string("");
        if unsafe { (*(*self).kv).get(&key, &mut value) } { return value; }
        std::string("")
    }
    fn SetShardingMode(&mut self, mode: &std::string) -> bool {
        let key: std::string = std::string("sharding/mode");
        self.PutVersioned(&key, mode)
    }
    fn GetShardingPolicy(&mut self, table: &std::string) -> std::string {
        if unsafe { cm_kv_absent((*self).kv) } { return std::string(""); }
        if (*table).empty() { return std::string(""); }
        let key: std::string = std::string("sharding/policy/") + *table;
        let mut value: std::string = std::string("");
        if unsafe { (*(*self).kv).get(&key, &mut value) } { return value; }
        std::string("")
    }
    fn ListShardingPolicyTables(&mut self) -> std::vector<std::string> {
        let mut value: std::string = std::string("");
        if !unsafe { cm_kv_absent((*self).kv) } {
            let key: std::string = std::string("sharding/policy_tables");
            unsafe { (*(*self).kv).get(&key, &mut value); }
        }
        unsafe { cm_split_csv(&value) }
    }
    fn SetShardingPolicy(&mut self, table: &std::string, serialized_policy: &std::string) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        if (*table).empty() { return false; }
        if (*serialized_policy).empty() { return false; }
        let mut tables: std::vector<std::string> = self.ListShardingPolicyTables();
        let mut present: bool = false;
        let mut i: usize = 0;
        while i < tables.size() {
            if tables[i] == *table { present = true; }
            i = i + 1;
        }
        let pkey: std::string = std::string("sharding/policy/") + *table;
        unsafe { (*(*self).kv).put(&pkey, serialized_policy); }
        if !present {
            tables.push_back(*table);
            let tkey: std::string = std::string("sharding/policy_tables");
            let tval: std::string = self.JoinReplicas(&tables);
            unsafe { (*(*self).kv).put(&tkey, &tval); }
        }
        self.BumpVersion();
        true
    }
    fn DeleteShardingPolicy(&mut self, table: &std::string) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        if (*table).empty() { return false; }
        let tables: std::vector<std::string> = self.ListShardingPolicyTables();
        let mut pruned: std::vector<std::string> = cm_split_csv(&std::string(""));
        let mut i: usize = 0;
        while i < tables.size() {
            if tables[i] != *table { pruned.push_back(tables[i]); }
            i = i + 1;
        }
        if pruned.size() == tables.size() && self.GetShardingPolicy(table).empty() {
            return true;
        }
        let pkey: std::string = std::string("sharding/policy/") + *table;
        unsafe { (*(*self).kv).remove(&pkey); }
        let tkey: std::string = std::string("sharding/policy_tables");
        let tval: std::string = self.JoinReplicas(&pruned);
        unsafe { (*(*self).kv).put(&tkey, &tval); }
        self.BumpVersion();
        true
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=config_manager.1 version=1 rust_sha256=981b15d877560ea8ebeec276079a049645bb6aef3e7af5ce8b007442259a1f9b*/
struct ConfigManager;

struct ConfigManager {
    KvStore* kv;

    std::string JoinReplicas(const std::vector<std::string>& replicas) const;
    bool PutVersioned(const std::string& key, const std::string& value);
    void BumpVersion();
    uint64_t GetVersion();
    uint32_t GetShardCount();
    bool SetShardCount(uint32_t count);
    std::vector<std::string> GetShardReplicas(uint32_t shard_id);
    bool SetShardReplicas(uint32_t shard_id, const std::vector<std::string>& replicas);
    std::string GetShardLeader(uint32_t shard_id);
    bool SetShardLeader(uint32_t shard_id, const std::string& leader);
    std::string GetShardStatus(uint32_t shard_id);
    bool SetShardStatus(uint32_t shard_id, const std::string& status);
    bool AddShard(uint32_t shard_id, const std::vector<std::string>& replicas);
    bool RemoveShard(uint32_t shard_id);
    uint32_t GetShardReplacement(uint32_t shard_id);
    bool KillShard(uint32_t dead_id, uint32_t taker_id);
    uint64_t GetEpoch();
    bool AdvanceEpoch();
    std::string GetNodeAddr(const std::string& site);
    bool SetNodeAddr(const std::string& site, const std::string& addr);
    std::string GetNodeStatus(const std::string& site);
    bool SetNodeStatus(const std::string& site, const std::string& status);
    std::string GetShardingMode();
    bool SetShardingMode(const std::string& mode);
    std::string GetShardingPolicy(const std::string& table);
    std::vector<std::string> ListShardingPolicyTables();
    bool SetShardingPolicy(const std::string& table, const std::string& serialized_policy);
    bool DeleteShardingPolicy(const std::string& table);
};


inline std::string ConfigManager::JoinReplicas(const std::vector<std::string>& replicas) const {
    std::string result = std::string("");
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((replicas)).size()) {
        if (rusty::detail::deref_if_pointer_like(i) > 0) {
            result = rusty::detail::deref_if_pointer_like(result) + std::string(",");
        }
        result = rusty::detail::deref_if_pointer_like(result) + (replicas)[i];
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    return std::move(result);
}

inline bool ConfigManager::PutVersioned(const std::string& key, const std::string& value) {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(key, value);
    }
    this->BumpVersion();
    return true;
}

inline void ConfigManager::BumpVersion() {
    const std::string vkey = std::string("__version__");
    std::string vs = std::string("");
    uint64_t cur = static_cast<uint64_t>(0);
    if (((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(vkey, &vs)) {
        cur = cm_parse_u64(vs);
    }
    const std::string nv = std::to_string(rusty::detail::deref_if_pointer_like(cur) + 1);
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(vkey, nv);
    }
}

inline uint64_t ConfigManager::GetVersion() {
    if (cm_kv_absent(((*this)).kv)) {
        return static_cast<uint64_t>(0);
    }
    const std::string vkey = std::string("__version__");
    std::string value = std::string("");
    if (((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(vkey, &value)) {
        return cm_parse_u64(value);
    }
    return static_cast<uint64_t>(0);
}

inline uint32_t ConfigManager::GetShardCount() {
    if (cm_kv_absent(((*this)).kv)) {
        return static_cast<uint32_t>(0);
    }
    const std::string key = std::string("shard_count");
    std::string value = std::string("");
    if (((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key, &value)) {
        return static_cast<uint32_t>(cm_parse_u64(value));
    }
    return static_cast<uint32_t>(0);
}

inline bool ConfigManager::SetShardCount(uint32_t count) {
    const std::string key = std::string("shard_count");
    const std::string val = std::to_string(std::move(count));
    return this->PutVersioned(key, val);
}

inline std::vector<std::string> ConfigManager::GetShardReplicas(uint32_t shard_id) {
    std::string value = std::string("");
    if (!cm_kv_absent(((*this)).kv)) {
        const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/replicas");
        // @unsafe
        {
            ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key, &value);
        }
    }
    // @unsafe
    {
        return cm_split_csv(value);
    }
}

inline bool ConfigManager::SetShardReplicas(uint32_t shard_id, const std::vector<std::string>& replicas) {
    const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/replicas");
    const std::string val = this->JoinReplicas(replicas);
    return this->PutVersioned(key, val);
}

inline std::string ConfigManager::GetShardLeader(uint32_t shard_id) {
    if (cm_kv_absent(((*this)).kv)) {
        return std::string("");
    }
    const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/leader");
    std::string value = std::string("");
    if (((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key, &value)) {
        return std::move(value);
    }
    return std::string("");
}

inline bool ConfigManager::SetShardLeader(uint32_t shard_id, const std::string& leader) {
    const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/leader");
    return this->PutVersioned(key, leader);
}

inline std::string ConfigManager::GetShardStatus(uint32_t shard_id) {
    if (cm_kv_absent(((*this)).kv)) {
        return std::string("");
    }
    const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/status");
    std::string value = std::string("");
    if (((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key, &value)) {
        return std::move(value);
    }
    return std::string("");
}

inline bool ConfigManager::SetShardStatus(uint32_t shard_id, const std::string& status) {
    const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/status");
    return this->PutVersioned(key, status);
}

inline bool ConfigManager::AddShard(uint32_t shard_id, const std::vector<std::string>& replicas) {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    const uint32_t count = this->GetShardCount();
    const std::string rkey = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/replicas");
    const std::string rval = this->JoinReplicas(replicas);
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(rkey, rval);
    }
    const std::string skey = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/status");
    const std::string sval = std::string("active");
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(skey, sval);
    }
    const std::string ckey = std::string("shard_count");
    const std::string cval = std::to_string(rusty::detail::deref_if_pointer_like(count) + 1);
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(ckey, cval);
    }
    this->BumpVersion();
    return true;
}

inline bool ConfigManager::RemoveShard(uint32_t shard_id) {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    const uint32_t count = this->GetShardCount();
    if (rusty::detail::deref_if_pointer_like(count) == static_cast<uint32_t>(0)) {
        return false;
    }
    const std::string rkey = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/replicas");
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).remove(rkey);
    }
    const std::string lkey = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/leader");
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).remove(lkey);
    }
    const std::string skey = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/status");
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).remove(skey);
    }
    const std::string ckey = std::string("shard_count");
    const std::string cval = std::to_string(rusty::detail::deref_if_pointer_like(count) - 1);
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(ckey, cval);
    }
    this->BumpVersion();
    return true;
}

inline uint32_t ConfigManager::GetShardReplacement(uint32_t shard_id) {
    if (cm_kv_absent(((*this)).kv)) {
        return static_cast<uint32_t>(0);
    }
    const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/replacement");
    std::string value = std::string("");
    if (((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key, &value)) {
        return static_cast<uint32_t>(cm_parse_u64(value));
    }
    return static_cast<uint32_t>(0);
}

inline bool ConfigManager::KillShard(uint32_t dead_id, uint32_t taker_id) {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    if (rusty::detail::deref_if_pointer_like(dead_id) == rusty::detail::deref_if_pointer_like(taker_id)) {
        return false;
    }
    if (this->GetShardStatus(std::move(dead_id)).empty()) {
        return false;
    }
    if (this->GetShardReplicas(std::move(taker_id)).empty()) {
        return false;
    }
    const uint64_t epoch = this->GetEpoch() + static_cast<uint64_t>(1);
    const std::string dkey = (std::string("shard/") + std::to_string(std::move(dead_id))) + std::string("/status");
    const std::string dval = std::string("dead");
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(dkey, dval);
    }
    const std::string pkey = (std::string("shard/") + std::to_string(std::move(dead_id))) + std::string("/replacement");
    const std::string pval = std::to_string(std::move(taker_id));
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(pkey, pval);
    }
    const std::string rkey = (std::string("shard/") + std::to_string(std::move(dead_id))) + std::string("/replicas");
    const std::string empty = std::string("");
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(rkey, empty);
    }
    const std::string ekey = std::string("epoch");
    const std::string eval = std::to_string(std::move(epoch));
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(ekey, eval);
    }
    this->BumpVersion();
    return true;
}

inline uint64_t ConfigManager::GetEpoch() {
    if (cm_kv_absent(((*this)).kv)) {
        return static_cast<uint64_t>(0);
    }
    const std::string key = std::string("epoch");
    std::string value = std::string("");
    if (((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key, &value)) {
        return cm_parse_u64(value);
    }
    return static_cast<uint64_t>(0);
}

inline bool ConfigManager::AdvanceEpoch() {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    const uint64_t epoch = this->GetEpoch() + static_cast<uint64_t>(1);
    const std::string key = std::string("epoch");
    const std::string val = std::to_string(std::move(epoch));
    return this->PutVersioned(key, val);
}

inline std::string ConfigManager::GetNodeAddr(const std::string& site) {
    if (cm_kv_absent(((*this)).kv)) {
        return std::string("");
    }
    const std::string key = ((std::string("node/") + site)) + std::string("/addr");
    std::string value = std::string("");
    if (((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key, &value)) {
        return std::move(value);
    }
    return std::string("");
}

inline bool ConfigManager::SetNodeAddr(const std::string& site, const std::string& addr) {
    const std::string key = ((std::string("node/") + site)) + std::string("/addr");
    return this->PutVersioned(key, addr);
}

inline std::string ConfigManager::GetNodeStatus(const std::string& site) {
    if (cm_kv_absent(((*this)).kv)) {
        return std::string("");
    }
    const std::string key = ((std::string("node/") + site)) + std::string("/status");
    std::string value = std::string("");
    if (((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key, &value)) {
        return std::move(value);
    }
    return std::string("");
}

inline bool ConfigManager::SetNodeStatus(const std::string& site, const std::string& status) {
    const std::string key = ((std::string("node/") + site)) + std::string("/status");
    return this->PutVersioned(key, status);
}

inline std::string ConfigManager::GetShardingMode() {
    if (cm_kv_absent(((*this)).kv)) {
        return std::string("");
    }
    const std::string key = std::string("sharding/mode");
    std::string value = std::string("");
    if (((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key, &value)) {
        return std::move(value);
    }
    return std::string("");
}

inline bool ConfigManager::SetShardingMode(const std::string& mode) {
    const std::string key = std::string("sharding/mode");
    return this->PutVersioned(key, mode);
}

inline std::string ConfigManager::GetShardingPolicy(const std::string& table) {
    if (cm_kv_absent(((*this)).kv)) {
        return std::string("");
    }
    if (((table)).empty()) {
        return std::string("");
    }
    const std::string key = std::string("sharding/policy/") + table;
    std::string value = std::string("");
    if (((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key, &value)) {
        return std::move(value);
    }
    return std::string("");
}

inline std::vector<std::string> ConfigManager::ListShardingPolicyTables() {
    std::string value = std::string("");
    if (!cm_kv_absent(((*this)).kv)) {
        const std::string key = std::string("sharding/policy_tables");
        // @unsafe
        {
            ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key, &value);
        }
    }
    // @unsafe
    {
        return cm_split_csv(value);
    }
}

inline bool ConfigManager::SetShardingPolicy(const std::string& table, const std::string& serialized_policy) {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    if (((table)).empty()) {
        return false;
    }
    if (((serialized_policy)).empty()) {
        return false;
    }
    std::vector<std::string> tables = this->ListShardingPolicyTables();
    bool present = false;
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < tables.size()) {
        if (tables[i] == table) {
            present = true;
        }
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    const std::string pkey = std::string("sharding/policy/") + table;
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(pkey, serialized_policy);
    }
    if (!present) {
        tables.push_back(table);
        const std::string tkey = std::string("sharding/policy_tables");
        const std::string tval = this->JoinReplicas(tables);
        // @unsafe
        {
            ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(tkey, tval);
        }
    }
    this->BumpVersion();
    return true;
}

inline bool ConfigManager::DeleteShardingPolicy(const std::string& table) {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    if (((table)).empty()) {
        return false;
    }
    const std::vector<std::string> tables = this->ListShardingPolicyTables();
    std::vector<std::string> pruned = cm_split_csv(std::string(""));
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < tables.size()) {
        if (tables[i] != table) {
            pruned.push_back(tables[i]);
        }
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    if ((pruned.size() == tables.size()) && this->GetShardingPolicy(table).empty()) {
        return true;
    }
    const std::string pkey = std::string("sharding/policy/") + table;
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).remove(pkey);
    }
    const std::string tkey = std::string("sharding/policy_tables");
    const std::string tval = this->JoinReplicas(pruned);
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(tkey, tval);
    }
    this->BumpVersion();
    return true;
}
/*RUSTYCPP:GEN-END id=config_manager.1*/
/*RUSTYCPP:GEN-BEGIN*/
/*RUSTYCPP:GEN-END*/

}  // namespace janus
