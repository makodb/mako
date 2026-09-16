# Dead and repeated code in src/deptran/raft/server.{h,cc}

Measured 2026-09-15 against `cb0c4c207`. Four independent sweeps, each
enumerating a full population before reporting its zero-use subset; every
claimed-dead item was then re-checked by a separate pass that tried to find a
use. 17 of 17 survived — no false positives.

Files: `server.h` 2862 lines, `server.cc` 5298 lines, **8160 total**.

Sweep totals must NOT be summed: they overlap heavily (all four found the
commented-out bootstrap at `server.cc:4550`). The figures below are
deduplicated.

## Totals

| category | items | lines |
|---|---|---|
| dead functions | 11 | ~160 |
| dead DSL predicates (zero callers) | 9 | ~85 |
| dead preprocessor arms | 12 sites | ~80 |
| commented-out code | 9 sites | ~40 |
| dead data members | 6 | 6 |
| **dead subtotal** | | **~371** |
| duplicated logic (removable by factoring) | 11 clusters / 44 sites | ~122 |
| **total** | | **~493 (6.0% of the file pair)** |

## Dead data members (zero reads AND zero writes)

`init_` (h:2168), `filename` (h:2191), `status_` (h:2217, plus its unused
STOPPED/RUNNING enumerators), `max_executed_slot_` (h:2458),
`max_committed_slot_` (h:2459), `logs_` (h:2460).

Adjacent but distinct:
- **write-only**: `n_vote_` (h:2461) is incremented at h:2337 and read nowhere.
- **never written**, so constant by construction: `heartbeat_` (h:2196),
  `wait_int_` (h:2173), `failover_` (h:2187). `failover_` is documented as
  deliberately unwired; the other two are not.

## Dead functions (11, ~160 lines)

`applyLogs` (72 lines — by far the largest single dead item), `removeCmd` (13),
`GetQuorumSize` (13), `GetCurrentConfig` (12), `CreateSnapshot` (11),
`GetLastLogIndex` (8), `GetInstance` (8), `RequestVote` (7), `randDuration`
(7), `GetCurrentConfigSnapshot` (7), `timer_thread` (2 — declared at h:2145
with no definition anywhere).

## Dead DSL predicates (9, ~85 lines of Rust + generated C++)

`raft_server_compaction_index_clamp`, `raft_server_random_range_cap`,
`raft_server_election_in_startup_grace`,
`raft_server_append_index_is_accepted`,
`raft_server_leadership_transition_*` (two),
`raft_server_random_range_needs_swap`,
`raft_server_random_range_is_single`,
`raft_server_snapshot_index_is_available`.

A further 12 predicates are exercised only by their own `static_assert`s. Those
are not dead — the assertion is a real compile-time test — but the family has
grown faster than its call sites.

## Dead preprocessor arms (~80 lines)

- `RAFT_LEADER_ELECTION_DEBUG`: 9 sites, 38 lines, never defined.
- `RAFT_BATCH_OPTIMIZATION` unselected arms: `server.cc:3494-3526` (33),
  `4775-4780` (6), `4677-4680` (3).

## Commented-out code (9 sites, ~40 lines)

Largest: `server.cc:4550-4567` (18 lines — a heartbeat/election bootstrap
superseded by the live code in `Setup` at 1681-1717) and `server.h:2594-2602`
(9 lines — an old vector-based `GetRaftInstance` whose live replacement sits
three lines below). Then a follower-view block (`server.cc:4754-4759`), a
rand()-based latency fault injector (`4897-4899`), two debug busy-loop
remnants, a disabled assertion, a commented `#include`, and a commented
`raft_logs_` declaration.

## Duplicated logic (11 clusters, 44 sites, ~122 lines)

Ranked by what factoring would save:

1. **`server.cc:1681-1717` — the `#ifdef RAFT_TEST_CORO` and `#ifndef` arms are
   character-identical**, 20 lines duplicated for no difference. Same pattern
   at `server.h:2178-2190`, where `failover_{true}` is declared identically in
   both arms.
2. **"halt the server" atomic-store idiom**, 8-15 sites, ~21 lines — the same
   `rpc_ready_/stop_/looping_` store triple.
3. **ReplicationWakeGate's two waiter routines** (`server.cc:279`), ~16-20
   lines of parallel structure between the heartbeat and election waiters.
4. **"demote to follower" idiom** (stepDown + cancel), 4 sites, ~18 lines.
5. **peer table rebuild** (`server.cc:1815` and the heartbeat prologue), 2
   sites, ~8-10 lines — introduced by the PeerTable change in `99765a06f`;
   mine to factor.
6. Apply-thread callback recovery tail, snapshot-recovery gap predicate,
   election-failure tail, next_index wrap-repair ternary, AppendEntries
   payload-assign triple, `QueueReplicationWake` vs its shutdown twin — 2 sites
   each, 4-13 lines.

## Reading

~6% is not alarming for a file of this age, and the distribution is
informative: the single biggest item (`applyLogs`, 72 lines) is one function,
and the biggest duplication (20 identical lines in two `#ifdef` arms) is one
edit. Roughly half the total is concentrated in about eight places.

The dead DSL predicates are the one category that will keep growing on its own,
because each conversion adds predicates and only some get callers.
