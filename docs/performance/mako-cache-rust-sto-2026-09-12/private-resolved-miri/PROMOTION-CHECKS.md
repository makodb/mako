# Private candidate qualification preparation

This is preparation for a possible promotion. It is not a production acceptance
report. The measured source is the frozen private `cache-prefetch` tree at
`/var/tmp/mako-resolved-health.lt7kUf/cache-prefetch`.

## Existing driver coverage

The existing focused sanitizer driver selects only `local_version_tests::` for
the sto-masstree library. Add a required six-name inventory before relying on it
for this candidate. Each name below belongs to `sto-masstree --all-features --lib`.

- `tests::put_resolving_cold_lookup_once_and_hits_keep_current_occ_state`
- `tests::put_resolving_publication_abort_and_foreign_tokens`
- `tests::put_resolving_keeps_own_publication_and_foreign_phantom_accounting`
- `tests::put_resolving_health_rejects_poisoned_table_in_both_token_modes`
- `tests::put_resolving_checked_health_failure_poisons_table_and_core_before_staging`
- `tests::put_resolving_checked_healthy_commit_abort_and_tombstone_both_policies`

All six passed exact pinned Miri runs with isolation and leak checking enabled.
The two poison-routing fixtures do not leak under Miri. They need no new shared
quarantine entries. The existing full sto-masstree Miri library selection already
includes all six. Keep the native local-version suite, fixed-u64 admission test,
and existing exact indeterminate test unchanged.

The engine sanitizer profile already runs the complete native engine library.
Its fail-closed listing will discover these eight additional tests in the
combined arm without expanding its target selection.

- `resolved_cache::tests::allocation_failure_disables_and_storage_cannot_grow`
- `prefetch::tests::hint_does_not_allocate_a_stamp_or_change_the_gate`
- `txn::resolved_cache_tests::resolved_cache_checked_hit_routes_injected_tree_health_to_table_and_core`
- `txn::resolved_cache_tests::resolved_cache_checked_warm_tombstone_scan_and_new_db_epoch`
- `txn::resolved_cache_tests::resolved_cache_collisions_mixed_tables_and_runtime_identity_are_exact`
- `txn::resolved_cache_tests::resolved_cache_health_core_poison_allows_no_successful_terminal`
- `txn::resolved_cache_tests::resolved_cache_hit_abort_repeated_write_delete_reinsert_and_bypass`
- `txn::resolved_cache_tests::resolved_cache_hit_does_not_hide_another_workers_commit`

The existing fused success test now warms a constant cached key and alternates
forgetting and dropping the consumed facade. The existing mixed local/ordered
version test also exercises warmed hits. Both remain required ordinary cases.

## Exact native failure classification

Add only this definite new intentional-retention case to the focused engine
ASan quarantine list before running that gate:

`txn::resolved_cache_tests::resolved_cache_health_core_poison_allows_no_successful_terminal`

It triggers an actual other-worker core cleanup panic. It is excluded from Miri
by `not(miri)`, so adding it to the shared Miri quarantine inventory would make
that inventory fail. Keep its exact ASan rerun separate with only leak checking
disabled. TSan and UBSan should run it normally.

The Tree-health injection case poisons the table and core before staging but
does not create an uncertain mutation payload. Start with ordinary leak checks
and the existing two reviewed process-lifetime allocation-root exceptions. Do
not preemptively add another suppression. Healthy native fixtures explicitly
drop their idle worker attachments, including the fixed cache allocation.

The real global-native-poison proof remains a separate native-only test archive
and subprocess qualification at `/var/tmp/mako-native-health-proof.SMAbQK`.
It is not part of ordinary `--all-features`, Miri, or production timing binaries.
Any promotion must preserve that separation and verify helper symbols are absent
from production archives and release executables.

## Miri fixture issue and source distinction

The frozen combined engine library lists ten native-free tests. Its original
full Miri invocation stops in the prefetch fixture because timestamp allocation
queries realtime under Miri isolation. `engine-native-free.log` preserves this
failure. It does not report undefined behavior or a leak.

The other nine tests passed separately with the exact prefetch test excluded.
`engine-remaining-report.json` records that selection and its result. No Miri
leak or isolation option was disabled.

At the parent's direction, `fixture/` is a separate source copy with only the
prefetch test changed. `prefetch-fixture-only.patch` seeds stamp42 through
`Clock::observe_stamp`, preserving zero/nonzero stamp, free/held gate, and dense
sequence checks without a realtime query. The patch passes `git apply --check`
against the frozen source. `engine-seeded-fixture-report.json` records the full
ten-test passing run against this copy, all source hashes, and the one-file
difference. All225 source inputs stayed unchanged during each run. No measured
source or binary was changed.

The prefetch instruction is deliberately a no-op under Miri. A Miri pass checks
the fixture and surrounding Rust ownership, not hardware execution. The native
engine library must still execute the hint state-preservation test with its
runtime CPU guard enabled.

## Promotion work still required

Before promotion, apply any approved fixture-only fix and format the final
source, then create a new source/native manifest. Do not refresh prior reports
to imply that their binaries used this candidate. Run the expanded focused
ASan/TSan/UBSan gates, the isolated Miri library checks, the full native facade
suite, cache functional suites and cross-engine reopen with that exact source.
Retain the existing lost-TouchLog LSan negative control. No new blanket
suppression is justified by the resolved cache's32KiB per-attachment storage.

Useful targeted mutation checks are restoring the unchecked hit, removing
own-publication accounting, removing exact key/table identity validation, and
reusing a stale OCC observation. The new routing, phantom, collision, and
cross-worker tests provide deterministic expected failures. This document does
not claim those new mutations have been executed.

Memory remains bounded per WorkerSlot, not across unlimited historical DB
epochs. The slot retains512 cache entries and32KiB of logical Vec capacity,
excluding allocator overhead. Existing process-lifetime worker retention means
a later DB epoch can allocate another slot and cache.
