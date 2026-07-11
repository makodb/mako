#pragma once

// Migration write-fence bridge — the seam between the storage engine's write
// STAGING points (mbta_wrapper.hh kernels) and the cluster module's
// MigrationGuard. Decl-only ON PURPOSE: mbta_wrapper.hh is included by ~28
// masstree-heavy TUs, and this header must not drag `import cluster` (or
// anything else) into them. The implementation (migration_fence.cc) imports
// the module.
//
// Semantics (docs/mako-book.md s3, transactional migration):
//   - migration_write_fenced: is (table, key) inside a fenced range (frozen OR
//     moved)? Checked at write staging INSIDE the transaction, so together
//     with the epoch drain it closes the check-then-act race -- a txn that
//     began after the fence install must see it here; one that began before is
//     waited out by the drain. The caller aborts the txn retryably on true.
//   - migration_read_moved: is (table, key) inside a MOVED range? (Reads are
//     rejected only after the shard shed the range.)
//   - MigrationFenceBypass: RAII, thread-local. The migration data plane's OWN
//     writes (the copy pulling rows into a destination that may still carry a
//     stale fence, e.g. ping-pong) are coordinator-controlled and exempt.

#include <stddef.h>

#include <string>

namespace mako {

bool migration_write_fenced(const std::string& table, const char* key, size_t len);
bool migration_read_moved(const std::string& table, const char* key, size_t len);

// The staging entry point the kernels call: registers this txn as a staged
// writer (once per txn; the count is the DRAIN's ground truth -- Silo epochs
// cannot serve here because idle 2PC participants pin them forever with
// in_progress-but-empty txns), THEN checks the fence. Returns true if fenced
// (the registration is rolled back if this call created it); the caller
// aborts the txn retryably. Ordering makes the drain proof-grade: a writer is
// counted BEFORE it can stage, and a post-fence writer aborts here before
// staging -- so once the fence is up, the counter only drains, and zero means
// every pre-fence staged-write txn has finished.
bool migration_stage_fenced(const std::string& table, const char* key, size_t len);

// Hook for Transaction::stop (every commit AND abort): closes this thread's
// staged-writer registration, if any.
void migration_fence_txn_done();

// The drain, as a PER-TABLE generation WATERMARK: bumps the fence generation
// and waits until every writer registered under the OLD generation ON THE
// FENCED TABLE has finished. Two disproofs shaped this:
//   - NOT a quiescence wait ("zero staged writers"): a busy shard never
//     quiesces (live TPC-C kept the plain counter nonzero across the whole
//     window; every migration aborted on drain timeout).
//   - NOT a process-wide watermark either: cross-shard 2PC gives individual
//     txns multi-second tails (forensics: workers held registrations 3-5s on
//     UNRELATED warehouses' indexes), so waiting for every pre-fence writer
//     in the process times out on live beds. A txn staged elsewhere can never
//     write into the fenced table -- every staging call re-checks the fence --
//     so only same-table pre-fence writers matter.
// Registrations land in a per-table hash slot (collisions share a slot:
// conservative, the drain may wait for extra writers but never fewer). An
// empty `table` waits on EVERY slot (the table-agnostic "" fence the unit
// beds use). False on timeout.
bool migration_fence_drain_writes_for(const std::string& table, int timeout_ms);

// Back-compat: the table-agnostic drain (waits on every slot).
bool migration_fence_drain_writes(int timeout_ms);

// Begin/poll split of the same drain, for RPC-driven waiting: rrr requests
// carry a ~1s client timeout, so a remote drain must be one BEGIN (the single
// watermark generation bump -- repeated bumps would recycle the generation
// buckets and pull post-fence registrants back into the waited set) followed
// by short-budget POLLS. begin returns the bump's own bucket -- the one
// bucket the polls do NOT wait on (every older bucket is waited: with only
// two parities, a straggler from two drains ago sat in the not-waited parity
// and its pre-fence write leaked past the drain, observed live).
int migration_fence_drain_begin();
bool migration_fence_drained(const std::string& table, int skip_bucket);

// Forensics: dump every open staged-writer registration (thread slot, table,
// gen bucket, age) to stderr. Rate-limit at the call site.
void migration_fence_dump_staged();
// Forensics: one line with the per-gen-bucket counter values for `table`'s
// hash slot (or the whole array for "") plus the overflow bucket -- the
// drain's actual ground truth, catching leaked counts whose per-thread
// forensic slot has since been overwritten.
void migration_fence_dump_residual(const std::string& table);

// Routing-ownership recheck for server-executed ops: the shard that owns
// (table_id, key) per the CURRENT partition-governed routing, or -1 when the
// table is ungoverned (legacy static ownership; nothing to recheck). A
// request routed before a migration cutover can execute after it -- the old
// owner's persistent fence normally rejects it, and this recheck closes the
// same window at the routing layer (serving shard != current owner => reject
// retryably, the client re-routes).
int migration_owner_shard(int table_id, const char* key, size_t len);

// @safe - thread-local depth counter; nestable.
struct MigrationFenceBypass {
    MigrationFenceBypass();
    ~MigrationFenceBypass();
    MigrationFenceBypass(const MigrationFenceBypass&) = delete;
    MigrationFenceBypass& operator=(const MigrationFenceBypass&) = delete;
};

// Staged-writer registration spanning one LOGICAL non-txn write op — every
// internal one-op-txn retry included. The one-op kernels (mbta put/insert)
// retry OCC aborts INSIDE the engine call: per-txn registration
// (migration_stage_fenced + the Transaction::stop hook) closes at the FIRST
// attempt's abort, leaving the internal retry uncounted and unfenced — a
// write that then commits after the drain returned is invisible to the
// catch-up copy (observed live as a lost acknowledged write, racing-writer
// test). This RAII spans the whole engine call instead: construct BEFORE the
// fence check (register-then-check, same proof shape), destruct after the
// final attempt finished — the drain waits out every in-flight logical write
// whose fence check predates the fence. Bypass-aware like the rest.
struct MigrationStagedWriter {
    MigrationStagedWriter();
    ~MigrationStagedWriter();
    MigrationStagedWriter(const MigrationStagedWriter&) = delete;
    MigrationStagedWriter& operator=(const MigrationStagedWriter&) = delete;
private:
    bool counted_;
    unsigned bucket_ = 0;   // fence generation bucket captured at registration
};

}  // namespace mako
