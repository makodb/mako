# Fix Commit/Rollback Implementation Plan

## Issue
ISSUE-1886cab7-2

## Problem
`HandleCommit` and `HandleRollback` in `src/mako/client_service.cc:89-127` were described as "no-ops that always return SUCCESS."

## Analysis

### Transaction Model
Mako uses **auto-commit semantics** where each Put/Get/Delete operation is immediately committed:
- `shard_put()` calls `transPut()` followed by `Sto::shard_try_lock_last_writeset()`
- This means each operation is atomically committed immediately, not buffered

### Existing Implementation (Already Complete)
ShardReceiver has public methods that MakoClientService delegates to:
- `BeginClientTransaction(client_id, txn_counter)`: Generates txn_id, tracks in `client_transactions_`
- `CommitClientTransaction(txn_id)`: Removes from tracking, returns SUCCESS/ERROR
- `RollbackClientTransaction(txn_id)`: Removes from tracking (no undo possible with auto-commit)

The delegation was already implemented in a previous commit (7a6a5847).

### Bug Fixed
The original `RollbackClientTransaction` and `HandleClientRollbackRequest` incorrectly called
`db->shard_abort_txn(nullptr)` which would abort the current thread's transaction state,
not the client's transaction. This was incorrect because:
1. Auto-commit means operations are already committed - cannot be undone
2. The nullptr argument refers to thread-local state, not the client's txn_id

**Fix**: Removed the incorrect `shard_abort_txn(nullptr)` call and added documentation
clarifying the auto-commit semantics.

## Solution (Applied)

1. **server.h** - Already had public methods for BeginClientTransaction, CommitClientTransaction, RollbackClientTransaction

2. **server.cc** - Fixed RollbackClientTransaction and HandleClientRollbackRequest:
   - Removed incorrect `db->shard_abort_txn(nullptr)` call
   - Added comments explaining auto-commit semantics
   - Rollback now only removes txn from tracking (cannot undo auto-committed ops)

3. **client_service.cc** - Already delegating to ShardReceiver methods

### Changes Made: ~10 lines modified

## Auto-Commit Semantics Documentation

Mako's client API uses auto-commit semantics:
- **BeginTransaction**: Creates a transaction tracking entry (for ID generation)
- **Put/Get/Delete**: Each operation is immediately committed (not buffered)
- **Commit**: Removes transaction from tracking (operations already committed)
- **Rollback**: Removes transaction from tracking (cannot undo already-committed operations)

This design ensures durability (no buffered data lost on crash) but means true
multi-operation atomic transactions are not supported through this API.

## Verification
- Run CI tests: `./ci/ci.sh all`
- The clientServer test should continue to pass
