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

A required-native caller first identifies the linked engine. This implementation
reports `MAKO_LOCAL_ENGINE_ID` (`mako-local/sto-masstrans`) from
`mako_local_engine_id()`. `mako_local_build_fingerprint_size()` must equal
`MAKO_LOCAL_BUILD_FINGERPRINT_SIZE` (32), and
`mako_local_build_fingerprint()` returns exactly that many SHA-256 identity
bytes. The engine-ID string and fingerprint bytes have process lifetime, are
read-only, and must not be freed. The string is NUL-terminated; the fingerprint
is a fixed-size binary value and is not a string.

The fingerprint covers the canonical header, the configured source and
compiler-dependency closure of every native static archive linked by
`mako-local`, effective compile commands and relevant CMake configuration,
generated configuration, compiler target/predefines/binary, and the linked
libc++/libc++abi identities. It is a build-compatibility identity, not a
durability checksum or an authentication primitive. Required-native Rust
builds independently recompute it, embed the expected bytes, and reference a
digest-named symbol in `libmako.a`; startup rejects a mismatched engine ID,
size, or bytes. A selected CMake build tree must remain immutable for the
duration of the Cargo link: the gate verifies all four archives before linking,
and the digest anchor pins `libmako.a`, but external concurrent replacement of
the separately linked dependency archives is outside the supported build
protocol.

After the identity handshake, a caller must check
`mako_local_abi_version()` and `mako_local_feature_bits()` before using an
optional surface. Revision 0 defines these feature bits:

| Feature | Guarantee when present |
| --- | --- |
| `MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS` | The point-transaction surface is available. |
| `MAKO_LOCAL_FEATURE_READ_MY_WRITES` | Point reads and repeated point mutations observe the transaction's staged state. |
| `MAKO_LOCAL_FEATURE_OPACITY` | The linked engine was built with opacity. Absence does not weaken committed-transaction serializability. |
| `MAKO_LOCAL_FEATURE_TRANSACTIONAL_SCANS` | Forward and reverse scans participate in the transaction. |
| `MAKO_LOCAL_FEATURE_SCAN_READ_MY_WRITES` | Scans merge the transaction's staged point mutations. |
| `MAKO_LOCAL_FEATURE_TEST_COMMIT_OBSERVER` | The test-only commit-observer functions are available. |
| `MAKO_LOCAL_FEATURE_TEST_CLEANUP_FAILURES` (bit 6) | The test-only cleanup-failure arm and clear functions are available. |

Raw callers may call `mako_local_txn_scan_chunk()` or
`mako_local_txn_rscan_chunk()` only when both
`MAKO_LOCAL_FEATURE_TRANSACTIONAL_SCANS` and
`MAKO_LOCAL_FEATURE_SCAN_READ_MY_WRITES` are present. The exported symbols may
exist in a profile that omits those bits; calling them in that profile is
outside the contract and is not required to return
`MAKO_LOCAL_FEATURE_UNAVAILABLE`.

The commit-observer and cleanup-failure functions are usable only when their
respective feature bit is present. In a build without the corresponding bit,
the functions return `MAKO_LOCAL_FEATURE_UNAVAILABLE` before validating their
arguments. Test-only symbols remain exported in production profiles so link
identity does not depend on whether either test feature was compiled in.

## Transaction states and worker health

The operation tables use the following state notation:

| State | Meaning | Worker health | Handle cleanup |
| --- | --- | --- | --- |
| **A** | The transaction is active. | The owner worker is healthy but occupied by this transaction. | The handle must eventually be committed, aborted, or destroyed. |
| **F** | The transaction has finished. Native transaction TLS and database active-transaction accounting were released. | The owner worker is healthy and may begin another transaction. | The small facade handle is still live and must be destroyed on its owner thread. |
| **Q** | Native transaction or facade cleanup could not be proved complete. Every facade, referenced buffer, TLS owner, and database-accounting reference that might still be live is retained. A failed begin can enter **Q** without returning a facade. | An independent TLS quarantine marker makes the owner worker permanently transaction-unusable. Retire it. | If a facade was returned, call destroy once as described below; after destroy reports `WORKER_POISONED`, do not retry or dereference it. A failed begin returns no handle to destroy. |
| **D** | The facade handle was destroyed. | The worker health is whatever the preceding state established. | The pointer is invalid and must not be used again. |

Only a properly formed call on the owner thread is guaranteed to surface a
pre-existing **Q** state as `MAKO_LOCAL_WORKER_POISONED`. Argument and output-pointer
validation may run first and return `INVALID_ARGUMENT` or `VALUE_TOO_LARGE`.
`mako_local_thread_attach()` returns `WORKER_POISONED` rather than its normal
idempotent `OK` on an already attached quarantined worker. The authoritative
probe is `mako_local_worker_health()`: it returns `NOT_ATTACHED`, `OK`, or
`WORKER_POISONED` for the calling OS worker. `OK` means healthy, not idle; an
active healthy transaction can still make a later begin return
`TXN_ALREADY_ACTIVE`.

The safe Rust `worker_health()` maps these three expected C statuses to
`WorkerHealth::{NotAttached, Healthy, Poisoned}`. Its error channel is reserved
for ABI drift or a status outside this query's contract, so callers cannot
accidentally treat “not attached” or “poisoned” as an uninspected generic error.

### Terminal operation rule

For `get`, `put`, `insert`, `remove`, and both scan directions:

- `OK`, `DUPLICATE_WRITE`, and `BUFFER_TOO_SMALL` leave an active transaction
  in **A**. A precondition error also leaves a valid **A** transaction
  unchanged.
- An operation-level `CONFLICT`, `TXN_TOO_LARGE`, or `OUT_OF_MEMORY` is
  returned only after native abort cleanup succeeded. It transitions **A** to
  **F**.
- An operation-level `INTERNAL` is terminal, but native abort cleanup
  succeeded: **A** transitions to **F**. The underlying failure remains an
  internal error even though the worker is reusable after facade destruction.
- `WORKER_POISONED` means cleanup could not be proved complete or the worker
  was already quarantined: **A** transitions to **Q**, or remains **Q**.
- `TXN_FINISHED` means the handle was already finished or does not match the
  worker's active TLS transaction; it does not reactivate it.

Every non-null transaction handle remains caller-managed until it reaches
**D** or is abandoned after the one-shot **Q** procedure. An operation that
transitions to **F** does not itself free the handle.

### Destroy exactly once after terminal failure

After a terminal operation returns `INTERNAL`, or any transaction call returns
`WORKER_POISONED`, the owner thread must call `mako_local_txn_destroy()` exactly
once if it has a non-null facade:

- `OK` means the handle was cleanly finished and is now **D**. The worker is
  reusable when `mako_local_worker_health()` also returns `OK`.
- `WORKER_POISONED` means **Q**. The implementation deliberately retains the
  handle and everything native cleanup might still reference. Do not retry
  abort or destroy, do not submit another transaction to that worker, and do
  not close the owning database expecting `BUSY` to clear.
- `WRONG_THREAD` means the probe was made on the wrong thread and did not
  determine health. Arrange one destroy call on the owner thread.

The same no-retry rule applies when `mako_local_txn_abort()` directly returns
`WORKER_POISONED`, or when the first call to `mako_local_txn_destroy()` itself
returns it. `mako_local_txn_destroy()` on a healthy active transaction performs
the abort itself; an `OK` return consumes the handle. A begin that returns
`WORKER_POISONED` initializes its output to null and has no caller-owned handle;
retire the worker without calling destroy.

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
| 11 | `MAKO_LOCAL_INTERNAL` | Native state failed or violated an invariant. Transaction disposition and commit visibility remain operation-specific. |
| 12 | `MAKO_LOCAL_DUPLICATE_WRITE` | A legacy/no-RYW engine rejected a repeated mutation. Current RYW profiles compose it instead. |
| 13 | `MAKO_LOCAL_TXN_TOO_LARGE` | The weighted transaction item budget was exceeded. On a transaction operation this is terminal. |
| 14 | `MAKO_LOCAL_VALUE_TOO_LARGE` | A table name, key, value, or scan bound exceeded its revision-0 limit. |
| 15 | `MAKO_LOCAL_COMMIT_HOOK_REJECTED` | The post-validation hook returned zero or threw a contained C++ exception. The transaction definitely aborted. |
| 16 | `MAKO_LOCAL_TIMESTAMP_EXHAUSTED` | No representable Mako logical timestamp remained for the requested operation. |
| 17 | `MAKO_LOCAL_BUFFER_TOO_SMALL` | The scan arena cannot hold the next live entry. The transaction remains active. |
| 18 | `MAKO_LOCAL_FEATURE_UNAVAILABLE` | A negotiated optional function is unavailable in this build. |
| 19 | `MAKO_LOCAL_WORKER_POISONED` | Native cleanup could not be proved complete. The calling transaction worker is permanently quarantined. |

`mako_local_status_string()` returns diagnostics only. Callers must branch on
the integer status, not on the English string.

Because revision 0 may grow, a future unknown status returned while a
transaction might be active is terminal-uncertain. Do not attempt commit. Apply
the same one-shot owner-thread destroy procedure as for `WORKER_POISONED`, and
retire the worker if cleanup cannot be proved. Every revision-0 extension must
set the authoritative TLS quarantine before returning whenever cleanup is not
proved complete. The safe Rust entry points call `mako_local_thread_attach()`
again before every later table-open or transaction-begin admission; on an
already attached worker, that is an enforcement gate over the same quarantine
flag exposed by `mako_local_worker_health()`.

## Threading and lifetimes

- Call `mako_local_thread_attach()` once on every long-lived worker that will
  open tables or run transactions. Repeated calls on a healthy worker are
  harmless. A quarantined worker instead returns `WORKER_POISONED` and can
  never be reattached or reset.
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
- Quarantine is stored independently of a transaction facade in TLS. It
  therefore survives a failed begin that returned no handle and remains
  observable until the worker thread is retired.

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
On successful absence the pointer and length are zero, and `found_out == 0`.
Every successful boolean byte from get, put, insert, remove, or scan is exactly
zero or one; other byte values are malformed ABI output rather than truthy
values. A returned point-value length never exceeds
`MAKO_LOCAL_MAX_VALUE_BYTES`.

Scan descriptors and bytes are written into caller-owned arrays. Native code
retains neither. Only `entry_count_out` descriptors and `arena_used_out` bytes
are initialized results; all other buffer contents are unspecified. On
`BUFFER_TOO_SMALL`, count, used, and `done` remain zero and
`arena_required_out` is the exact bytes required by the first live entry. On
`OK`, `arena_required_out` is zero, counts do not exceed their supplied
capacities, and every reported descriptor slice lies within `arena_used_out`.

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
| `mako_local_engine_id` | Returns `MAKO_LOCAL_ENGINE_ID`. | Static NUL-terminated string with process lifetime; never null, never caller-owned, and no transaction effect. |
| `mako_local_build_fingerprint` | Returns the linked build's identity bytes. | Static read-only binary bytes with process lifetime; never null, never caller-owned, and no transaction effect. Read exactly the separately reported size. |
| `mako_local_build_fingerprint_size` | Returns `MAKO_LOCAL_BUILD_FINGERPRINT_SIZE` (currently 32). | Scalar result; no ownership or transaction effect. |
| `mako_local_abi_version` | Returns `MAKO_LOCAL_ABI_VERSION` (currently 0). | Scalar result; no ownership or transaction effect. |
| `mako_local_feature_bits` | Returns the linked build's feature mask. | Scalar result; no ownership or transaction effect. |
| `mako_local_scan_options_size` | Returns `MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE`. | Scalar result; no ownership or transaction effect. |
| `mako_local_scan_entry_size` | Returns `sizeof(mako_local_scan_entry)`. | Scalar result; no ownership or transaction effect. |
| `mako_local_status_string` | Returns a known static string or the static unknown-status string. | Never caller-owned; no transaction effect. |
| `mako_local_thread_attach` | `OK`, `BUSY`, `THREAD_LIMIT`, `OUT_OF_MEMORY`, `INTERNAL`, `WORKER_POISONED`. | `OK` attaches or is idempotent only while healthy. `WORKER_POISONED` is a permanent rejection. A failed initial attempt creates no transaction. Because runtime claim and ID reservation can precede later initialization, a failed worker must not switch adapters; retire it after unrecoverable failure. |
| `mako_local_worker_health` | `OK`, `NOT_ATTACHED`, `WORKER_POISONED`. | Reports the calling OS worker's TLS state. `OK` means attached and healthy even when an **A** transaction occupies it. `WORKER_POISONED` is permanent. No transaction transition. |
| `mako_local_quarantined_worker_count` | Returns a `uint64_t` count. | Process-wide monotonic diagnostic count of workers that entered **Q**. One worker increments it at most once; reads do not alter health or transaction state. |
| `mako_local_test_set_commit_observer` | With the feature: `OK`, `INVALID_ARGUMENT`, `NOT_ATTACHED`, `BUSY`. Without it: `FEATURE_UNAVAILABLE`. | On `OK`, borrows callback/context in this worker's TLS until clear. No transaction transition; setting during an active transaction does not itself end it. |
| `mako_local_test_clear_commit_observer` | With the feature: `OK`, `NOT_ATTACHED`. Without it: `FEATURE_UNAVAILABLE`. | `OK` is idempotent and ends any callback/context borrow. No transaction transition. |
| `mako_local_test_arm_cleanup_failure` | With the feature: `OK`, `INVALID_ARGUMENT`, `NOT_ATTACHED`, `BUSY`, `WORKER_POISONED`. Without it: `FEATURE_UNAVAILABLE`. | Arms one valid `MAKO_LOCAL_CLEANUP_BOUNDARY_*` value in the calling worker's TLS. A second arm before consumption or clear is `BUSY`. The next matching cleanup consumes the arm before forcing a native cleanup exception. No ordinary transaction transition occurs merely by arming. |
| `mako_local_test_clear_cleanup_failure` | With the feature: `OK`, `NOT_ATTACHED`, `WORKER_POISONED`. Without it: `FEATURE_UNAVAILABLE`. | Clears an unconsumed arm and is otherwise idempotent. It cannot recover or make reusable a poisoned worker. |
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
| `mako_local_txn_begin` | `OK`, `INVALID_ARGUMENT`, `NOT_ATTACHED`, `TXN_ALREADY_ACTIVE`, `OUT_OF_MEMORY`, `INTERNAL`, `WORKER_POISONED`. | Once `out` is validated, it is null on every error. `OK` returns a new facade. | `OK`: **A**, destroy yes. `TXN_ALREADY_ACTIVE`: an existing healthy ambient transaction is unchanged; no new handle. A caught begin failure returns its original status only after native cleanup completed. Cleanup uncertainty or a pre-existing TLS quarantine returns `WORKER_POISONED`, leaves `out` null, retains an internal quarantine anchor, and requires worker retirement without a destroy call. |
| `mako_local_txn_get` | `OK`, `INVALID_ARGUMENT`, `VALUE_TOO_LARGE`, `WRONG_THREAD`, `TXN_FINISHED`, `WRONG_DB_OR_TABLE`, `CONFLICT`, `TXN_TOO_LARGE`, `OUT_OF_MEMORY`, `INTERNAL`, `WORKER_POISONED`. | The three outputs follow conditional all-pointer initialization. `OK` reports absent as null/zero/zero, or returns ABI-owned bytes with `found=1`. | `OK` and precondition errors: **A** remains **A**. `CONFLICT`, `TXN_TOO_LARGE`, `OUT_OF_MEMORY`, and `INTERNAL`: **A** to **F** after successful cleanup. `WORKER_POISONED`: **A** to **Q** or already **Q**. `TXN_FINISHED`: already **F** or TLS mismatch. Destroy yes for every returned facade. |
| `mako_local_txn_put` | The `get` status set, plus `DUPLICATE_WRITE` in a legacy/no-RYW build. | `created_out` is zero after pointer validation and on every error; `OK` sets whether a key was created. No returned allocation. | `OK`, `DUPLICATE_WRITE`, and precondition errors leave **A**. Terminal statuses follow the terminal operation rule. Destroy yes. |
| `mako_local_txn_insert` | The `get` status set, plus `DUPLICATE_WRITE` in a legacy/no-RYW build. | `inserted_out` is zero after pointer validation and on every error; `OK` sets whether insertion occurred. No returned allocation. | `OK`, `DUPLICATE_WRITE`, and precondition errors leave **A**. Terminal statuses follow the terminal operation rule. Destroy yes. |
| `mako_local_txn_remove` | The `get` status set, plus `DUPLICATE_WRITE` in a legacy/no-RYW build. | `existed_out` is zero after pointer validation and on every error; `OK` sets whether a live key existed. No returned allocation. | `OK`, `DUPLICATE_WRITE`, and precondition errors leave **A**. Terminal statuses follow the terminal operation rule. Destroy yes. |
| `mako_local_txn_scan_chunk` | `OK`, `BUFFER_TOO_SMALL`, `INVALID_ARGUMENT`, `VALUE_TOO_LARGE`, `WRONG_THREAD`, `TXN_FINISHED`, `WRONG_DB_OR_TABLE`, `CONFLICT`, `TXN_TOO_LARGE`, `OUT_OF_MEMORY`, `INTERNAL`, `WORKER_POISONED`. | The four scalar outputs follow conditional all-pointer initialization. `OK` reports caller-owned descriptor/arena extents; `BUFFER_TOO_SMALL` sets only exact required bytes. | Requires both scan feature bits. `OK`, `BUFFER_TOO_SMALL`, and precondition errors leave **A**. Terminal statuses follow the terminal operation rule. Destroy yes. |
| `mako_local_txn_rscan_chunk` | Same status set as `mako_local_txn_scan_chunk`. | Same output and ownership rule, with descending results over the same `[lower, upper)` set. | Requires both scan feature bits. Same lifecycle as forward scan. Destroy yes. |
| `mako_local_txn_commit` | `OK`, `CONFLICT`, `INVALID_ARGUMENT`, `WRONG_THREAD`, `TXN_FINISHED`, `WORKER_POISONED`. | No output pointer. | On a valid active call, `OK` is **F** and definitely committed; `CONFLICT` is **F** and definitely aborted. Any contained exception from native commit can have unknown cleanup progress and therefore returns `WORKER_POISONED`: **Q**, visibility unknown. Precondition errors do not attempt a commit. Destroy yes. |
| `mako_local_txn_commit_with_hook` | The ordinary commit set, plus `COMMIT_HOOK_REJECTED` and `TIMESTAMP_EXHAUSTED`. A null hook is `INVALID_ARGUMENT`. | The hook/context are synchronously borrowed. A write transaction calls the hook at most once with its nonzero Mako timestamp; a read-only or conflicting transaction does not. | `OK`: **F**, definitely committed. `CONFLICT`, `COMMIT_HOOK_REJECTED`, and `TIMESTAMP_EXHAUSTED`: **F**, definitely aborted. A null hook leaves **A**. `WORKER_POISONED` follows ordinary commit. Destroy yes. |
| `mako_local_txn_abort` | `OK`, `INVALID_ARGUMENT`, `WRONG_THREAD`, `TXN_FINISHED`, `WORKER_POISONED`. | No output pointer. | Valid `OK`: **A** to **F**, definitely aborted. `WORKER_POISONED`: **A** to **Q** or already **Q**; use the one-shot destroy rule. `WRONG_THREAD` leaves owner state unchanged. `TXN_FINISHED` was already **F** or mismatched. Destroy yes. |
| `mako_local_txn_destroy` | `OK`, `WRONG_THREAD`, `TXN_FINISHED`, `WORKER_POISONED`; null is `OK`. | No output pointer. | Null or healthy non-null `OK`: **D**; an **A** transaction is aborted first. `WRONG_THREAD` retains the handle and owner state. An ordinary **F** handle destroys with `OK`; `TXN_FINISHED` here denotes an active/TLS mismatch and retains it. `WORKER_POISONED` denotes **Q**: retain native allocations, retire worker, and do not call again. |

For point and scan precondition statuses, “**A** remains **A**” assumes the
caller supplied a valid active handle. A null handle creates no state; a
wrong-thread call leaves the transaction unchanged on its owner worker; and a
call on an already-finished facade cannot make it active again.

Commit disposition and facade cleanup are independent results. The status from
`mako_local_txn_commit()` or `mako_local_txn_commit_with_hook()` establishes
the disposition in the matrix above; the later destroy status cannot rewrite
it. In particular, commit `OK` followed by destroy `WORKER_POISONED` is still
definitely committed, while `CONFLICT` followed by destroy
`WORKER_POISONED` is still definitely aborted. A safe wrapper must preserve
both results, as `CommitReport` does, rather than replacing the commit status
with the cleanup status. If the commit call itself returns `WORKER_POISONED`,
visibility is unknown and the durability obligation must remain pinned.

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

## Quarantine diagnostics and cleanup-failure tests

Revision 0 represents **Q** explicitly. A caught native cleanup failure marks
both any live facade and an independent TLS worker flag, returns
`MAKO_LOCAL_WORKER_POISONED`, and retains everything cleanup might still
reference. `mako_local_worker_health()` exposes that TLS state even when a
failed begin returned no facade. `mako_local_quarantined_worker_count()` is a
process-wide monotonic diagnostic: the first quarantine transition on a worker
increments it once, and repeated probes or cleanup attempts cannot increment it
again. Neither API recovers a worker; retirement is the only valid response.

The test-only cleanup surface uses these stable boundary values:

| Value | Boundary | Injected path |
| ---: | --- | --- |
| 1 | `MAKO_LOCAL_CLEANUP_BOUNDARY_BEGIN` | After native begin startup, force the exceptional begin-cleanup path. |
| 2 | `MAKO_LOCAL_CLEANUP_BOUNDARY_OPERATION` | Cleanup after a terminal point or scan operation. |
| 3 | `MAKO_LOCAL_CLEANUP_BOUNDARY_COMMIT` | Cleanup performed inside native commit. |
| 4 | `MAKO_LOCAL_CLEANUP_BOUNDARY_ABORT` | Cleanup requested by explicit abort. |
| 5 | `MAKO_LOCAL_CLEANUP_BOUNDARY_DESTROY` | Abort cleanup initiated by destroying an active facade. Destroying an already-finished healthy facade performs no native cleanup and does not consume this arm. |

With `MAKO_LOCAL_FEATURE_TEST_CLEANUP_FAILURES`,
`mako_local_test_arm_cleanup_failure()` arms exactly one boundary in the
calling worker's TLS. The next matching boundary consumes the arm before
forcing `Transaction::stop()` to throw at entry, so no test relies on an
unknown partial-cleanup position. A different boundary does not consume it;
a second arm is `BUSY` until it is consumed or
`mako_local_test_clear_cleanup_failure()` clears it. Tests that trigger
quarantine must use a sacrificial OS worker and must not attempt to recover or
reuse its TLS state. Without the feature, both control functions are
`FEATURE_UNAVAILABLE` stubs and production cleanup paths contain no failpoint
branch.

The C header is the declaration source of truth. Pinned bindgen generation
produces all raw Rust constants, layouts, callbacks, and function declarations;
a checked inventory rejects an unexpected addition or omission. Strict C11 and
C++ conformance translation units check every constant, layout, callback,
signature, and C++ `noexcept` type. A Rust integration probe forces a link
reference to every export, while the native symbol gate requires the exact
allowlist plus one strong digest-named build anchor. These checks are mandatory
in the required-native CMake/Cargo gate, and CMake configuration fails rather
than silently skipping Rust when its test profile cannot find Cargo.

Status identity is mechanically synchronized as part of that declaration
pipeline. The C header's canonical manifest generates C++ diagnostics and the
raw Rust `KnownStatus` catalog; the safe Rust layer exhaustively classifies that
generated enum for ordinary operation lifecycle and commit disposition.
Required-native ABI verification also compares every linked diagnostic with
the manifest. The per-operation matrix in this document remains the normative
explanation of when each status is permitted and how ownership changes.

The safe Rust ownership layer is also executed against a fake ABI without C++.
That suite covers every active, finished, quarantined, and destroyed transition;
unknown terminal statuses; one-shot abort and destroy behavior; commit
disposition independent of cleanup; and malformed successful point and scan
outputs before any reported native length is trusted. This Rust-only suite
passes under Miri as the ownership and output-validation gate. Native failpoint
tests are separate and prove that the real C++ boundary produces the same
quarantine and diagnostic behavior.

The fixed-worker async adapter that removes a poisoned worker from service is
still deferred. The typed status, health query, counter, begin-cleanup anchor,
and test coverage make that policy implementable without weakening the current
thread-affine API. This document does not claim that the rest of Phase 1C is
complete or that the surface is ready for promotion to ABI v1.
