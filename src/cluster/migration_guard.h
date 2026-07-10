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

// A fenced range of `table`. Two states:
//   frozen (moved=false): keys in [lo,hi) are WRITE-fenced -- the mid-migration
//     contract (reads keep serving from the source until cutover).
//   moved  (moved=true):  the shard SHED the range at commit -- reads AND
//     writes are rejected (a stale-routed read must not see a clean miss for
//     data that moved); clients retry, reload config, land on the new owner.
// An empty table ("") fences the range across every table.
struct FrozenRange {
    std::string table;
    std::string lo;
    std::string hi;
    bool moved = false;
};

// The guarded state (bare struct; the Mutex is the MigrationGuard field).
struct MigrationGuardState {
    std::vector<FrozenRange> ranges;
};

// ---- kernels: run under an already-held guard ----
// Is `key` inside any fenced range for `table`? (Writes are rejected in BOTH
// states, so this matches frozen and moved entries.)
inline bool mg_is_frozen(const MigrationGuardState& s, const std::string& table,
                         const std::string& key) {
    for (const auto& r : s.ranges) {
        if ((r.table.empty() || r.table == table) && key >= r.lo && key < r.hi) {
            return true;
        }
    }
    return false;
}
// Is `key` inside a MOVED range for `table`? (Reads are rejected only after the
// shard shed the range -- during a migration the source keeps serving reads.)
inline bool mg_is_moved(const MigrationGuardState& s, const std::string& table,
                        const std::string& key) {
    for (const auto& r : s.ranges) {
        if (r.moved && (r.table.empty() || r.table == table) &&
            key >= r.lo && key < r.hi) {
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
    s.ranges.push_back(FrozenRange{table, lo, hi, false});
}
// Upgrade [lo,hi) of `table` to MOVED (the shard shed the range at commit):
// flips a matching frozen entry, or inserts one if the fence was never set.
inline void mg_mark_moved(MigrationGuardState& s, const std::string& table,
                          const std::string& lo, const std::string& hi) {
    for (auto& r : s.ranges) {
        if (r.table == table && r.lo == lo && r.hi == hi) {
            r.moved = true;
            return;
        }
    }
    s.ranges.push_back(FrozenRange{table, lo, hi, true});
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
    // Consulted by READ paths: true only after the shard shed the range.
    fn is_moved(&self, table: &std::string, key: &std::string) -> bool {
        let g = (*self).state.lock().unwrap();
        unsafe { mg_is_moved((*g), table, key) }
    }
    // The shard shed [lo,hi) of `table` (DropRange at commit): reads join writes
    // behind the fence until stale routers reload.
    fn mark_moved(&mut self, table: &std::string, lo: &std::string, hi: &std::string) {
        let mut g = (*self).state.lock().unwrap();
        unsafe { mg_mark_moved((*g), table, lo, hi) };
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
/*RUSTYCPP:GEN-BEGIN id=migration_guard.1 version=1 rust_sha256=6075f9a33f3e3e6f13728711603e0d12afc16ba10df357832fd379885c8967a0*/
struct MigrationGuard;

struct MigrationGuard {
    rusty::Mutex<MigrationGuardState> state;

    static MigrationGuard new_();
    void freeze(const std::string& table, const std::string& lo, const std::string& hi);
    void unfreeze(const std::string& table, const std::string& lo, const std::string& hi);
    bool is_frozen(const std::string& table, const std::string& key) const;
    bool is_moved(const std::string& table, const std::string& key) const;
    void mark_moved(const std::string& table, const std::string& lo, const std::string& hi);
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

inline bool MigrationGuard::is_moved(const std::string& table, const std::string& key) const {
    const auto g = ((*this)).state.lock().unwrap();
    // @unsafe
    {
        return mg_is_moved((rusty::detail::deref_if_pointer_like(g)), table, key);
    }
}

inline void MigrationGuard::mark_moved(const std::string& table, const std::string& lo, const std::string& hi) {
    auto g = ((*this)).state.lock().unwrap();
    // @unsafe
    {
        mg_mark_moved((rusty::detail::deref_if_pointer_like(g)), table, lo, hi);
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
