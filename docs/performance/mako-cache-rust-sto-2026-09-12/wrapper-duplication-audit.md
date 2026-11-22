# Facade bookkeeping audit after raw attribution

This is a read-only source audit. No additional optimization was implemented. The raw diagnostic is not a replacement engine API.

The same-binary raw/facade comparison reported W1 throughput of 1.070602/1.331939 Mtx/s and W4 throughput of 4.040061/4.959284 Mtx/s. W1 retired instructions fell from 4298.479 to 3516.505, about 782 instructions per commit. Raw mode omitted facade attachment lookup, limits, canonical logging state, witnesses and report translation together. It retained the actual core transaction, ordinary native lookup, owned table PUT and matching table/runtime policies. This measures their aggregate cost; it does not assign the difference to frame movement or payload copying.

## Required state versus duplicate representation

The bounded 128-byte PUT input becomes a core-owned `ValueRepr::Staged` box in `sto-masstree/src/lib.rs:288`. The presence-only path converts and stages it at line 3479. The facade then copies the same input into the pooled canonical value at `mako-engine-sto/src/txn.rs:498`. The second copy is real, even when the caller later chooses an unlogged commit.

For trusted transactions, that second owner currently has a purpose. Record preflight can be chosen after the operations at `txn.rs:660`. The holder path transfers the canonical value after the core terminal has completed at lines 1149 and 1317. Its lifetime is independent of core cleanup and quarantine. Removing that independence would reopen the borrowed-staging failure and unwind problems from the rejected experiment.

An existing safe clone is not a zero-copy substitute. `Value::clone` at `sto-masstree/src/lib.rs:327` clones the bounded Staged box. `From<Vec<u8>>` at line 430 creates Shared storage instead, adding reference-counted ownership and changing the bounded staging representation. The generic core also does not expose an immutable pending-value iterator. Its public item callback at `sto-core/src/transaction.rs:1318` admits access under the transaction failure boundary; it is not a free inspection operation.

Canonical metadata cannot be replaced with a count of core writes. The facade keeps first-touch byte keys for record encoding, original presence to suppress insert-then-delete records at `txn.rs:400`, and read/insert/delete history for restricted timestamp admission at line 413. The core uses compact record identities and final intents. Those representations answer different questions.

## Small safe candidates

Two private follow-ups would avoid ownership redesign, but neither promises the full raw-mode gain:

- Replace the temporary `Vec<OpRef>` in general record preflight and general record terminal, at `txn.rs:670` and line 844, with a repeatable allocation-free iterator over the already frozen canonical log. Sizing must return the same checked byte count and operation count; encoding must traverse the same order and reject an extent mismatch. This removes two temporary operation-array constructions on that path. Fused one-put ACK already uses a stack array, so this is a general logged-path improvement, not an explanation of its W1 gap.
- Fuse first-touch initialization and successful PUT metadata in `touched.rs:38` and `txn.rs:498`. Recycling clears both buffers at `touched.rs:75`; first touch clears them again; PUT clears the value once more. A specialized safe helper can avoid redundant state resets while preserving the exact-key match, first presence, mutation count, witness retirement and bounded pool. Large prior transactions also make recycling visit every retained entry, even when only one is active. Any accounting shortcut must preserve the existing 64-entry/64-KiB logical retention bound and fused true-plus-forget/true-plus-Drop ownership tests.

The available record-local W1 engine profile assigns about 0.47% self cycles to `Transaction::touch` and 0.55% to `recycle_touched`. The larger PUT and Drop symbols include unrelated work. These figures came from `perf report --stdio --no-children --tid 333125 --percentage relative --sort symbol -g none` on the existing `profile-record-local/engine-w1/perf.data`. They use perf report's symbol denominator, include warmup and final verification, and predate the prefetch promotion. They do not establish a measured saving for either candidate.

One existing capability distinction permits a broader but straightforward omission: `LocalDb::transaction` at `db.rs:251` passes no trusted binding. Such a transaction cannot select record preflight, and cannot acquire a one-put witness. Its canonical payload could therefore be omitted without removing logging support from trusted transactions. This would help ordinary unlogged API use, not the current trusted-bound engine benchmark or cache ACK. Limits, database checks, native health admission, OCC and abort cleanup would still be required.

## Recommendation

Keep the current owned core and holder buffers. Do not introduce a pending-value ownership API, shared staging representation or boxed frame solely on the aggregate raw result. The iterator change is the clearest low-risk logged-path follow-up. Reset fusion has a small measured cost ceiling in the one-put workload and should remain a bounded experiment if pursued. Neither warrants a default-engine change; C ABI remains the default.
