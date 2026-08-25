# Mako local C ABI revision 0 contract

This document is the normative caller contract for the revision-0
`mako_local_*` C ABI declared in the
[public header](../../src/mako/storage/mako_local_abi.h). Revision 0 is still a draft: new symbols,
statuses, sized-option fields, and feature bits may be added before ABI v1.
The numeric status assignments listed here are reserved and must not be
renumbered.

The ABI is a boundary over the existing C++ STO/MassTrans engine. It does not
make the engine, its transaction handles, or its thread-local state into Rust
objects. Unless a rule below says otherwise, input pointers are borrowed only
for the duration of the call and the implementation retains no caller pointer.

## Capability negotiation

A caller must check `mako_local_abi_version()` and
`mako_local_feature_bits()` before using an optional surface. Revision 0
defines these feature bits:

| Feature | Guarantee when present |
| --- | --- |
| `MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS` | The point-transaction surface is available. |
| `MAKO_LOCAL_FEATURE_READ_MY_WRITES` | Point reads and repeated point mutations observe the transaction's staged state. |
| `MAKO_LOCAL_FEATURE_OPACITY` | The linked engine was built with opacity. Absence does not weaken committed-transaction serializability. |
| `MAKO_LOCAL_FEATURE_TRANSACTIONAL_SCANS` | Forward and reverse scans participate in the transaction. |
| `MAKO_LOCAL_FEATURE_SCAN_READ_MY_WRITES` | Scans merge the transaction's staged point mutations. |
| `MAKO_LOCAL_FEATURE_TEST_COMMIT_OBSERVER` | The test-only commit-observer functions are available. |

Raw callers may call `mako_local_txn_scan_chunk()` or
`mako_local_txn_rscan_chunk()` only when both
`MAKO_LOCAL_FEATURE_TRANSACTIONAL_SCANS` and
`MAKO_LOCAL_FEATURE_SCAN_READ_MY_WRITES` are present. The exported symbols may
exist in a profile that omits those bits; calling them in that profile is
outside the contract and is not required to return
`MAKO_LOCAL_FEATURE_UNAVAILABLE`.

The commit-observer functions are usable only when their feature bit is
present. In a build without the bit, both functions return
`MAKO_LOCAL_FEATURE_UNAVAILABLE` before validating their arguments.

## Transaction states and worker health

The operation tables use the following state notation:

| State | Meaning | Worker health | Handle cleanup |
| --- | --- | --- | --- |
| **A** | The transaction is active. | The owner worker is healthy but occupied by this transaction. | The handle must eventually be committed, aborted, or destroyed. |
| **F** | The transaction has finished. Native transaction TLS and database active-transaction accounting were released. | The owner worker is healthy and may begin another transaction. | The small facade handle is still live and must be destroyed on its owner thread. |
| **Q** | Native abort cleanup could not be proved complete. The transaction, referenced buffers, TLS ownership, and database accounting are retained. | The owner worker is transaction-unusable and must be retired. Nontransactional calls are not a health check. | Call destroy once as described below; after it reports `INTERNAL`, do not retry and do not dereference the handle. |
| **D** | The facade handle was destroyed. | The worker health is whatever the preceding state established. | The pointer is invalid and must not be used again. |

Only a properly formed call on the owner thread is guaranteed to surface a
pre-existing **Q** state as `MAKO_LOCAL_INTERNAL`. Argument and output-pointer
validation may run first and return `INVALID_ARGUMENT` or `VALUE_TOO_LARGE`.
`mako_local_thread_attach()` is idempotent and also returns `OK` on an already
attached quarantined worker. These calls are therefore not worker-health
probes.

### Terminal operation rule

For `get`, `put`, `insert`, `remove`, and both scan directions:

- `OK`, `DUPLICATE_WRITE`, and `BUFFER_TOO_SMALL` leave an active transaction
  in **A**. A precondition error also leaves a valid **A** transaction
  unchanged.
- An operation-level `CONFLICT`, `TXN_TOO_LARGE`, or `OUT_OF_MEMORY` is
  returned only after native abort cleanup succeeded. It transitions **A** to
  **F**.
- An operation-level `INTERNAL` is terminal-uncertain. It can mean cleanup
  succeeded (**F**), cleanup failed (**Q**), or the handle was already **Q**.
- `TXN_FINISHED` means the handle was already finished or does not match the
  worker's active TLS transaction; it does not reactivate it.

Every non-null transaction handle remains caller-managed until it reaches
**D** or is abandoned after the one-shot **Q** procedure. An operation that
transitions to **F** does not itself free the handle.

### Destroy exactly once after terminal uncertainty

After an operation or commit returns `INTERNAL`, the owner thread must call
`mako_local_txn_destroy()` exactly once:

- `OK` means the handle was cleanly finished and is now **D**. The worker is
  reusable.
- `INTERNAL` means **Q**. The implementation deliberately retains the handle
  and everything native cleanup might still reference. Do not retry abort or
  destroy, do not submit another transaction to that worker, and do not close
  the owning database expecting `BUSY` to clear.
- `WRONG_THREAD` means the probe was made on the wrong thread and did not
  determine health. Arrange one destroy call on the owner thread.

The same no-retry rule applies when `mako_local_txn_abort()` directly returns
`INTERNAL`. `mako_local_txn_destroy()` on a healthy active transaction performs
the abort itself; an `OK` return consumes the handle.

## Status registry

A status number has no global transaction disposition by itself. For example,
`OUT_OF_MEMORY` from `db_open` has no transaction effect, while
operation-level `OUT_OF_MEMORY` is terminal. Use the operation matrix below.

| Number | Status | Meaning |
| ---: | --- | --- |
| 0 | `MAKO_LOCAL_OK` | The operation succeeded. A point read may still report absence. |
| 1 | `MAKO_LOCAL_CONFLICT` | OCC validation, lock acquisition, or transactional read conflicted. |
| 2 | `MAKO_LOCAL_NOT_ATTACHED` | This OS thread has not successfully attached to the local ABI runtime. |
| 3 | `MAKO_LOCAL_WRONG_THREAD` | A transaction handle was used outside its owner OS thread. |
| 4 | `MAKO_LOCAL_TXN_ALREADY_ACTIVE` | The worker already has an ambient STO transaction. |
| 5 | `MAKO_LOCAL_TXN_FINISHED` | The transaction is inactive or is not the worker's current ABI transaction. |
| 6 | `MAKO_LOCAL_WRONG_DB_OR_TABLE` | A table belongs to another database, or a table name/ID mapping conflicts. |
| 7 | `MAKO_LOCAL_INVALID_ARGUMENT` | A required pointer, slice, option, capacity, flag, or other argument is invalid. |
| 8 | `MAKO_LOCAL_THREAD_LIMIT` | The fixed process-wide STO thread-ID space is exhausted. |
| 9 | `MAKO_LOCAL_BUSY` | A resource cannot currently be claimed or closed. |
| 10 | `MAKO_LOCAL_OUT_OF_MEMORY` | A caught allocation failure occurred. Its lifecycle effect is operation-specific. |
| 11 | `MAKO_LOCAL_INTERNAL` | Native state failed, violated an invariant, or is cleanup-uncertain. Its lifecycle effect is operation-specific. |
| 12 | `MAKO_LOCAL_DUPLICATE_WRITE` | A legacy/no-RYW engine rejected a repeated mutation. Current RYW profiles compose it instead. |
| 13 | `MAKO_LOCAL_TXN_TOO_LARGE` | The weighted transaction item budget was exceeded. On a transaction operation this is terminal. |
| 14 | `MAKO_LOCAL_VALUE_TOO_LARGE` | A table name, key, value, or scan bound exceeded its revision-0 limit. |
| 15 | `MAKO_LOCAL_COMMIT_HOOK_REJECTED` | The post-validation hook returned zero or threw a contained C++ exception. The transaction definitely aborted. |
| 16 | `MAKO_LOCAL_TIMESTAMP_EXHAUSTED` | No representable Mako logical timestamp remained for the requested operation. |
| 17 | `MAKO_LOCAL_BUFFER_TOO_SMALL` | The scan arena cannot hold the next live entry. The transaction remains active. |
| 18 | `MAKO_LOCAL_FEATURE_UNAVAILABLE` | A negotiated optional function is unavailable in this build. |

`mako_local_status_string()` returns diagnostics only. Callers must branch on
the integer status, not on the English string.

Because revision 0 may grow, a future unknown status returned while a
transaction might be active is terminal-uncertain. Do not attempt commit. Apply
the same one-shot owner-thread destroy procedure as for `INTERNAL`, and retire
the worker if cleanup cannot be proved.

## Threading and lifetimes

- Call `mako_local_thread_attach()` once on every long-lived worker that will
  open tables or run transactions. Repeated calls on that worker are harmless.
- An OS thread may have at most one active transaction. A transaction must be
  operated, committed or aborted, and destroyed on the exact OS thread that
  began it.
- An attached worker cannot switch between the local ABI, native Mako, and the
  plain `mtx_*` adapter. STO thread IDs are process-lifetime resources and are
  not recycled. There is no detach operation.
- Database and table handles may be shared by attached workers while their
  lifetime is externally protected. `db_close` must not race with any use.
- Lifetime nesting is `database > table` and `database/table > transaction`.
  The database must remain live until every healthy transaction facade,
  including an **F** facade awaiting destroy, reaches **D**. A **Q** facade
  deliberately retains its database, which must then remain live.
- A table handle is borrowed from its database and has no destroy function. It
  becomes invalid when the database facade is closed.
- `db_close` frees facade table handles, but native MassTrans tables and the
  epoch runtime intentionally remain process-lifetime. Opening another
  database creates a new logical facade; it does not recover or adopt the old
  facade's tables.
- A **Q** transaction retains database active-transaction accounting, so
  `db_close` continues to return `BUSY`. This is intentional containment.

## Output initialization and ownership

Revision 0 has a conditional all-output-pointer rule:

1. A function with one required output pointer first validates that pointer.
   If it is non-null, the function initializes the output before validating
   other inputs.
2. A function with several required scalar output pointers first validates
   that *all* of them are non-null. If any is null, it returns
   `INVALID_ARGUMENT` without writing any of them. Once all are non-null, it
   initializes all of them before any further validation.
3. Therefore, after the output-pointer set itself passed validation, every
   later failure has deterministic scalar outputs. The rule does not promise
   initialization of the valid subset of an invalid, partially null output
   set.

The concrete initial values are:

- `db_open`, `table_open`, and `txn_begin`: `*out = NULL`.
- `txn_get`: `*value_out = NULL`, `*value_len_out = 0`, and
  `*found_out = 0`.
- `txn_put`, `txn_insert`, and `txn_remove`: the verb-result byte is zero.
- Both scan calls: count, used bytes, required bytes, and `done` are zero.

On a successful found `txn_get`, `value_out` is allocated by the ABI and must
be released exactly once with `mako_local_bytes_free()`. A found empty value is
distinct from absence: it has `found_out == 1`, length zero, and a non-null
allocation that must be freed. `mako_local_bytes_free(NULL)` is valid.

Scan descriptors and bytes are written into caller-owned arrays. Native code
retains neither. Only `entry_count_out` descriptors and `arena_used_out` bytes
are initialized results; all other buffer contents are unspecified. On
`BUFFER_TOO_SMALL`, count, used, and `done` remain zero and
`arena_required_out` is the exact bytes required by the first live entry.

The string from `mako_local_status_string()` is static and must not be freed.
The post-validation hook context is borrowed only during the synchronous
commit call. A test-observer callback and context are borrowed from successful
set until successful clear.

## Binary slices and revision-0 limits

Names, keys, values, and scan bounds are binary slices. A null pointer is valid
only with length zero; empty slices are otherwise ordinary values. Table names
and keys are limited to 1 KiB, and values to 1 MiB. Oversized inputs return the
nonterminal `VALUE_TOO_LARGE` before native transaction state is touched.

One transaction has a weighted 512-item budget. `get` and `remove` charge one
item. `put` and `insert` charge `4 + ceil(key_length / 8)`. Every accepted
operation is charged even if it targets an already-mutated key. Scans consume
budget for native transactional items visited. Crossing the limit returns
terminal `TXN_TOO_LARGE` and follows the terminal operation rule.

## Nontransactional operation matrix

In the following tables, status names omit the `MAKO_LOCAL_` prefix for
compactness. “No transaction effect” also means no transaction-worker health
change unless the row states otherwise.

| Export | Possible result or status | Output, ownership, and state effect |
| --- | --- | --- |
| `mako_local_abi_version` | Returns `MAKO_LOCAL_ABI_VERSION` (currently 0). | Scalar result; no ownership or transaction effect. |
| `mako_local_feature_bits` | Returns the linked build's feature mask. | Scalar result; no ownership or transaction effect. |
| `mako_local_scan_options_size` | Returns `MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE`. | Scalar result; no ownership or transaction effect. |
| `mako_local_scan_entry_size` | Returns `sizeof(mako_local_scan_entry)`. | Scalar result; no ownership or transaction effect. |
| `mako_local_status_string` | Returns a known static string or the static unknown-status string. | Never caller-owned; no transaction effect. |
| `mako_local_thread_attach` | `OK`, `BUSY`, `THREAD_LIMIT`, `OUT_OF_MEMORY`, `INTERNAL`. | `OK` attaches or is idempotent. A failed attempt creates no transaction. Because runtime claim and ID reservation can precede later initialization, a failed worker must not switch adapters; retire it after unrecoverable failure. This function is not a **Q** health check. |
| `mako_local_test_set_commit_observer` | With the feature: `OK`, `INVALID_ARGUMENT`, `NOT_ATTACHED`, `BUSY`. Without it: `FEATURE_UNAVAILABLE`. | On `OK`, borrows callback/context in this worker's TLS until clear. No transaction transition; setting during an active transaction does not itself end it. |
| `mako_local_test_clear_commit_observer` | With the feature: `OK`, `NOT_ATTACHED`. Without it: `FEATURE_UNAVAILABLE`. | `OK` is idempotent and ends any callback/context borrow. No transaction transition. |
| `mako_local_advance_mako_timestamp_past` | `OK`, `INVALID_ARGUMENT`, `TIMESTAMP_EXHAUSTED`. | Zero is invalid. A successful call monotonically advances the process clock; smaller observations do not move it backward. The requirement that the value came from a prior hook is a caller precondition, not provenance checked by v0. No transaction effect. |
| `mako_local_db_open` | `OK`, `INVALID_ARGUMENT`, `OUT_OF_MEMORY`, `INTERNAL`. | Once `out` is validated, it is null on every error. `OK` returns a caller-owned database facade requiring `db_close`. Attachment is not required. |
| `mako_local_db_close` | `OK`, `BUSY`, `INTERNAL`; null is `OK`. | `OK` consumes the facade. `BUSY` retains it and is retryable after healthy active transactions finish. On `INTERNAL`, v0 cannot make destruction retry-safe: abandon the pointer without dereferencing or retrying it. No worker-health transition. |
| `mako_local_table_open` | `OK`, `INVALID_ARGUMENT`, `VALUE_TOO_LARGE`, `NOT_ATTACHED`, `WRONG_DB_OR_TABLE`, `OUT_OF_MEMORY`, `INTERNAL`. | Once `out` is validated, it is null on every error. `OK` returns a database-owned borrowed table. Concurrent opens are serialized. Reopening a name with its original ID returns the same handle; name/ID conflicts return `WRONG_DB_OR_TABLE`. An empty binary name is valid. |
| `mako_local_table_id` | Returns the table ID; null returns zero. | Scalar result. ID zero is allowed, so a null result is not distinguishable from a valid zero ID without retaining the original status/handle. The table remains borrowed. |
| `mako_local_bytes_free` | No status; null is accepted. | Consumes only an allocation returned through a successful found `txn_get`. It has no transaction effect. |

An attached worker that gets a late `OUT_OF_MEMORY` or `INTERNAL` from
`mako_local_thread_attach()` has no successful attachment guarantee. Runtime
claim and a process-lifetime thread ID may already have been consumed. A caller
may retry attachment on the same worker when the cause is known to be
transient, but must never switch that worker to another adapter.

## Transaction operation matrix

“Destroy: yes” means the non-null facade remains live after the call and must
eventually be destroyed on its owner thread. It does not mean that a **Q**
destroy should be retried.

| Export | Possible statuses | Outputs and ownership | Transaction disposition, destroy, and worker health |
| --- | --- | --- | --- |
| `mako_local_txn_begin` | `OK`, `INVALID_ARGUMENT`, `NOT_ATTACHED`, `TXN_ALREADY_ACTIVE`, `OUT_OF_MEMORY`, `INTERNAL`. | Once `out` is validated, it is null on every error. `OK` returns a new facade. | `OK`: **A**, destroy yes. `TXN_ALREADY_ACTIVE`: the existing ambient transaction is unchanged; no new handle. `INTERNAL` can also report an existing **Q** transaction. A caught begin failure returns no handle; see the begin-cleanup limitation below. |
| `mako_local_txn_get` | `OK`, `INVALID_ARGUMENT`, `VALUE_TOO_LARGE`, `WRONG_THREAD`, `TXN_FINISHED`, `WRONG_DB_OR_TABLE`, `CONFLICT`, `TXN_TOO_LARGE`, `OUT_OF_MEMORY`, `INTERNAL`. | The three outputs follow conditional all-pointer initialization. `OK` reports absent as null/zero/zero, or returns ABI-owned bytes with `found=1`. | `OK` and precondition errors: **A** remains **A**. `CONFLICT`, `TXN_TOO_LARGE`, `OUT_OF_MEMORY`: **A** to **F**. `INTERNAL`: **F** or **Q**. `TXN_FINISHED`: already **F** or TLS mismatch. Destroy yes. |
| `mako_local_txn_put` | The `get` status set, plus `DUPLICATE_WRITE` in a legacy/no-RYW build. | `created_out` is zero after pointer validation and on every error; `OK` sets whether a key was created. No returned allocation. | `OK`, `DUPLICATE_WRITE`, and precondition errors leave **A**. Terminal statuses follow the terminal operation rule. Destroy yes. |
| `mako_local_txn_insert` | The `get` status set, plus `DUPLICATE_WRITE` in a legacy/no-RYW build. | `inserted_out` is zero after pointer validation and on every error; `OK` sets whether insertion occurred. No returned allocation. | `OK`, `DUPLICATE_WRITE`, and precondition errors leave **A**. Terminal statuses follow the terminal operation rule. Destroy yes. |
| `mako_local_txn_remove` | The `get` status set, plus `DUPLICATE_WRITE` in a legacy/no-RYW build. | `existed_out` is zero after pointer validation and on every error; `OK` sets whether a live key existed. No returned allocation. | `OK`, `DUPLICATE_WRITE`, and precondition errors leave **A**. Terminal statuses follow the terminal operation rule. Destroy yes. |
| `mako_local_txn_scan_chunk` | `OK`, `BUFFER_TOO_SMALL`, `INVALID_ARGUMENT`, `VALUE_TOO_LARGE`, `WRONG_THREAD`, `TXN_FINISHED`, `WRONG_DB_OR_TABLE`, `CONFLICT`, `TXN_TOO_LARGE`, `OUT_OF_MEMORY`, `INTERNAL`. | The four scalar outputs follow conditional all-pointer initialization. `OK` reports caller-owned descriptor/arena extents; `BUFFER_TOO_SMALL` sets only exact required bytes. | Requires both scan feature bits. `OK`, `BUFFER_TOO_SMALL`, and precondition errors leave **A**. Terminal statuses follow the terminal operation rule. Destroy yes. |
| `mako_local_txn_rscan_chunk` | Same status set as `mako_local_txn_scan_chunk`. | Same output and ownership rule, with descending results over the same `[lower, upper)` set. | Requires both scan feature bits. Same lifecycle as forward scan. Destroy yes. |
| `mako_local_txn_commit` | `OK`, `CONFLICT`, `INVALID_ARGUMENT`, `WRONG_THREAD`, `TXN_FINISHED`, `OUT_OF_MEMORY`, `INTERNAL`. | No output pointer. | On a valid active call, `OK` is **F** and definitely committed; `CONFLICT` is **F** and definitely aborted. `OUT_OF_MEMORY` is **F** after cleanup, but commit visibility is conservatively unknown. `INTERNAL` is **F** or **Q** and visibility is unknown. Precondition errors do not attempt a commit. Destroy yes. |
| `mako_local_txn_commit_with_hook` | The ordinary commit set, plus `COMMIT_HOOK_REJECTED` and `TIMESTAMP_EXHAUSTED`. A null hook is `INVALID_ARGUMENT`. | The hook/context are synchronously borrowed. A write transaction calls the hook at most once with its nonzero Mako timestamp; a read-only or conflicting transaction does not. | `OK`: **F**, definitely committed. `CONFLICT`, `COMMIT_HOOK_REJECTED`, and `TIMESTAMP_EXHAUSTED`: **F**, definitely aborted. A null hook leaves **A**. `OUT_OF_MEMORY`/`INTERNAL` follow ordinary commit. Destroy yes. |
| `mako_local_txn_abort` | `OK`, `INVALID_ARGUMENT`, `WRONG_THREAD`, `TXN_FINISHED`, `INTERNAL`. | No output pointer. | Valid `OK`: **A** to **F**, definitely aborted. `INTERNAL`: **Q** or already **Q**; use the one-shot destroy rule. `WRONG_THREAD` leaves owner state unchanged. `TXN_FINISHED` was already **F** or mismatched. Destroy yes. |
| `mako_local_txn_destroy` | `OK`, `WRONG_THREAD`, `TXN_FINISHED`, `INTERNAL`; null is `OK`. | No output pointer. | Null or healthy non-null `OK`: **D**; an **A** transaction is aborted first. `WRONG_THREAD` retains the handle and owner state. An ordinary **F** handle destroys with `OK`; `TXN_FINISHED` here denotes an active/TLS mismatch and retains it. `INTERNAL` denotes **Q** for caller purposes: retain native allocations, retire worker, and do not call again. |

For point and scan precondition statuses, “**A** remains **A**” assumes the
caller supplied a valid active handle. A null handle creates no state; a
wrong-thread call leaves the transaction unchanged on its owner worker; and a
call on an already-finished facade cannot make it active again.

`mako_local_txn_commit_with_hook()` contains a C++ exception thrown by a raw
C++ callback and treats it as rejection. A safe language trampoline must also
prevent its own unwind mechanism from crossing the C boundary.

The test commit observer is diagnostic only: its callback cannot change commit
disposition. The bridge contains and ignores a C++ callback exception, but
callers must still treat unwinding through a C callback as forbidden.

## Scan contract details

Both directions enumerate the same binary-key set. The lower bound is
inclusive, an optional upper bound is exclusive, and a resume key is
exclusive. Forward order is ascending; reverse order is descending. Passing
the final returned key as the next resume cursor does not repeat it.

`entries_capacity` must be nonzero. `arena` may be null only when
`arena_capacity` is zero, `arena_capacity` must fit 32-bit offsets, and the
options prefix must be at least `MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE`. Unknown flags
are invalid. If the first live entry
does not fit, `BUFFER_TOO_SMALL` is nonterminal and the caller retries the
identical request with a larger arena. If at least one entry fits, the function
returns that partial chunk with `OK`; the caller resumes after its final key.
`done == 1` only when the engine proved the effective range exhausted, so a
full chunk can conservatively require one final empty call.

## Revision-0 quarantine limitations and deferred diagnostics

The current live-handle cleanup path implements **Q** conservatively: a caught
native abort-cleanup failure marks the facade poisoned and retains its buffers,
TLS ownership, and database accounting. Public revision 0 exposes that state
only as `MAKO_LOCAL_INTERNAL`, and the one-shot destroy result is the only
current distinction between clean **F** and **Q**.

Revision 0 does **not** yet provide:

- a poison-specific public status;
- a worker-health query;
- a quarantine diagnostic counter;
- a fixed-worker adapter that removes a poisoned worker from service; or
- fake-ABI execution of every active, finished, quarantined, and malformed
  output transition.

Status identity is mechanically synchronized today. The C header's canonical
manifest generates C++ diagnostics and the raw Rust `KnownStatus` catalog; the
safe Rust layer exhaustively classifies that generated enum for ordinary
operation lifecycle and commit disposition. Required-native ABI verification
also compares every linked diagnostic with the manifest. The per-operation
matrix in this document remains the normative explanation of when each status
is permitted and how ownership changes.

There is also one narrower begin path that cannot yet uphold the full **Q**
model. If `mako_local_txn_begin()` throws after native transaction startup, it
makes a best-effort native abort but returns no facade. A failure of that abort
is not separately reported or recorded as a poisoned ABI handle. Consequently,
a caller that receives `OUT_OF_MEMORY` or `INTERNAL` from begin cannot prove
the worker reusable and should retire that worker under revision 0. Adding an
explicit worker poison marker and deterministic begin/abort/destroy failpoint
coverage remains an ABI-freeze gate.

These diagnostics may be added while the ABI reports revision 0. They are
required before promoting this surface to ABI v1; this document does not claim
that the rest of Phase 1C is complete.
