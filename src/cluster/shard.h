#pragma once

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
#include <btree_port/btreemap.hpp>   // native-API ordered map (the stub's data)
#include <rusty/vec.hpp>             // drop_range's collect-then-remove buffer
#include <rusty/option.hpp>          // get() -> Option<std::string>
#include <rusty/slice.hpp>           // deref_if_pointer_like (generated bodies)

namespace janus {

// @safe - FNV-1a 64-bit hash of a byte string; the building block for a range
// checksum. Kept as a small C++ kernel (byte loop over raw bytes).
inline uint64_t sh_hash64(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < s.size(); ++i) {
        h ^= static_cast<uint8_t>(s[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

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


inline ShardRange ShardRange::make(std::string lo, std::string hi) {
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
    fn key_count(&self) -> usize { (*self).data.size() }
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
        rusty::Some(std::string(found.unwrap().get()))
    }
    // Delete = a "null write": drop the live value and record a tombstone, so
    // the deletion is a positive fact that copies + checksums like any write.
    fn remove(&mut self, key: &std::string) {
        (*self).data.remove(key);
        (*self).tombstones.insert(key, true);
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
                (*self).data.insert(kv.first, kv.second);
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
                if kv.first >= (*lo) && kv.first < (*hi) {
                    // COPY into locals (not a move of kv.*): the transpiler
                    // moves by-value insert args, and moving out of `source`
                    // would empty it -- but the source must stay intact to keep
                    // serving the range during the background copy.
                    let k: std::string = kv.first;
                    let v: std::string = kv.second;
                    (*self).data.insert(k, v);
                }
            }
            // Carry the source's tombstones too, so a deletion transmits (the
            // destination learns the key is gone, not just "not copied").
            for tk in (*source).tombstones {
                if tk.first >= (*lo) && tk.first < (*hi) {
                    let tkk: std::string = tk.first;
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
            if kv.first >= (*lo) && kv.first < (*hi) {
                victims.push(kv.first);
            }
        }
        let mut i: usize = 0;
        while i < victims.size() {
            (*self).data.remove(victims[i]);
            i = i + 1;
        }
        let mut tvictims: rusty::Vec<std::string> = rusty::Vec::<std::string>::new_();
        for tk in (*self).tombstones {
            if tk.first >= (*lo) && tk.first < (*hi) {
                tvictims.push(tk.first);
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
            if kv.first >= (*lo) && kv.first < (*hi) {
                n = n + 1;
            }
        }
        n
    }
    // Order-independent checksum over the range's live entries AND tombstones.
    // Two shards agree iff they hold exactly the same key->value pairs and the
    // same set of deletions -- the migration's cutover verification.
    fn checksum(&self, lo: &std::string, hi: &std::string) -> u64 {
        let mut sum: u64 = 0;
        for kv in (*self).data {
            if kv.first >= (*lo) && kv.first < (*hi) {
                let kh: u64 = unsafe { sh_hash64(&kv.first) };
                let vh: u64 = unsafe { sh_hash64(&kv.second) };
                sum = sum + (kh * 1000003) + vh;
            }
        }
        for tk in (*self).tombstones {
            if tk.first >= (*lo) && tk.first < (*hi) {
                let th: u64 = unsafe { sh_hash64(&tk.first) };
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
/*RUSTYCPP:GEN-BEGIN id=shard.2 version=1 rust_sha256=4c5aa0560586ec7a1984f848176c54d236328d37894f74d92b5c34d5d29632d3*/
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
    bool is_tombstoned(const std::string& key) const;
    void absorb(Shard* other);
    void copy_range_from(Shard* source, const std::string& lo, const std::string& hi);
    void drop_range(const std::string& lo, const std::string& hi);
    size_t range_count(const std::string& lo, const std::string& hi) const;
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


inline Shard Shard::new_(uint32_t id) {
    return Shard{.shard_id = std::move(id), .alive = true, .data = btree_port::BTreeMap<std::string, std::string>::new_(), .tombstones = btree_port::BTreeMap<std::string, bool>::new_(), .owned = rusty::Vec<ShardRange>::new_(), .mig_active = false, .mig_lo = std::string(""), .mig_hi = std::string(""), .mig_is_source = false, .mig_locked = false, .mig_gen = static_cast<uint64_t>(0)};
}

inline uint32_t Shard::id() const {
    return ((*this)).shard_id;
}

inline bool Shard::is_alive() const {
    return ((*this)).alive;
}

inline void Shard::mark_dead() {
    ((*this)).alive = false;
}

inline size_t Shard::key_count() const {
    return ((*this)).data.size();
}

inline bool Shard::contains(const std::string& key) const {
    return ((*this)).data.contains_key(key);
}

inline void Shard::put(const std::string& key, const std::string& value) {
    ((*this)).data.insert(key, std::move(value));
    ((*this)).tombstones.remove(key);
}

inline rusty::Option<std::string> Shard::get(const std::string& key) const {
    auto found = ((*this)).data.get(key);
    if (found.is_none()) {
        return rusty::None;
    }
    return rusty::Option<std::string>(std::string(found.unwrap().get()));
}

inline void Shard::remove(const std::string& key) {
    ((*this)).data.remove(key);
    ((*this)).tombstones.insert(key, true);
}

inline bool Shard::is_tombstoned(const std::string& key) const {
    return ((*this)).tombstones.contains_key(key);
}

inline void Shard::absorb(Shard* other) {
    // @unsafe
    {
        for (auto&& kv : rusty::for_in((*other).data)) {
            ((*this)).data.insert(std::move(kv.first), std::move(kv.second));
        }
        (*other).data.clear();
    }
}

inline void Shard::copy_range_from(Shard* source, const std::string& lo, const std::string& hi) {
    // @unsafe
    {
        for (auto&& kv : rusty::for_in((*source).data)) {
            if ((rusty::detail::deref_if_pointer_like(kv.first) >= (lo)) && (rusty::detail::deref_if_pointer_like(kv.first) < (hi))) {
                const std::string k = kv.first;
                std::string v = kv.second;
                ((*this)).data.insert(std::move(k), std::move(v));
            }
        }
        for (auto&& tk : rusty::for_in((*source).tombstones)) {
            if ((rusty::detail::deref_if_pointer_like(tk.first) >= (lo)) && (rusty::detail::deref_if_pointer_like(tk.first) < (hi))) {
                const std::string tkk = tk.first;
                ((*this)).data.remove(tkk);
                ((*this)).tombstones.insert(std::move(tkk), true);
            }
        }
    }
}

inline void Shard::drop_range(const std::string& lo, const std::string& hi) {
    rusty::Vec<std::string> victims = rusty::Vec<std::string>::new_();
    for (auto&& kv : rusty::for_in(((*this)).data)) {
        if ((rusty::detail::deref_if_pointer_like(kv.first) >= (lo)) && (rusty::detail::deref_if_pointer_like(kv.first) < (hi))) {
            victims.push(std::move(kv.first));
        }
    }
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < victims.size()) {
        ((*this)).data.remove(victims[i]);
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    rusty::Vec<std::string> tvictims = rusty::Vec<std::string>::new_();
    for (auto&& tk : rusty::for_in(((*this)).tombstones)) {
        if ((rusty::detail::deref_if_pointer_like(tk.first) >= (lo)) && (rusty::detail::deref_if_pointer_like(tk.first) < (hi))) {
            tvictims.push(std::move(tk.first));
        }
    }
    size_t j = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(j) < tvictims.size()) {
        ((*this)).tombstones.remove(tvictims[j]);
        j = rusty::detail::deref_if_pointer_like(j) + static_cast<size_t>(1);
    }
}

inline size_t Shard::range_count(const std::string& lo, const std::string& hi) const {
    size_t n = static_cast<size_t>(0);
    for (auto&& kv : rusty::for_in(((*this)).data)) {
        if ((rusty::detail::deref_if_pointer_like(kv.first) >= (lo)) && (rusty::detail::deref_if_pointer_like(kv.first) < (hi))) {
            n = rusty::detail::deref_if_pointer_like(n) + static_cast<size_t>(1);
        }
    }
    return std::move(n);
}

inline uint64_t Shard::checksum(const std::string& lo, const std::string& hi) const {
    uint64_t sum = static_cast<uint64_t>(0);
    for (auto&& kv : rusty::for_in(((*this)).data)) {
        if ((rusty::detail::deref_if_pointer_like(kv.first) >= (lo)) && (rusty::detail::deref_if_pointer_like(kv.first) < (hi))) {
            const uint64_t kh = sh_hash64(kv.first);
            const uint64_t vh = sh_hash64(kv.second);
            sum = (rusty::detail::deref_if_pointer_like(sum) + ((rusty::detail::deref_if_pointer_like(kh) * static_cast<uint64_t>(1000003)))) + rusty::detail::deref_if_pointer_like(vh);
        }
    }
    for (auto&& tk : rusty::for_in(((*this)).tombstones)) {
        if ((rusty::detail::deref_if_pointer_like(tk.first) >= (lo)) && (rusty::detail::deref_if_pointer_like(tk.first) < (hi))) {
            const uint64_t th = sh_hash64(tk.first);
            sum = (rusty::detail::deref_if_pointer_like(sum) + ((rusty::detail::deref_if_pointer_like(th) * static_cast<uint64_t>(2000029)))) + static_cast<uint64_t>(1);
        }
    }
    return std::move(sum);
}

inline void Shard::assign_range(const std::string& lo, const std::string& hi) {
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((*this)).owned.size()) {
        if ((rusty::detail::deref_if_pointer_like(((*this)).owned[i].lo) == (lo)) && (rusty::detail::deref_if_pointer_like(((*this)).owned[i].hi) == (hi))) {
            return;
        }
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    ((*this)).owned.push(ShardRange::make((lo), (hi)));
}

inline void Shard::unassign_range(const std::string& lo, const std::string& hi) {
    rusty::Vec<ShardRange> kept = rusty::Vec<ShardRange>::new_();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((*this)).owned.size()) {
        if (!((rusty::detail::deref_if_pointer_like(((*this)).owned[i].lo) == (lo)) && (rusty::detail::deref_if_pointer_like(((*this)).owned[i].hi) == (hi)))) {
            kept.push(ShardRange::make(((*this)).owned[i].lo, ((*this)).owned[i].hi));
        }
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    ((*this)).owned = std::move(kept);
}

inline bool Shard::owns(const std::string& key) const {
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < ((*this)).owned.size()) {
        if (((key) >= rusty::detail::deref_if_pointer_like(((*this)).owned[i].lo)) && ((key) < rusty::detail::deref_if_pointer_like(((*this)).owned[i].hi))) {
            return true;
        }
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    return false;
}

inline size_t Shard::owned_count() const {
    return ((*this)).owned.size();
}

inline void Shard::set_migration(const std::string& lo, const std::string& hi, bool is_source, uint64_t gen) {
    ((*this)).mig_active = true;
    ((*this)).mig_lo = (lo);
    ((*this)).mig_hi = (hi);
    ((*this)).mig_is_source = std::move(is_source);
    ((*this)).mig_locked = false;
    ((*this)).mig_gen = std::move(gen);
}

inline void Shard::lock_migration() {
    if (((*this)).mig_active) {
        ((*this)).mig_locked = true;
    }
}

inline void Shard::clear_migration() {
    ((*this)).mig_active = false;
    ((*this)).mig_locked = false;
}

inline bool Shard::is_migrating() const {
    return ((*this)).mig_active;
}

inline bool Shard::migration_locked() const {
    return rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_locked);
}

inline bool Shard::migration_is_source() const {
    return rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_is_source);
}

inline uint64_t Shard::migration_generation() const {
    return ((*this)).mig_gen;
}

inline bool Shard::frozen_for(const std::string& key) const {
    return ((rusty::detail::deref_if_pointer_like(((*this)).mig_active) && rusty::detail::deref_if_pointer_like(((*this)).mig_locked)) && ((key) >= rusty::detail::deref_if_pointer_like(((*this)).mig_lo))) && ((key) < rusty::detail::deref_if_pointer_like(((*this)).mig_hi));
}
/*RUSTYCPP:GEN-END id=shard.2*/

}  // namespace janus
