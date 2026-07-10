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

// The drain: wait until no pre-fence staged-write txn remains (see above).
// False on timeout.
bool migration_fence_drain_writes(int timeout_ms);

// @safe - thread-local depth counter; nestable.
struct MigrationFenceBypass {
    MigrationFenceBypass();
    ~MigrationFenceBypass();
    MigrationFenceBypass(const MigrationFenceBypass&) = delete;
    MigrationFenceBypass& operator=(const MigrationFenceBypass&) = delete;
};

}  // namespace mako
