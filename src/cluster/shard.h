module;

// Shard — a stub stand-in for a masstree-backed data shard, used to test the
// cluster reconfiguration flow (ShardManager driving ConfigManager /
// ClusterConfig) end-to-end WITHOUT the storage engine. It is just an
// in-memory key/value set plus a liveness flag; on kill_shard the manager
// migrates one shard's data into another the way a real range hand-off
// eventually will. Swap this for a masstree-backed shard to run the same
// manager against real storage.
//
// Authored in the inline-Rust DSL (docs/storage-interface.md): the
// `#if RUSTYCPP_RUST` block is the source of truth; regenerate with
// scripts/regen_storage_dsl.sh. Plain pub struct + inherent impl -> a
// copyable aggregate, so a Shard lives in the ShardManager's
// btree_port::BTreeMap<u32, Shard> by value.

#include <string>
#include <rusty/vec.hpp>             // drop_range's collect-then-remove buffer
#include <rusty/array.hpp>           // c529cd3d: BTreeMap::len() free-fn decl
#include <rusty/option.hpp>          // get() -> Option<std::string>
#include <rusty/slice.hpp>           // deref_if_pointer_like (generated bodies)

export module cluster:shard;
import btree_port.btree.map;
import rusty;                  // c529cd3d: rusty::Vec is a module now   // c529cd3d: btree_port is now a C++20 module (retired the .hpp header)

namespace btree_port { using btree::map::BTreeMap; }  // compat: flat name the DSL/GEN expect

export namespace janus {

#if RUSTYCPP_RUST
// A key range [lo, hi) a shard is in charge of.
pub struct ShardRange {
    lo: std::string,
    hi: std::string,
}
impl ShardRange {
    fn make(lo: std::string, hi: std::string) -> ShardRange {
        ShardRange { lo: lo, hi: hi }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=shard.1 version=1 rust_sha256=97ebf5fbfb28f7e77e59a82101064b92a4adcef2a8206f6e3f35bf02488d8173*/
struct ShardRange;

struct ShardRange {
    std::string lo;
    std::string hi;

    static ShardRange make(std::string lo, std::string hi);
};


ShardRange ShardRange::make(std::string lo, std::string hi) {
    return ShardRange{.lo = std::move(lo), .hi = std::move(hi)};
}
/*RUSTYCPP:GEN-END id=shard.1*/

#if RUSTYCPP_RUST
pub struct Shard {
    shard_id: u32,   // not `id`: a field named the same as the id() method clashes
    alive: bool,
    data: btree_port::BTreeMap<std::string, std::string>,      // live key -> value
    tombstones: btree_port::BTreeMap<std::string, bool>,       // deleted keys (a "null write")
    // ---- participant-side metadata: what this shard is in charge of, and its
    // local view of any in-flight migration for one of its ranges ----
    owned: rusty::Vec<ShardRange>,   // ranges this shard is responsible for
    mig_active: bool,                // participating in a migration?
    mig_lo: std::string,             // the migrating range [lo, hi)
    mig_hi: std::string,
    mig_is_source: bool,             // true = shedding it (source), false = receiving it (dest)
    mig_locked: bool,                // source side: the range is frozen (2PC prepared)
    mig_gen: u64,                    // the migration attempt id
}
impl Shard {
    // A fresh, live shard: no data, no owned ranges, not migrating.
    fn new(id: u32) -> Shard {
        Shard {
            shard_id: id,
            alive: true,
            data: btree_port::BTreeMap::<std::string, std::string>::new_(),
            tombstones: btree_port::BTreeMap::<std::string, bool>::new_(),
            owned: rusty::Vec::<ShardRange>::new_(),
            mig_active: false,
            mig_lo: std::string(""),
            mig_hi: std::string(""),
            mig_is_source: false,
            mig_locked: false,
            mig_gen: 0,
        }
    }
    fn id(&self) -> u32 { (*self).shard_id }
    fn is_alive(&self) -> bool { (*self).alive }
    fn mark_dead(&mut self) { (*self).alive = false; }
    fn key_count(&self) -> usize { (*self).data.len() }
    fn contains(&self, key: &std::string) -> bool {
        (*self).data.contains_key(key)
    }
    // Store a value (blind overwrite). A live write clears any prior tombstone.
    fn put(&mut self, key: &std::string, value: &std::string) {
        (*self).data.insert(key, value);
        (*self).tombstones.remove(key);
    }
    // Read a value, or None on miss (a tombstoned key is absent from `data`).
    fn get(&self, key: &std::string) -> rusty::Option<std::string> {
        let found = (*self).data.get(key);
        if found.is_none() { return rusty::None; }
        rusty::Some(std::string(found.unwrap()))
    }
    // Delete = a "null write": drop the live value and record a tombstone, so
    // the deletion is a positive fact that copies + checksums like any write.
    fn remove(&mut self, key: &std::string) {
        (*self).data.remove(key);
        (*self).tombstones.insert(key, true);
    }
    // Apply a migration write-delta (staged puts + staged deletes accumulated
    // during a background copy) to this shard. Hosted on Shard rather than
    // inlined into ShardManager::final_sync so the BTreeMap `for_in` codegen
    // lands in this (smaller) module partition: shard_manager's partition
    // trips a clang22 Itanium-mangler frontend crash on the rusty::iter
    // dispatcher lambda. See docs/dev/clang22-mangler-crash.md.
    fn apply_migration_delta(&mut self,
                             staged: &btree_port::BTreeMap<std::string, std::string>,
                             deleted: &btree_port::BTreeMap<std::string, bool>) {
        for kv in staged {
            self.put(kv.0, kv.1);
        }
        for dk in deleted {
            self.remove(dk.0);
        }
    }
    // True iff `key` is currently tombstoned (deleted) on this shard.
    fn is_tombstoned(&self, key: &std::string) -> bool {
        (*self).tombstones.contains_key(key)
    }
    // Take over another shard's entire dataset (data migration on kill_shard),
    // leaving `other` empty. Stand-in for a masstree range hand-off. `other`
    // is a raw pointer (not &mut Shard): the DSL lowers a `&mut local`
    // argument to a pointer, which only binds to a pointer parameter.
    fn absorb(&mut self, other: *mut Shard) {
        unsafe {
            for kv in (*other).data {
                (*self).data.insert(kv.0, kv.1);
            }
            (*other).data.clear();
        }
    }
    // ---- online migration primitives (lex range [lo, hi)) ----------------
    // Bulk-copy the source's keys in [lo, hi) into self, leaving the source
    // intact (it keeps serving during the background copy). `source` is a raw
    // pointer for the same reason absorb's `other` is.
    fn copy_range_from(&mut self, source: *mut Shard, lo: &std::string, hi: &std::string) {
        unsafe {
            for kv in (*source).data {
                if kv.0 >= (*lo) && kv.0 < (*hi) {
                    // COPY into locals (not a move of kv.*): the transpiler
                    // moves by-value insert args, and moving out of `source`
                    // would empty it -- but the source must stay intact to keep
                    // serving the range during the background copy.
                    let k: std::string = kv.0;
                    let v: std::string = kv.1;
                    (*self).data.insert(k, v);
                }
            }
            // Carry the source's tombstones too, so a deletion transmits (the
            // destination learns the key is gone, not just "not copied").
            for tk in (*source).tombstones {
                if tk.0 >= (*lo) && tk.0 < (*hi) {
                    let tkk: std::string = tk.0;
                    (*self).data.remove(tkk);
                    (*self).tombstones.insert(tkk, true);
                }
            }
        }
    }
    // Drop the keys in [lo, hi) -- live AND tombstoned (the source sheds the
    // range after COMMIT). Collect-then-remove: cannot mutate mid-iteration.
    fn drop_range(&mut self, lo: &std::string, hi: &std::string) {
        let mut victims: rusty::Vec<std::string> = rusty::Vec::<std::string>::new_();
        for kv in (*self).data {
            if kv.0 >= (*lo) && kv.0 < (*hi) {
                victims.push(kv.0);
            }
        }
        let mut i: usize = 0;
        while i < victims.size() {
            (*self).data.remove(victims[i]);
            i = i + 1;
        }
        let mut tvictims: rusty::Vec<std::string> = rusty::Vec::<std::string>::new_();
        for tk in (*self).tombstones {
            if tk.0 >= (*lo) && tk.0 < (*hi) {
                tvictims.push(tk.0);
            }
        }
        let mut j: usize = 0;
        while j < tvictims.size() {
            (*self).tombstones.remove(tvictims[j]);
            j = j + 1;
        }
    }
    // How many keys this shard holds in [lo, hi) (test observability).
    fn range_count(&self, lo: &std::string, hi: &std::string) -> usize {
        let mut n: usize = 0;
        for kv in (*self).data {
            if kv.0 >= (*lo) && kv.0 < (*hi) {
                n = n + 1;
            }
        }
        n
    }
    // FNV-1a-style 64-bit byte hash (a checksum building block). The mixing
    // constants stay under 2^63 so they are well-formed DSL integer literals.
    fn hash64(s: &std::string) -> u64 {
        let mut h: u64 = 1469598103934665603;
        let mut i: usize = 0;
        while i < (*s).size() {
            h = h ^ (((*s)[i] as u8) as u64);
            h = h * 1099511628211;
            i = i + 1;
        }
        h
    }
    // Order-independent checksum over the range's live entries AND tombstones.
    // Two shards agree iff they hold exactly the same key->value pairs and the
    // same set of deletions -- the migration's cutover verification.
    fn checksum(&self, lo: &std::string, hi: &std::string) -> u64 {
        let mut sum: u64 = 0;
        for kv in (*self).data {
            if kv.0 >= (*lo) && kv.0 < (*hi) {
                let kh: u64 = Shard::hash64(kv.0);
                let vh: u64 = Shard::hash64(kv.1);
                sum = sum + (kh * 1000003) + vh;
            }
        }
        for tk in (*self).tombstones {
            if tk.0 >= (*lo) && tk.0 < (*hi) {
                let th: u64 = Shard::hash64(tk.0);
                sum = sum + (th * 2000029) + 1;
            }
        }
        sum
    }
    // ---- ownership metadata -------------------------------------------------
    // The master tells the shard it is now in charge of [lo, hi).
    fn assign_range(&mut self, lo: &std::string, hi: &std::string) {
        let mut i: usize = 0;
        while i < (*self).owned.size() {
            if (*self).owned[i].lo == (*lo) && (*self).owned[i].hi == (*hi) { return; }
            i = i + 1;
        }
        (*self).owned.push(ShardRange::make((*lo), (*hi)));
    }
    // The master tells the shard it no longer owns [lo, hi).
    fn unassign_range(&mut self, lo: &std::string, hi: &std::string) {
        let mut kept: rusty::Vec<ShardRange> = rusty::Vec::<ShardRange>::new_();
        let mut i: usize = 0;
        while i < (*self).owned.size() {
            if !((*self).owned[i].lo == (*lo) && (*self).owned[i].hi == (*hi)) {
                kept.push(ShardRange::make((*self).owned[i].lo, (*self).owned[i].hi));
            }
            i = i + 1;
        }
        (*self).owned = kept;
    }
    // Is `key` inside one of this shard's owned ranges?
    fn owns(&self, key: &std::string) -> bool {
        let mut i: usize = 0;
        while i < (*self).owned.size() {
            if (*key) >= (*self).owned[i].lo && (*key) < (*self).owned[i].hi { return true; }
            i = i + 1;
        }
        false
    }
    fn owned_count(&self) -> usize { (*self).owned.size() }
    // ---- migration participant metadata ------------------------------------
    // The shard learns it is participating in a migration of [lo, hi): as the
    // source (shedding it) or the destination (receiving it), tagged with the
    // attempt's generation.
    fn set_migration(&mut self, lo: &std::string, hi: &std::string, is_source: bool, gen: u64) {
        (*self).mig_active = true;
        (*self).mig_lo = (*lo);
        (*self).mig_hi = (*hi);
        (*self).mig_is_source = is_source;
        (*self).mig_locked = false;
        (*self).mig_gen = gen;
    }
    // Source side of the 2PC prepare: freeze the migrating range.
    fn lock_migration(&mut self) {
        if (*self).mig_active { (*self).mig_locked = true; }
    }
    fn clear_migration(&mut self) {
        (*self).mig_active = false;
        (*self).mig_locked = false;
    }
    fn is_migrating(&self) -> bool { (*self).mig_active }
    fn migration_locked(&self) -> bool { (*self).mig_active && (*self).mig_locked }
    fn migration_is_source(&self) -> bool { (*self).mig_active && (*self).mig_is_source }
    fn migration_generation(&self) -> u64 { (*self).mig_gen }
    // Is `key` in this shard's frozen (locked) migrating range?
    fn frozen_for(&self, key: &std::string) -> bool {
        (*self).mig_active
            && (*self).mig_locked
            && (*key) >= (*self).mig_lo && (*key) < (*self).mig_hi
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=shard.2 version=1 rust_sha256=c0aecc22818e9b3f07044e24b6fb1b94b77fe798694b66e5231b5f9e6ee8ecad*/
struct Shard;

struct Shard {
    uint32_t shard_id;
    bool alive;
    btree_port::BTreeMap<std::string, std::string> data;
    btree_port::BTreeMap<std::string, bool> tombstones;
    rusty::Vec<ShardRange> owned;
    bool mig_active;
    std::string mig_lo;
    std::string mig_hi;
    bool mig_is_source;
    bool mig_locked;
    uint64_t mig_gen;

    static Shard new_(uint32_t id);
    uint32_t id() const;
    bool is_alive() const;
    void mark_dead();
    size_t key_count() const;
    bool contains(const std::string& key) const;
    void put(const std::string& key, const std::string& value);
    rusty::Option<std::string> get(const std::string& key) const;
    void remove(const std::string& key);
    void apply_migration_delta(const btree_port::BTreeMap<std::string, std::string>& staged, const btree_port::BTreeMap<std::string, bool>& deleted);
    bool is_tombstoned(const std::string& key) const;
    void absorb(Shard* other);
    void copy_range_from(Shard* source, const std::string& lo, const std::string& hi);
    void drop_range(const std::string& lo, const std::string& hi);
    size_t range_count(const std::string& lo, const std::string& hi) const;
    static uint64_t hash64(const std::string& s);
    uint64_t checksum(const std::string& lo, const std::string& hi) const;
    void assign_range(const std::string& lo, const std::string& hi);
    void unassign_range(const std::string& lo, const std::string& hi);
    bool owns(const std::string& key) const;
    size_t owned_count() const;
    void set_migration(const std::string& lo, const std::string& hi, bool is_source, uint64_t gen);
    void lock_migration();
    void clear_migration();
    bool is_migrating() const;
    bool migration_locked() const;
    bool migration_is_source() const;
    uint64_t migration_generation() const;
    bool frozen_for(const std::string& key) const;
};


Shard Shard::new_(uint32_t id) {
    return Shard{.shard_id = std::move(id), .alive = true, .data = btree_port::BTreeMap<std::string, std::string>::new_(), .tombstones = btree_port::BTreeMap<std::string, bool>::new_(), .owned = rusty::Vec<ShardRange>::new_(), .mig_active = false, .mig_lo = std::string(""), .mig_hi = std::string(""), .mig_is_source = false, .mig_locked = false, .mig_gen = static_cast<uint64_t>(0)};
}

uint32_t Shard::id() const {
    return ((*this)).shard_id;
}

bool Shard::is_alive() const {
    return ((*this)).alive;
}

void Shard::mark_dead() {
    ((*this)).alive = false;
}

size_t Shard::key_count() const {
    return rusty::len(((*this)).data);
}

bool Shard::contains(const std::string& key) const {
    return ((*this)).data.contains_key(key);
}

void Shard::put(const std::string& key, const std::string& value) {
    ((*this)).data.insert(key, std::move(value));
    ((*this)).tombstones.remove(key);
}

rusty::Option<std::string> Shard::get(const std::string& key) const {
    auto found = ((*this)).data.get(key);
    if (found.is_none()) {
        return rusty::None;
    }
    return rusty::Option<std::string>(std::string(found.unwrap()));
}

void Shard::remove(const std::string& key) {
    ((*this)).data.remove(key);
    ((*this)).tombstones.insert(key, true);
}

void Shard::apply_migration_delta(const btree_port::BTreeMap<std::string, std::string>& staged, const btree_port::BTreeMap<std::string, bool>& deleted) {
    for (auto&& kv : rusty::for_in(rusty::iter(staged))) {
        this->put(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv)), rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._1; }) return (std::forward<decltype(__t)>(__t)._1); else if constexpr (requires { std::get<1>(std::forward<decltype(__t)>(__t)); }) return std::get<1>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._1; }) return ((*std::forward<decltype(__t)>(__t))._1); else return std::get<1>(*std::forward<decltype(__t)>(__t)); })(kv)));
    }
    for (auto&& dk : rusty::for_in(rusty::iter(deleted))) {
        this->remove(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(dk)));
    }
}

bool Shard::is_tombstoned(const std::string& key) const {
    return ((*this)).tombstones.contains_key(key);
}

void Shard::absorb(Shard* other) {
    // @unsafe
    {
        for (auto&& kv : rusty::for_in((*other).data)) {
            ((*this)).data.insert(std::move(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv))), std::move(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._1; }) return (std::forward<decltype(__t)>(__t)._1); else if constexpr (requires { std::get<1>(std::forward<decltype(__t)>(__t)); }) return std::get<1>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._1; }) return ((*std::forward<decltype(__t)>(__t))._1); else return std::get<1>(*std::forward<decltype(__t)>(__t)); })(kv))));
        }
        (*other).data.clear();
    }
}

void Shard::copy_range_from(Shard* source, const std::string& lo, const std::string& hi) {
    // @unsafe
    {
        for (auto&& kv : rusty::for_in((*source).data)) {
            if ((rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv))) >= (lo)) && (rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv))) < (hi))) {
                const std::string k = rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv));
                std::string v = rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._1; }) return (std::forward<decltype(__t)>(__t)._1); else if constexpr (requires { std::get<1>(std::forward<decltype(__t)>(__t)); }) return std::get<1>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._1; }) return ((*std::forward<decltype(__t)>(__t))._1); else return std::get<1>(*std::forward<decltype(__t)>(__t)); })(kv));
                ((*this)).data.insert(std::move(k), std::move(v));
            }
        }
        for (auto&& tk : rusty::for_in((*source).tombstones)) {
            if ((rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(tk))) >= (lo)) && (rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(tk))) < (hi))) {
                const std::string tkk = rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(tk));
                ((*this)).data.remove(tkk);
                ((*this)).tombstones.insert(std::move(tkk), true);
            }
        }
    }
}

void Shard::drop_range(const std::string& lo, const std::string& hi) {
    rusty::Vec<std::string> victims = rusty::Vec<std::string>::new_();
    for (auto&& kv : rusty::for_in(((*this)).data)) {
        if ((rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv))) >= (lo)) && (rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv))) < (hi))) {
            victims.push(std::move(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv))));
        }
    }
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < victims.size()) {
        ((*this)).data.remove(victims[i]);
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    rusty::Vec<std::string> tvictims = rusty::Vec<std::string>::new_();
    for (auto&& tk : rusty::for_in(((*this)).tombstones)) {
        if ((rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(tk))) >= (lo)) && (rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(tk))) < (hi))) {
            tvictims.push(std::move(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(tk))));
        }
    }
    size_t j = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(j) < tvictims.size()) {
        ((*this)).tombstones.remove(tvictims[j]);
        j = rusty::detail::deref_if_pointer_like(j) + static_cast<size_t>(1);
    }
}

size_t Shard::range_count(const std::string& lo, const std::string& hi) const {
    size_t n = static_cast<size_t>(0);
    for (auto&& kv : rusty::for_in(((*this)).data)) {
        if ((rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv))) >= (lo)) && (rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv))) < (hi))) {
            n = rusty::detail::deref_if_pointer_like(n) + static_cast<size_t>(1);
        }
    }
    return std::move(n);
}

uint64_t Shard::hash64(const std::string& s) {
    uint64_t h = static_cast<uint64_t>(1469598103934665603);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((s)).size()) {
        h = rusty::detail::deref_if_pointer_like(h) ^ ((static_cast<uint64_t>((static_cast<uint8_t>((s)[i])))));
        h = rusty::detail::deref_if_pointer_like(h) * static_cast<uint64_t>(1099511628211);
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    return std::move(h);
}

uint64_t Shard::checksum(const std::string& lo, const std::string& hi) const {
    uint64_t sum = static_cast<uint64_t>(0);
    for (auto&& kv : rusty::for_in(((*this)).data)) {
        if ((rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv))) >= (lo)) && (rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv))) < (hi))) {
            const uint64_t kh = Shard::hash64(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv)));
            const uint64_t vh = Shard::hash64(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._1; }) return (std::forward<decltype(__t)>(__t)._1); else if constexpr (requires { std::get<1>(std::forward<decltype(__t)>(__t)); }) return std::get<1>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._1; }) return ((*std::forward<decltype(__t)>(__t))._1); else return std::get<1>(*std::forward<decltype(__t)>(__t)); })(kv)));
            sum = (rusty::detail::deref_if_pointer_like(sum) + ((rusty::detail::deref_if_pointer_like(kh) * static_cast<uint64_t>(1000003)))) + rusty::detail::deref_if_pointer_like(vh);
        }
    }
    for (auto&& tk : rusty::for_in(((*this)).tombstones)) {
        if ((rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(tk))) >= (lo)) && (rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(tk))) < (hi))) {
            const uint64_t th = Shard::hash64(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(tk)));
            sum = (rusty::detail::deref_if_pointer_like(sum) + ((rusty::detail::deref_if_pointer_like(th) * static_cast<uint64_t>(2000029)))) + static_cast<uint64_t>(1);
        }
    }
    return std::move(sum);
}

void Shard::assign_range(const std::string& lo, const std::string& hi) {
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((*this)).owned.size()) {
        if ((rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.lo); }) { return (__r.lo); } else if constexpr (requires { (__r.lo_field); }) { return (__r.lo_field); } else if constexpr (requires { ((*__r).lo); }) { return ((*__r).lo); } else { return ((*__r).lo_field); } }(((*this)).owned[i])) == (lo)) && (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.hi); }) { return (__r.hi); } else if constexpr (requires { (__r.hi_field); }) { return (__r.hi_field); } else if constexpr (requires { ((*__r).hi); }) { return ((*__r).hi); } else { return ((*__r).hi_field); } }(((*this)).owned[i])) == (hi))) {
            return;
        }
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    ((*this)).owned.push(ShardRange::make((lo), (hi)));
}

void Shard::unassign_range(const std::string& lo, const std::string& hi) {
    rusty::Vec<ShardRange> kept = rusty::Vec<ShardRange>::new_();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((*this)).owned.size()) {
        if (!((rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.lo); }) { return (__r.lo); } else if constexpr (requires { (__r.lo_field); }) { return (__r.lo_field); } else if constexpr (requires { ((*__r).lo); }) { return ((*__r).lo); } else { return ((*__r).lo_field); } }(((*this)).owned[i])) == (lo)) && (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.hi); }) { return (__r.hi); } else if constexpr (requires { (__r.hi_field); }) { return (__r.hi_field); } else if constexpr (requires { ((*__r).hi); }) { return ((*__r).hi); } else { return ((*__r).hi_field); } }(((*this)).owned[i])) == (hi)))) {
            kept.push(ShardRange::make([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.lo); }) { return (__r.lo); } else if constexpr (requires { (__r.lo_field); }) { return (__r.lo_field); } else if constexpr (requires { ((*__r).lo); }) { return ((*__r).lo); } else { return ((*__r).lo_field); } }(((*this)).owned[i]), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.hi); }) { return (__r.hi); } else if constexpr (requires { (__r.hi_field); }) { return (__r.hi_field); } else if constexpr (requires { ((*__r).hi); }) { return ((*__r).hi); } else { return ((*__r).hi_field); } }(((*this)).owned[i])));
        }
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    ((*this)).owned = std::move(kept);
}

bool Shard::owns(const std::string& key) const {
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((*this)).owned.size()) {
        if (((key) >= rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.lo); }) { return (__r.lo); } else if constexpr (requires { (__r.lo_field); }) { return (__r.lo_field); } else if constexpr (requires { ((*__r).lo); }) { return ((*__r).lo); } else { return ((*__r).lo_field); } }(((*this)).owned[i]))) && ((key) < rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.hi); }) { return (__r.hi); } else if constexpr (requires { (__r.hi_field); }) { return (__r.hi_field); } else if constexpr (requires { ((*__r).hi); }) { return ((*__r).hi); } else { return ((*__r).hi_field); } }(((*this)).owned[i])))) {
            return true;
        }
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    return false;
}

size_t Shard::owned_count() const {
    return ((*this)).owned.size();
}

void Shard::set_migration(const std::string& lo, const std::string& hi, bool is_source, uint64_t gen) {
    ((*this)).mig_active = true;
    ((*this)).mig_lo = (lo);
    ((*this)).mig_hi = (hi);
    ((*this)).mig_is_source = std::move(is_source);
    ((*this)).mig_locked = false;
    ((*this)).mig_gen = std::move(gen);
}

void Shard::lock_migration() {
    if (((*this)).mig_active) {
        ((*this)).mig_locked = true;
    }
}

void Shard::clear_migration() {
    ((*this)).mig_active = false;
    ((*this)).mig_locked = false;
}

bool Shard::is_migrating() const {
    return ((*this)).mig_active;
}

bool Shard::migration_locked() const {
    return rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_locked);
}

bool Shard::migration_is_source() const {
    return rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_is_source);
}

uint64_t Shard::migration_generation() const {
    return ((*this)).mig_gen;
}

bool Shard::frozen_for(const std::string& key) const {
    return ((rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_locked)) && ((key) >= rusty::detail::deref_if_pointer_like(((*this)).mig_lo))) && ((key) < rusty::detail::deref_if_pointer_like(((*this)).mig_hi));
}
/*RUSTYCPP:GEN-END id=shard.2*/

}  // namespace janus
