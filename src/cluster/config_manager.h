module;
#include <string>
#include <vector>
#include <cstdint>
#include <charconv>

// deref_if_pointer_like (raw-pointer field method calls) used by the
// DSL-generated bodies below. rusty, not srpc.
#include <rusty/slice.hpp>

export module cluster:config_manager;
import :kv_store;

export namespace janus {

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
 * with the __version__ key written LAST (bump_version, called after the
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
    fn join_replicas(&self, replicas: &std::vector<std::string>) -> std::string {
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
    fn put_versioned(&mut self, key: &std::string, value: &std::string) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        unsafe { (*(*self).kv).put(key, value); }
        self.bump_version();
        true
    }
    // Read __version__, write it back incremented (the last visible write).
    fn bump_version(&mut self) {
        let vkey: std::string = std::string("__version__");
        let mut cur: u64 = 0;
        let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&vkey) };
        if vopt.is_some() {
            let vs: std::string = vopt.unwrap();
            cur = unsafe { cm_parse_u64(&vs) };
        }
        let nv: std::string = std::to_string(cur + 1);
        unsafe { (*(*self).kv).put(&vkey, &nv); }
    }

    // ---- version ---------------------------------------------------------
    fn get_version(&mut self) -> u64 {
        if unsafe { cm_kv_absent((*self).kv) } { return 0; }
        let vkey: std::string = std::string("__version__");
        let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&vkey) };
        if vopt.is_some() {
            let value: std::string = vopt.unwrap();
            return unsafe { cm_parse_u64(&value) };
        }
        0
    }

    // ---- shard management ------------------------------------------------
    fn get_shard_count(&mut self) -> u32 {
        if unsafe { cm_kv_absent((*self).kv) } { return 0; }
        let key: std::string = std::string("shard_count");
        let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&key) };
        if vopt.is_some() {
            let value: std::string = vopt.unwrap();
            return unsafe { cm_parse_u64(&value) } as u32;
        }
        0
    }
    fn set_shard_count(&mut self, count: u32) -> bool {
        let key: std::string = std::string("shard_count");
        let val: std::string = std::to_string(count);
        self.put_versioned(&key, &val)
    }
    fn get_shard_replicas(&mut self, shard_id: u32) -> std::vector<std::string> {
        let mut value: std::string = std::string("");
        if !unsafe { cm_kv_absent((*self).kv) } {
            let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/replicas");
            let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&key) };
            if vopt.is_some() { value = vopt.unwrap(); }
        }
        unsafe { cm_split_csv(&value) }
    }
    fn set_shard_replicas(&mut self, shard_id: u32, replicas: &std::vector<std::string>) -> bool {
        let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/replicas");
        let val: std::string = self.join_replicas(replicas);
        self.put_versioned(&key, &val)
    }
    fn get_shard_leader(&mut self, shard_id: u32) -> std::string {
        if unsafe { cm_kv_absent((*self).kv) } { return std::string(""); }
        let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/leader");
        let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&key) };
        vopt.unwrap_or(std::string(""))
    }
    fn set_shard_leader(&mut self, shard_id: u32, leader: &std::string) -> bool {
        let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/leader");
        self.put_versioned(&key, leader)
    }
    fn get_shard_status(&mut self, shard_id: u32) -> std::string {
        if unsafe { cm_kv_absent((*self).kv) } { return std::string(""); }
        let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/status");
        let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&key) };
        vopt.unwrap_or(std::string(""))
    }
    fn set_shard_status(&mut self, shard_id: u32, status: &std::string) -> bool {
        let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/status");
        self.put_versioned(&key, status)
    }

    // ---- shard lifecycle -------------------------------------------------
    fn add_shard(&mut self, shard_id: u32, replicas: &std::vector<std::string>) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        let count: u32 = self.get_shard_count();
        let rkey: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/replicas");
        let rval: std::string = self.join_replicas(replicas);
        unsafe { (*(*self).kv).put(&rkey, &rval); }
        let skey: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/status");
        let sval: std::string = std::string("active");
        unsafe { (*(*self).kv).put(&skey, &sval); }
        let ckey: std::string = std::string("shard_count");
        let cval: std::string = std::to_string(count + 1);
        unsafe { (*(*self).kv).put(&ckey, &cval); }
        self.bump_version();
        true
    }
    // The next shard id the master will hand out. Monotonic and never reused,
    // so a removed shard's id is not recycled (a fresh shard is not confused
    // with a just-departed one).
    fn next_shard_id(&mut self) -> u32 {
        if unsafe { cm_kv_absent((*self).kv) } { return 0; }
        let key: std::string = std::string("next_shard_id");
        let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&key) };
        if vopt.is_some() {
            let value: std::string = vopt.unwrap();
            return unsafe { cm_parse_u64(&value) } as u32;
        }
        0
    }
    // Register a new shard: the MASTER allocates its id. An empty shard starts
    // with no id, calls this with its replica set, and adopts the returned id.
    // Records replicas + active status, advances the id allocator, and bumps
    // shard_count (via add_shard, which writes __version__ last).
    fn register_shard(&mut self, replicas: &std::vector<std::string>) -> u32 {
        if unsafe { cm_kv_absent((*self).kv) } { return 0; }
        let id: u32 = self.next_shard_id();
        let nkey: std::string = std::string("next_shard_id");
        let nval: std::string = std::to_string(id + 1);
        unsafe { (*(*self).kv).put(&nkey, &nval); }
        self.add_shard(id, replicas);
        id
    }
    fn remove_shard(&mut self, shard_id: u32) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        let count: u32 = self.get_shard_count();
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
        self.bump_version();
        true
    }
    fn get_shard_replacement(&mut self, shard_id: u32) -> u32 {
        if unsafe { cm_kv_absent((*self).kv) } { return 0; }
        let key: std::string = std::string("shard/") + std::to_string(shard_id) + std::string("/replacement");
        let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&key) };
        if vopt.is_some() {
            let value: std::string = vopt.unwrap();
            return unsafe { cm_parse_u64(&value) } as u32;
        }
        0
    }
    fn kill_shard(&mut self, dead_id: u32, taker_id: u32) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        if dead_id == taker_id { return false; }
        if self.get_shard_status(dead_id).empty() { return false; }
        if self.get_shard_replicas(taker_id).empty() { return false; }
        let epoch: u64 = self.get_epoch() + 1;
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
        self.bump_version();
        true
    }

    // ---- epoch -----------------------------------------------------------
    fn get_epoch(&mut self) -> u64 {
        if unsafe { cm_kv_absent((*self).kv) } { return 0; }
        let key: std::string = std::string("epoch");
        let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&key) };
        if vopt.is_some() {
            let value: std::string = vopt.unwrap();
            return unsafe { cm_parse_u64(&value) };
        }
        0
    }
    fn advance_epoch(&mut self) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        let epoch: u64 = self.get_epoch() + 1;
        let key: std::string = std::string("epoch");
        let val: std::string = std::to_string(epoch);
        self.put_versioned(&key, &val)
    }

    // ---- node management -------------------------------------------------
    fn get_node_addr(&mut self, site: &std::string) -> std::string {
        if unsafe { cm_kv_absent((*self).kv) } { return std::string(""); }
        let key: std::string = (std::string("node/") + *site) + std::string("/addr");
        let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&key) };
        vopt.unwrap_or(std::string(""))
    }
    fn set_node_addr(&mut self, site: &std::string, addr: &std::string) -> bool {
        let key: std::string = (std::string("node/") + *site) + std::string("/addr");
        self.put_versioned(&key, addr)
    }
    fn get_node_status(&mut self, site: &std::string) -> std::string {
        if unsafe { cm_kv_absent((*self).kv) } { return std::string(""); }
        let key: std::string = (std::string("node/") + *site) + std::string("/status");
        let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&key) };
        vopt.unwrap_or(std::string(""))
    }
    fn set_node_status(&mut self, site: &std::string, status: &std::string) -> bool {
        let key: std::string = (std::string("node/") + *site) + std::string("/status");
        self.put_versioned(&key, status)
    }

    // ---- sharding policy (opaque bytes) ----------------------------------
    fn get_sharding_mode(&mut self) -> std::string {
        if unsafe { cm_kv_absent((*self).kv) } { return std::string(""); }
        let key: std::string = std::string("sharding/mode");
        let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&key) };
        vopt.unwrap_or(std::string(""))
    }
    fn set_sharding_mode(&mut self, mode: &std::string) -> bool {
        let key: std::string = std::string("sharding/mode");
        self.put_versioned(&key, mode)
    }
    fn get_sharding_policy(&mut self, table: &std::string) -> std::string {
        if unsafe { cm_kv_absent((*self).kv) } { return std::string(""); }
        if (*table).empty() { return std::string(""); }
        let key: std::string = std::string("sharding/policy/") + *table;
        let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&key) };
        vopt.unwrap_or(std::string(""))
    }
    fn list_sharding_policy_tables(&mut self) -> std::vector<std::string> {
        let mut value: std::string = std::string("");
        if !unsafe { cm_kv_absent((*self).kv) } {
            let key: std::string = std::string("sharding/policy_tables");
            let vopt: rusty::Option<std::string> = unsafe { (*(*self).kv).get(&key) };
            if vopt.is_some() { value = vopt.unwrap(); }
        }
        unsafe { cm_split_csv(&value) }
    }
    fn set_sharding_policy(&mut self, table: &std::string, serialized_policy: &std::string) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        if (*table).empty() { return false; }
        if (*serialized_policy).empty() { return false; }
        let mut tables: std::vector<std::string> = self.list_sharding_policy_tables();
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
            let tval: std::string = self.join_replicas(&tables);
            unsafe { (*(*self).kv).put(&tkey, &tval); }
        }
        self.bump_version();
        true
    }
    fn delete_sharding_policy(&mut self, table: &std::string) -> bool {
        if unsafe { cm_kv_absent((*self).kv) } { return false; }
        if (*table).empty() { return false; }
        let tables: std::vector<std::string> = self.list_sharding_policy_tables();
        let mut pruned: std::vector<std::string> = cm_split_csv(&std::string(""));
        let mut i: usize = 0;
        while i < tables.size() {
            if tables[i] != *table { pruned.push_back(tables[i]); }
            i = i + 1;
        }
        if pruned.size() == tables.size() && self.get_sharding_policy(table).empty() {
            return true;
        }
        let pkey: std::string = std::string("sharding/policy/") + *table;
        unsafe { (*(*self).kv).remove(&pkey); }
        let tkey: std::string = std::string("sharding/policy_tables");
        let tval: std::string = self.join_replicas(&pruned);
        unsafe { (*(*self).kv).put(&tkey, &tval); }
        self.bump_version();
        true
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=config_manager.1 version=1 rust_sha256=11b2ff6cf88966c14f5e052f80aab7443884da2482b848ff7efe970938c6d3a2*/
struct ConfigManager;

struct ConfigManager {
    KvStore* kv;

    std::string join_replicas(const std::vector<std::string>& replicas) const;
    bool put_versioned(const std::string& key, const std::string& value);
    void bump_version();
    uint64_t get_version();
    uint32_t get_shard_count();
    bool set_shard_count(uint32_t count);
    std::vector<std::string> get_shard_replicas(uint32_t shard_id);
    bool set_shard_replicas(uint32_t shard_id, const std::vector<std::string>& replicas);
    std::string get_shard_leader(uint32_t shard_id);
    bool set_shard_leader(uint32_t shard_id, const std::string& leader);
    std::string get_shard_status(uint32_t shard_id);
    bool set_shard_status(uint32_t shard_id, const std::string& status);
    bool add_shard(uint32_t shard_id, const std::vector<std::string>& replicas);
    uint32_t next_shard_id();
    uint32_t register_shard(const std::vector<std::string>& replicas);
    bool remove_shard(uint32_t shard_id);
    uint32_t get_shard_replacement(uint32_t shard_id);
    bool kill_shard(uint32_t dead_id, uint32_t taker_id);
    uint64_t get_epoch();
    bool advance_epoch();
    std::string get_node_addr(const std::string& site);
    bool set_node_addr(const std::string& site, const std::string& addr);
    std::string get_node_status(const std::string& site);
    bool set_node_status(const std::string& site, const std::string& status);
    std::string get_sharding_mode();
    bool set_sharding_mode(const std::string& mode);
    std::string get_sharding_policy(const std::string& table);
    std::vector<std::string> list_sharding_policy_tables();
    bool set_sharding_policy(const std::string& table, const std::string& serialized_policy);
    bool delete_sharding_policy(const std::string& table);
};


inline std::string ConfigManager::join_replicas(const std::vector<std::string>& replicas) const {
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

inline bool ConfigManager::put_versioned(const std::string& key, const std::string& value) {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(key, value);
    }
    this->bump_version();
    return true;
}

inline void ConfigManager::bump_version() {
    const std::string vkey = std::string("__version__");
    uint64_t cur = static_cast<uint64_t>(0);
    rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(vkey);
    if (vopt.is_some()) {
        const std::string vs = vopt.unwrap();
        cur = cm_parse_u64(vs);
    }
    const std::string nv = std::to_string(rusty::detail::deref_if_pointer_like(cur) + 1);
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(vkey, nv);
    }
}

inline uint64_t ConfigManager::get_version() {
    if (cm_kv_absent(((*this)).kv)) {
        return static_cast<uint64_t>(0);
    }
    const std::string vkey = std::string("__version__");
    rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(vkey);
    if (vopt.is_some()) {
        const std::string value = vopt.unwrap();
        return cm_parse_u64(value);
    }
    return static_cast<uint64_t>(0);
}

inline uint32_t ConfigManager::get_shard_count() {
    if (cm_kv_absent(((*this)).kv)) {
        return static_cast<uint32_t>(0);
    }
    const std::string key = std::string("shard_count");
    rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key);
    if (vopt.is_some()) {
        const std::string value = vopt.unwrap();
        return static_cast<uint32_t>(cm_parse_u64(value));
    }
    return static_cast<uint32_t>(0);
}

inline bool ConfigManager::set_shard_count(uint32_t count) {
    const std::string key = std::string("shard_count");
    const std::string val = std::to_string(std::move(count));
    return this->put_versioned(key, val);
}

inline std::vector<std::string> ConfigManager::get_shard_replicas(uint32_t shard_id) {
    std::string value = std::string("");
    if (!cm_kv_absent(((*this)).kv)) {
        const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/replicas");
        rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key);
        if (vopt.is_some()) {
            value = vopt.unwrap();
        }
    }
    // @unsafe
    {
        return cm_split_csv(value);
    }
}

inline bool ConfigManager::set_shard_replicas(uint32_t shard_id, const std::vector<std::string>& replicas) {
    const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/replicas");
    const std::string val = this->join_replicas(replicas);
    return this->put_versioned(key, val);
}

inline std::string ConfigManager::get_shard_leader(uint32_t shard_id) {
    if (cm_kv_absent(((*this)).kv)) {
        return std::string("");
    }
    const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/leader");
    const rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key);
    return vopt.unwrap_or(std::string(""));
}

inline bool ConfigManager::set_shard_leader(uint32_t shard_id, const std::string& leader) {
    const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/leader");
    return this->put_versioned(key, leader);
}

inline std::string ConfigManager::get_shard_status(uint32_t shard_id) {
    if (cm_kv_absent(((*this)).kv)) {
        return std::string("");
    }
    const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/status");
    const rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key);
    return vopt.unwrap_or(std::string(""));
}

inline bool ConfigManager::set_shard_status(uint32_t shard_id, const std::string& status) {
    const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/status");
    return this->put_versioned(key, status);
}

inline bool ConfigManager::add_shard(uint32_t shard_id, const std::vector<std::string>& replicas) {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    const uint32_t count = this->get_shard_count();
    const std::string rkey = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/replicas");
    const std::string rval = this->join_replicas(replicas);
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
    this->bump_version();
    return true;
}

inline uint32_t ConfigManager::next_shard_id() {
    if (cm_kv_absent(((*this)).kv)) {
        return static_cast<uint32_t>(0);
    }
    const std::string key = std::string("next_shard_id");
    rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key);
    if (vopt.is_some()) {
        const std::string value = vopt.unwrap();
        return static_cast<uint32_t>(cm_parse_u64(value));
    }
    return static_cast<uint32_t>(0);
}

inline uint32_t ConfigManager::register_shard(const std::vector<std::string>& replicas) {
    if (cm_kv_absent(((*this)).kv)) {
        return static_cast<uint32_t>(0);
    }
    uint32_t id = this->next_shard_id();
    const std::string nkey = std::string("next_shard_id");
    const std::string nval = std::to_string(rusty::detail::deref_if_pointer_like(id) + 1);
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(nkey, nval);
    }
    this->add_shard(std::move(id), replicas);
    return std::move(id);
}

inline bool ConfigManager::remove_shard(uint32_t shard_id) {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    const uint32_t count = this->get_shard_count();
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
    this->bump_version();
    return true;
}

inline uint32_t ConfigManager::get_shard_replacement(uint32_t shard_id) {
    if (cm_kv_absent(((*this)).kv)) {
        return static_cast<uint32_t>(0);
    }
    const std::string key = (std::string("shard/") + std::to_string(std::move(shard_id))) + std::string("/replacement");
    rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key);
    if (vopt.is_some()) {
        const std::string value = vopt.unwrap();
        return static_cast<uint32_t>(cm_parse_u64(value));
    }
    return static_cast<uint32_t>(0);
}

inline bool ConfigManager::kill_shard(uint32_t dead_id, uint32_t taker_id) {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    if (rusty::detail::deref_if_pointer_like(dead_id) == rusty::detail::deref_if_pointer_like(taker_id)) {
        return false;
    }
    if (this->get_shard_status(std::move(dead_id)).empty()) {
        return false;
    }
    if (this->get_shard_replicas(std::move(taker_id)).empty()) {
        return false;
    }
    const uint64_t epoch = this->get_epoch() + static_cast<uint64_t>(1);
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
    this->bump_version();
    return true;
}

inline uint64_t ConfigManager::get_epoch() {
    if (cm_kv_absent(((*this)).kv)) {
        return static_cast<uint64_t>(0);
    }
    const std::string key = std::string("epoch");
    rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key);
    if (vopt.is_some()) {
        const std::string value = vopt.unwrap();
        return cm_parse_u64(value);
    }
    return static_cast<uint64_t>(0);
}

inline bool ConfigManager::advance_epoch() {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    const uint64_t epoch = this->get_epoch() + static_cast<uint64_t>(1);
    const std::string key = std::string("epoch");
    const std::string val = std::to_string(std::move(epoch));
    return this->put_versioned(key, val);
}

inline std::string ConfigManager::get_node_addr(const std::string& site) {
    if (cm_kv_absent(((*this)).kv)) {
        return std::string("");
    }
    const std::string key = ((std::string("node/") + site)) + std::string("/addr");
    const rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key);
    return vopt.unwrap_or(std::string(""));
}

inline bool ConfigManager::set_node_addr(const std::string& site, const std::string& addr) {
    const std::string key = ((std::string("node/") + site)) + std::string("/addr");
    return this->put_versioned(key, addr);
}

inline std::string ConfigManager::get_node_status(const std::string& site) {
    if (cm_kv_absent(((*this)).kv)) {
        return std::string("");
    }
    const std::string key = ((std::string("node/") + site)) + std::string("/status");
    const rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key);
    return vopt.unwrap_or(std::string(""));
}

inline bool ConfigManager::set_node_status(const std::string& site, const std::string& status) {
    const std::string key = ((std::string("node/") + site)) + std::string("/status");
    return this->put_versioned(key, status);
}

inline std::string ConfigManager::get_sharding_mode() {
    if (cm_kv_absent(((*this)).kv)) {
        return std::string("");
    }
    const std::string key = std::string("sharding/mode");
    const rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key);
    return vopt.unwrap_or(std::string(""));
}

inline bool ConfigManager::set_sharding_mode(const std::string& mode) {
    const std::string key = std::string("sharding/mode");
    return this->put_versioned(key, mode);
}

inline std::string ConfigManager::get_sharding_policy(const std::string& table) {
    if (cm_kv_absent(((*this)).kv)) {
        return std::string("");
    }
    if (((table)).empty()) {
        return std::string("");
    }
    const std::string key = std::string("sharding/policy/") + table;
    const rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key);
    return vopt.unwrap_or(std::string(""));
}

inline std::vector<std::string> ConfigManager::list_sharding_policy_tables() {
    std::string value = std::string("");
    if (!cm_kv_absent(((*this)).kv)) {
        const std::string key = std::string("sharding/policy_tables");
        rusty::Option<std::string> vopt = ((rusty::detail::deref_if_pointer_like(((*this)).kv))).get(key);
        if (vopt.is_some()) {
            value = vopt.unwrap();
        }
    }
    // @unsafe
    {
        return cm_split_csv(value);
    }
}

inline bool ConfigManager::set_sharding_policy(const std::string& table, const std::string& serialized_policy) {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    if (((table)).empty()) {
        return false;
    }
    if (((serialized_policy)).empty()) {
        return false;
    }
    std::vector<std::string> tables = this->list_sharding_policy_tables();
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
        const std::string tval = this->join_replicas(tables);
        // @unsafe
        {
            ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(tkey, tval);
        }
    }
    this->bump_version();
    return true;
}

inline bool ConfigManager::delete_sharding_policy(const std::string& table) {
    if (cm_kv_absent(((*this)).kv)) {
        return false;
    }
    if (((table)).empty()) {
        return false;
    }
    const std::vector<std::string> tables = this->list_sharding_policy_tables();
    std::vector<std::string> pruned = cm_split_csv(std::string(""));
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < tables.size()) {
        if (tables[i] != table) {
            pruned.push_back(tables[i]);
        }
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    if ((pruned.size() == tables.size()) && this->get_sharding_policy(table).empty()) {
        return true;
    }
    const std::string pkey = std::string("sharding/policy/") + table;
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).remove(pkey);
    }
    const std::string tkey = std::string("sharding/policy_tables");
    const std::string tval = this->join_replicas(pruned);
    // @unsafe
    {
        ((rusty::detail::deref_if_pointer_like(((*this)).kv))).put(tkey, tval);
    }
    this->bump_version();
    return true;
}
/*RUSTYCPP:GEN-END id=config_manager.1*/
/*RUSTYCPP:GEN-BEGIN*/
/*RUSTYCPP:GEN-END*/

}  // namespace janus
