module;

// MigrationGuard — the per-shard freeze registry that makes online migration
// safe on the live non-txn path. During a migration the source shard freezes the
// migrating range [lo,hi); the server's non-txn write handler consults
// is_frozen() and returns RETRY_LATER for a frozen key, so the client backs off
// and retries (landing on the destination once the cutover reroutes it). No
// write is lost; the range is briefly write-unavailable.
//
// A process-global singleton per shard (get_migration_guard()). Thread-safe: the
// non-txn handler runs on many worker threads while the FreezeRange/UnfreezeRange
// RPC handler mutates it, so all state lives behind a rusty::Mutex. The freeze
// check is a small linear scan (a migration touches at most a handful of ranges).
//
// Authored in the inline-Rust DSL (docs/storage-interface.md): the
// `#if RUSTYCPP_RUST` block is the source of truth; regenerate with
// scripts/regen_storage_dsl.sh.

#include <string>
#include <vector>
#include <rusty/mutex.hpp>
#include <rusty/slice.hpp>   // deref_if_pointer_like (generated guard bodies)

export module cluster:migration_guard;

export namespace janus {

// A frozen range: keys in [lo, hi) of `table` are write-frozen. An empty table
// ("") freezes the range across every table (the table-agnostic default).
struct FrozenRange {
    std::string table;
    std::string lo;
    std::string hi;
};

// The guarded state (bare struct; the Mutex is the MigrationGuard field).
struct MigrationGuardState {
    std::vector<FrozenRange> ranges;
};

// ---- kernels: run under an already-held guard ----
// Is `key` inside any frozen range for `table`?
inline bool mg_is_frozen(const MigrationGuardState& s, const std::string& table,
                         const std::string& key) {
    for (const auto& r : s.ranges) {
        if ((r.table.empty() || r.table == table) && key >= r.lo && key < r.hi) {
            return true;
        }
    }
    return false;
}
// Freeze [lo,hi) of `table` (idempotent).
inline void mg_freeze(MigrationGuardState& s, const std::string& table,
                      const std::string& lo, const std::string& hi) {
    for (const auto& r : s.ranges) {
        if (r.table == table && r.lo == lo && r.hi == hi) return;
    }
    s.ranges.push_back(FrozenRange{table, lo, hi});
}
// Unfreeze exactly [lo,hi) of `table` (commit/abort).
inline void mg_unfreeze(MigrationGuardState& s, const std::string& table,
                        const std::string& lo, const std::string& hi) {
    std::vector<FrozenRange> kept;
    for (const auto& r : s.ranges) {
        if (!(r.table == table && r.lo == lo && r.hi == hi)) kept.push_back(r);
    }
    s.ranges = std::move(kept);
}
inline size_t mg_count(const MigrationGuardState& s) { return s.ranges.size(); }
inline void mg_clear(MigrationGuardState& s) { s.ranges.clear(); }

#if RUSTYCPP_RUST
pub struct MigrationGuard {
    state: rusty::Mutex<MigrationGuardState>,
}
impl MigrationGuard {
    fn new() -> MigrationGuard {
        MigrationGuard {
            state: rusty::Mutex::<MigrationGuardState>::default_(),
        }
    }
    fn freeze(&mut self, table: &std::string, lo: &std::string, hi: &std::string) {
        let mut g = (*self).state.lock().unwrap();
        unsafe { mg_freeze((*g), table, lo, hi) };
    }
    fn unfreeze(&mut self, table: &std::string, lo: &std::string, hi: &std::string) {
        let mut g = (*self).state.lock().unwrap();
        unsafe { mg_unfreeze((*g), table, lo, hi) };
    }
    // Consulted by the non-txn write handler on every op for the range's table.
    fn is_frozen(&self, table: &std::string, key: &std::string) -> bool {
        let g = (*self).state.lock().unwrap();
        unsafe { mg_is_frozen((*g), table, key) }
    }
    fn frozen_count(&self) -> usize {
        let g = (*self).state.lock().unwrap();
        unsafe { mg_count((*g)) }
    }
    fn clear(&mut self) {
        let mut g = (*self).state.lock().unwrap();
        unsafe { mg_clear((*g)) };
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=migration_guard.1 version=1 rust_sha256=2783a61ba4f9a572f7cf8027dbf7f96e506d91c83f59459b1cf3a46601246e5b*/
struct MigrationGuard;

struct MigrationGuard {
    rusty::Mutex<MigrationGuardState> state;

    static MigrationGuard new_();
    void freeze(const std::string& table, const std::string& lo, const std::string& hi);
    void unfreeze(const std::string& table, const std::string& lo, const std::string& hi);
    bool is_frozen(const std::string& table, const std::string& key) const;
    size_t frozen_count() const;
    void clear();
};


inline MigrationGuard MigrationGuard::new_() {
    return MigrationGuard{.state = rusty::Mutex<MigrationGuardState>::default_()};
}

inline void MigrationGuard::freeze(const std::string& table, const std::string& lo, const std::string& hi) {
    auto g = ((*this)).state.lock().unwrap();
    // @unsafe
    {
        mg_freeze((rusty::detail::deref_if_pointer_like(g)), table, lo, hi);
    }
}

inline void MigrationGuard::unfreeze(const std::string& table, const std::string& lo, const std::string& hi) {
    auto g = ((*this)).state.lock().unwrap();
    // @unsafe
    {
        mg_unfreeze((rusty::detail::deref_if_pointer_like(g)), table, lo, hi);
    }
}

inline bool MigrationGuard::is_frozen(const std::string& table, const std::string& key) const {
    const auto g = ((*this)).state.lock().unwrap();
    // @unsafe
    {
        return mg_is_frozen((rusty::detail::deref_if_pointer_like(g)), table, key);
    }
}

inline size_t MigrationGuard::frozen_count() const {
    const auto g = ((*this)).state.lock().unwrap();
    // @unsafe
    {
        return mg_count((rusty::detail::deref_if_pointer_like(g)));
    }
}

inline void MigrationGuard::clear() {
    auto g = ((*this)).state.lock().unwrap();
    // @unsafe
    {
        mg_clear((rusty::detail::deref_if_pointer_like(g)));
    }
}
/*RUSTYCPP:GEN-END id=migration_guard.1*/

// Process-global per-shard freeze registry — the server's non-txn handler and
// the FreezeRange RPC handler share this one instance.
// @safe - function-local static
inline MigrationGuard& get_migration_guard() {
    static MigrationGuard instance = MigrationGuard::new_();
    return instance;
}

}  // namespace janus
