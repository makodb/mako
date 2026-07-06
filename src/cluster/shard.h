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

#if RUSTYCPP_RUST
pub struct Shard {
    shard_id: u32,   // not `id`: a field named the same as the id() method clashes
    alive: bool,
    data: btree_port::BTreeMap<std::string, std::string>,
}
impl Shard {
    // A fresh, live shard with no data.
    fn new(id: u32) -> Shard {
        Shard {
            shard_id: id,
            alive: true,
            data: btree_port::BTreeMap::<std::string, std::string>::new_(),
        }
    }
    fn id(&self) -> u32 { (*self).shard_id }
    fn is_alive(&self) -> bool { (*self).alive }
    fn mark_dead(&mut self) { (*self).alive = false; }
    fn key_count(&self) -> usize { (*self).data.size() }
    fn contains(&self, key: &std::string) -> bool {
        (*self).data.contains_key(key)
    }
    // Store a value (blind overwrite).
    fn put(&mut self, key: &std::string, value: &std::string) {
        (*self).data.insert(key, value);
    }
    // Read a value, or None on miss.
    fn get(&self, key: &std::string) -> rusty::Option<std::string> {
        let found = (*self).data.get(key);
        if found.is_none() { return rusty::None; }
        rusty::Some(std::string(found.unwrap().get()))
    }
    fn remove(&mut self, key: &std::string) {
        (*self).data.remove(key);
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
        }
    }
    // Drop the keys in [lo, hi) (the source sheds the range after COMMIT).
    // Collect-then-remove: cannot mutate the map mid-iteration.
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
}
#endif
/*RUSTYCPP:GEN-BEGIN id=shard.1 version=1 rust_sha256=3501c5aca186e62bfd152cbd12774ab220602d7d956c49b9e8b31cea2f945a73*/
struct Shard;

struct Shard {
    uint32_t shard_id;
    bool alive;
    btree_port::BTreeMap<std::string, std::string> data;

    static Shard new_(uint32_t id);
    uint32_t id() const;
    bool is_alive() const;
    void mark_dead();
    size_t key_count() const;
    bool contains(const std::string& key) const;
    void put(const std::string& key, const std::string& value);
    rusty::Option<std::string> get(const std::string& key) const;
    void remove(const std::string& key);
    void absorb(Shard* other);
    void copy_range_from(Shard* source, const std::string& lo, const std::string& hi);
    void drop_range(const std::string& lo, const std::string& hi);
    size_t range_count(const std::string& lo, const std::string& hi) const;
};


inline Shard Shard::new_(uint32_t id) {
    return Shard{.shard_id = std::move(id), .alive = true, .data = btree_port::BTreeMap<std::string, std::string>::new_()};
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
/*RUSTYCPP:GEN-END id=shard.1*/

}  // namespace janus
