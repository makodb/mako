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

// @safe - thread-local depth counter; nestable.
struct MigrationFenceBypass {
    MigrationFenceBypass();
    ~MigrationFenceBypass();
    MigrationFenceBypass(const MigrationFenceBypass&) = delete;
    MigrationFenceBypass& operator=(const MigrationFenceBypass&) = delete;
};

}  // namespace mako
