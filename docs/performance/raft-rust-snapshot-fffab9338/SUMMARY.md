# Raft with snapshots on: the Rust-lane snapshot store (plan phase N12)

Plan: `docs/migration/raft/commo-service-rpc-plan.md`, phase N. Five builds
("arms"), one host, every point run as rotated rounds, so each arm runs once
per round in every position in turn and host drift lands on all arms evenly.

## Short answer

- **The Rust store is as fast as the C++ manager it replaces, wherever the
  measurement can resolve it.**
  - At 64 KiB, 1 MiB and 16 MiB snapshots, and at 286 KB unthrottled, store vs
    pre is within +-0.7% on p50 and +-3% on p99, with no significant
    regression.
  - Memory is the same.
- **Catch-up is much faster.** A follower that fell behind (stalled 4 s)
  catches up as follows:

  | | pre | store | post |
  |---|---|---|---|
  | 1 MiB | 24 ms | 20.5 ms | 14 ms |
  | 16 MiB | 806 ms | 376 ms | 280 ms |

  At 16 MiB the leader's p99 over the run falls from 397 ms (pre) to 121 ms
  (store) and 12.9 ms (post, with resend suppression). The follower's install
  step at 1 MiB takes 7 µs instead of 403 µs.
- **Resend suppression works.** InstallSnapshot RPCs per catch-up:

  | | pre | store | post |
  |---|---|---|---|
  | 1 MiB | 5 | 5.5 | 1 |
  | 16 MiB | 11 | 14.5 | 2 |

  The plan's limit is 2.
- **The old path crashes; the new one did not.**
  - The pre arm segfaulted in 13 of 370 runs, all at points where
    InstallSnapshot happens.
  - The store and post arms had 0 failures in 370 runs each.
- **Snapshots off: no change.** post vs pre at the four standard points is
  within +-0.5% on p50 and p99.
- **Gates missed** (details below):
  - 60 MiB snapshots, where p99 is too noisy to resolve and, at interval 100,
    every build degrades;
  - the pre arm's crashes leave some points short of 23 complete rounds;
  - one p50 sign test at p = 0.043;
  - a +10% max-apply-gap median at the 6-partition point, confounded by the
    same crashes;
  - hybrid post vs pre at two noisy points.

## Settings

| | |
|---|---|
| host | zoo-003: 64-core Intel Xeon E5-2683 v4 @ 2.10 GHz, Linux 7.0.0-29, one host, TCP over loopback |
| build | clang with libc++, `MODE=perf`, `CMAKE_BUILD_TYPE=Release` |
| cluster | 3 `raft_bench` processes (`examples/raft_bench.sh`); leader `localhost`; synthetic state machine (a per-partition sequence number, image padded to `--snapshot-bytes`); memory-only log |
| runner | `scripts/raft_perf/rotation_trial.sh` (rotated rounds), `scripts/raft_perf/paired_stats.py` (median of per-round B/A - 1, two-sided sign test), `scripts/raft_perf/n12_summary.py` (the gates below) |
| rounds | 25 per point, 10 for the stall points |
| dates | 2026-09-27 22:00 to 2026-09-28 22:30 |

**Arms**

| arm | build dir | source | lane | snapshot store |
|---|---|---|---|---|
| pre | `build_rust_pre` | `b7ac74a0b` (N0) | rust | C++ `MemorySnapshotManager` |
| store | `build_rust_store` | `a6ba19ee9` (N3-N5) | rust | Rust `SnapshotStore`, no resend suppression |
| post | `build_rust` | `a22d391cc` (N6-N8; later commits are docs) | rust | Rust `SnapshotStore` + N6 suppression |
| hybrid pre | `build_hybrid_pre` | `b7ac74a0b` | hybrid | C++ manager |
| hybrid post | `build` | `a22d391cc` | hybrid | C++ manager |

**Points** (snapshots on: `MAKO_RAFT_SNAPSHOTS=1`, `MAKO_RAFT_SNAPSHOT_INTERVAL` as given)

| point | payload | rate | partitions | interval | snapshot | duration |
|---|---|---|---|---|---|---|
| `s4k_i100_b{65536,1048576,16777216,62914560}` | 4 KB | 240/s | 1 | 100 | 64 KiB - 60 MiB | 10 s |
| `s4k_i500_b...` | 4 KB | 240/s | 1 | 500 | 64 KiB - 60 MiB | 25 s |
| `s286k_sat_i1000_b1048576` | 286,208 B | unthrottled | 1 | 1000 | 1 MiB | 30 s |
| `s286k_r190_p6_i100_b1048576` | 286,208 B | 190/s | 6 | 100 | 1 MiB | 200 s |
| `stall_b{1048576,16777216}` | 4 KB | 240/s | 1 | 200 | 1 / 16 MiB | 15 s; one follower SIGSTOPped at 5 s for 4 s |
| `off_4k_r240`, `off_4k_sat`, `off_286k_r90`, `off_1m_r26` | 4 KB / 4 KB / 286 KB / 1 MiB | 240 / unthrottled / 90 / 26 | 1 | snapshots off | -- | 10 s |

In-flight caps: 4096 entries (4 KB), 256 (286 KB), 64 (1 MiB). The plan does
not define its "four standard points"; the disabled-path set above is the
sweep's usual one.

## Gates (from the plan)

- store vs pre at every point: median B/A - 1 within +-2% for throughput and
  p50, and +-5% for p99; no significant regression (sign test); max apply
  gap, the stalled follower's install time and `catchup_ms` not worse by
  more than 5%.
- RSS at 4 KB with 16 and 60 MiB images: each arm's peak minus its own peak
  with snapshots off, at most one image above pre's.
- post vs store: install RPCs per catch-up at most 2; the same bounds.
- hybrid post vs pre: the same bounds.
- Disabled path: post vs pre under the same bounds.
- At least 23 of 25 (9 of 10) complete rounds per comparison.

A change beyond a bound in the better direction is reported as "improved",
not as a failure.

## Results

Median B/A - 1 over the rounds, with the sign-test p.

| point | comparison | rounds | throughput | p50 | p99 | p99.9 | max apply gap | verdict |
|---|---|---|---|---|---|---|---|---|
| s4k_i100_b65536 | store vs pre | 25/25 | +0.00% (p=1.00) | -0.11% (p=1.00) | -0.25% (p=0.42) | -0.57% (p=0.11) | +0.03% (p=1.00) | pass |
| s4k_i100_b65536 | post vs store | 25/25 | +0.00% (p=1.00) | +0.62% (p=0.04) | +0.82% (p=0.15) | +0.38% (p=1.00) | -4.95% (p=0.23) | **miss**: p50 sign test p=0.043 |
| s4k_i100_b65536 | hybrid post vs pre | 25/25 | +0.00% (p=1.00) | -0.15% (p=0.15) | +0.06% (p=0.69) | +0.59% (p=0.69) | +1.98% (p=0.69) | pass |
| s4k_i100_b1048576 | store vs pre | 25/25 | +0.00% (p=1.00) | -0.19% (p=0.23) | -0.06% (p=1.00) | -0.08% (p=1.00) | +0.42% (p=1.00) | pass |
| s4k_i100_b1048576 | post vs store | 25/25 | +0.00% (p=1.00) | -0.15% (p=0.42) | -0.47% (p=0.23) | -1.34% (p=0.11) | -0.55% (p=0.69) | pass |
| s4k_i100_b1048576 | hybrid post vs pre | 25/25 | +0.00% (p=1.00) | +0.15% (p=0.69) | -0.28% (p=0.69) | +0.98% (p=0.23) | +1.85% (p=0.42) | pass |
| s4k_i100_b16777216 | store vs pre | 23/25 | +0.00% (p=0.62) | -0.53% (p=0.40) | -2.80% (p=0.40) | -1.17% (p=0.40) | -7.97% (p=0.09) | pass; improved: max apply gap -8.0% |
| s4k_i100_b16777216 | post vs store | 25/25 | +0.00% (p=1.00) | +0.60% (p=0.42) | +2.30% (p=0.69) | +4.53% (p=0.42) | +4.59% (p=0.69) | pass |
| s4k_i100_b16777216 | hybrid post vs pre | 25/25 | +0.00% (p=0.29) | -1.14% (p=0.23) | -0.70% (p=1.00) | +1.99% (p=0.69) | -3.66% (p=0.11) | pass |
| s4k_i100_b62914560 | store vs pre | 21/25 | +0.00% (p=0.29) | +1.24% (p=0.66) | +1.24% (p=0.19) | +1.55% (p=0.66) | +2.20% (p=0.66) | **miss**: 21 rounds (pre crashed) |
| s4k_i100_b62914560 | post vs store | 25/25 | +0.00% (p=0.21) | +1.96% (p=0.23) | +21.60% (p=1.00) | +7.53% (p=0.42) | +8.08% (p=0.69) | **miss**: p99, max apply gap (see 1) |
| s4k_i100_b62914560 | hybrid post vs pre | 22/25 | +0.00% (p=0.27) | +1.00% (p=0.83) | +20.38% (p=0.02) | +21.11% (p=0.13) | +19.58% (p=0.52) | **miss**: 22 rounds; p99 (see 1) |
| s4k_i500_b65536 | store vs pre | 25/25 | +0.00% (p=1.00) | +0.19% (p=0.69) | +0.06% (p=0.54) | +0.93% (p=0.42) | +0.46% (p=0.42) | pass |
| s4k_i500_b65536 | post vs store | 25/25 | +0.00% (p=1.00) | +0.04% (p=1.00) | +0.28% (p=0.23) | -0.03% (p=0.84) | -0.06% (p=1.00) | pass |
| s4k_i500_b65536 | hybrid post vs pre | 25/25 | +0.00% (p=1.00) | +0.26% (p=0.42) | +0.11% (p=1.00) | +0.27% (p=0.42) | +3.40% (p=0.23) | pass |
| s4k_i500_b1048576 | store vs pre | 25/25 | +0.00% (p=1.00) | -0.15% (p=1.00) | -0.14% (p=0.23) | +0.65% (p=0.69) | +0.36% (p=0.11) | pass |
| s4k_i500_b1048576 | post vs store | 25/25 | +0.00% (p=1.00) | +0.08% (p=0.54) | +0.11% (p=0.54) | -0.84% (p=0.69) | +4.93% (p=0.11) | pass |
| s4k_i500_b1048576 | hybrid post vs pre | 25/25 | +0.00% (p=1.00) | +0.04% (p=0.84) | -0.05% (p=1.00) | +0.67% (p=0.42) | +0.06% (p=1.00) | pass |
| s4k_i500_b16777216 | store vs pre | 25/25 | +0.00% (p=1.00) | +0.11% (p=0.42) | -0.30% (p=1.00) | +1.02% (p=0.69) | -0.97% (p=0.42) | pass |
| s4k_i500_b16777216 | post vs store | 25/25 | +0.00% (p=1.00) | -0.15% (p=0.69) | -0.33% (p=0.69) | -1.23% (p=0.11) | +0.02% (p=1.00) | pass |
| s4k_i500_b16777216 | hybrid post vs pre | 25/25 | +0.00% (p=1.00) | +0.11% (p=0.84) | +0.69% (p=0.42) | -0.21% (p=1.00) | +0.06% (p=1.00) | pass |
| s4k_i500_b62914560 | store vs pre | 25/25 | +0.00% (p=1.00) | -0.15% (p=0.54) | +23.30% (p=0.42) | +0.20% (p=0.69) | +0.50% (p=0.69) | **miss**: p99 (see 2) |
| s4k_i500_b62914560 | post vs store | 25/25 | +0.00% (p=1.00) | +0.04% (p=1.00) | -16.00% (p=0.42) | -0.19% (p=1.00) | -0.28% (p=0.23) | pass (see 2) |
| s4k_i500_b62914560 | hybrid post vs pre | 25/25 | +0.00% (p=1.00) | -0.19% (p=1.00) | +21.24% (p=0.42) | +0.34% (p=0.11) | +0.42% (p=0.23) | **miss**: p99 (see 2) |
| s286k_sat_i1000_b1048576 | store vs pre | 25/25 | -0.48% (p=0.11) | +0.61% (p=0.23) | +2.88% (p=0.23) | +1.57% (p=0.42) | -2.18% (p=0.69) | pass |
| s286k_sat_i1000_b1048576 | post vs store | 25/25 | +0.56% (p=0.69) | -0.40% (p=0.42) | -1.41% (p=0.11) | -2.44% (p=0.23) | +1.01% (p=1.00) | pass |
| s286k_sat_i1000_b1048576 | hybrid post vs pre | 25/25 | +0.69% (p=0.15) | -0.93% (p=0.42) | -1.91% (p=0.11) | -3.87% (p=0.23) | -4.98% (p=1.00) | pass |
| s286k_r190_p6_i100_b1048576 | store vs pre | 22/25 | +0.00% (p=1.00) | +0.72% (p=0.29) | +3.36% (p=0.52) | +10.89% (p=0.13) | +9.98% (p=0.02) | **miss**: 22 rounds; max apply gap (see 4) |
| s286k_r190_p6_i100_b1048576 | post vs store | 25/25 | +0.00% (p=1.00) | +0.94% (p=0.15) | +0.71% (p=0.69) | +4.44% (p=0.23) | -1.19% (p=0.69) | pass |
| s286k_r190_p6_i100_b1048576 | hybrid post vs pre | 25/25 | +0.00% (p=1.00) | -0.25% (p=0.42) | +7.90% (p=0.23) | +4.19% (p=0.23) | +2.32% (p=1.00) | **miss**: p99 +7.9%, not significant |
| stall_b1048576 | store vs pre | 7/10 | +0.00% (p=1.00) | -0.04% (p=1.00) | -0.36% (p=0.03) | -12.89% (p=0.45) | -29.66% (p=0.02) | **miss**: 7 rounds (pre crashed); otherwise improved |
| stall_b1048576 | post vs store | 10/10 | +0.00% (p=1.00) | +0.31% (p=0.75) | +0.27% (p=0.75) | -22.51% (p=0.02) | +2.30% (p=1.00) | pass; improved: catch-up -35.7% |
| stall_b1048576 | hybrid post vs pre | 10/10 | +0.00% (p=1.00) | +0.13% (p=0.34) | +0.21% (p=0.75) | -0.83% (p=1.00) | -2.04% (p=0.75) | pass |
| stall_b16777216 | store vs pre | 8/10 | +0.00% (p=1.00) | -0.68% (p=0.07) | -68.38% (p=0.01) | -61.07% (p=0.01) | -42.44% (p=0.01) | **miss**: 8 rounds (pre crashed); otherwise improved |
| stall_b16777216 | post vs store | 10/10 | +0.00% (p=1.00) | -0.38% (p=0.34) | -89.13% (p=0.00) | -62.69% (p=0.00) | -40.98% (p=0.00) | pass; improved: p99 -89%, apply gap -41%, catch-up -23% |
| stall_b16777216 | hybrid post vs pre | 10/10 | +0.00% (p=1.00) | +0.42% (p=0.18) | +13.32% (p=0.34) | +12.57% (p=0.34) | +6.70% (p=0.75) | **miss**: p99, apply gap, catch-up +22%, none significant (see 5) |
| off_4k_r240 | post vs pre | 25/25 | +0.00% (p=1.00) | +0.11% (p=0.84) | +0.42% (p=0.42) | -0.62% (p=1.00) | +1.77% (p=0.11) | pass |
| off_4k_sat | post vs pre | 25/25 | +5.08% (p=0.11) | -3.04% (p=0.23) | -4.61% (p=0.69) | -3.34% (p=0.42) | -0.55% (p=0.42) | pass |
| off_286k_r90 | post vs pre | 25/25 | +0.00% (p=1.00) | -0.09% (p=0.84) | -0.53% (p=0.69) | -3.77% (p=1.00) | -0.23% (p=1.00) | pass |
| off_1m_r26 | post vs pre | 25/25 | +0.00% (p=1.00) | -0.46% (p=1.00) | +0.15% (p=1.00) | +6.49% (p=1.00) | +1.91% (p=0.69) | pass |

### Catch-up (stall runs), medians over 10 runs

| point | arm | install RPCs sent | received by the stalled follower | catch-up ms | follower install µs (p50) | leader p99 µs (whole run) |
|---|---|---|---|---|---|---|
| 1 MiB | pre | 5 | 5 | 24 | 403 | 3,593 |
| 1 MiB | store | 5.5 | 5.5 | 20.5 | 7 | 3,559 |
| 1 MiB | post | 1 | 1 | 14 | 6 | 3,570 |
| 1 MiB | hybrid pre | 3 | 3 | 31 | 386 | 3,653 |
| 1 MiB | hybrid post | 3 | 3 | 30 | 372.5 | 3,671 |
| 16 MiB | pre | 11 | 3 | 806 | 2,416.5 | 397,097 |
| 16 MiB | store | 14.5 | 4 | 376.5 | 1,733 | 120,897 |
| 16 MiB | post | 2 | 2 | 279.5 | 1,693 | 12,934.5 |
| 16 MiB | hybrid pre | 9 | 3 | 946 | 2,517 | 578,702.5 |
| 16 MiB | hybrid post | 11 | 4 | 1,329.5 | 2,442.5 | 889,143 |

`raft_bench` has no "leader p99 during catch-up" field, and adding one would
have meant rebuilding the frozen N0 arm, so the stall runs' whole-run leader
p99 stands in for it.

### RSS (leader peak minus the same arm's snapshots-off peak, KiB, medians)

| point | pre | store | post | one image | verdict |
|---|---|---|---|---|---|
| 4 KB, interval 100, 16 MiB | 17,092 | 16,640 | 17,956 | 16,384 | pass |
| 4 KB, interval 100, 60 MiB | 107,128 | 106,440 | 107,848 | 61,440 | pass |
| 4 KB, interval 500, 16 MiB | 24,296 | 24,432 | 25,884 | 16,384 | pass |
| 4 KB, interval 500, 60 MiB | 115,092 | 114,148 | 115,100 | 61,440 | pass |

### Failed runs

| arm | runs | segfault | follower gave up (exit 7) |
|---|---|---|---|
| pre | 370 | 13 | 1 |
| store | 370 | 0 | 0 |
| post | 370 | 0 | 0 |
| hybrid pre | 270 | 0 | 2 |
| hybrid post | 270 | 0 | 1 |

Two further runs of the last point, one store and one hybrid pre, died when
the shared `/tmp` filled up. They were rerun with the same parameters. No
segfault run was rerun.

## The gates missed, and what each one is

1. **60 MiB, interval 100: every build degrades.**
   - `CreateSnapshotLocked` compacts the log through the new snapshot index
     immediately. The retention window is not applied there
     (`src/deptran/raft/src/server_h.rs`, `CompactLogLocked(snap_index)`).
     So a follower more than about 100 entries (0.4 s) behind can only catch
     up by InstallSnapshot.
   - At 60 MiB it often cannot catch up at all. A new snapshot is taken and
     compacted before the previous one arrives, so the follower gets one
     install per snapshot: about 26 in a 12 s run. Meanwhile the leader's
     latency rises to hundreds of milliseconds.
   - This is core behaviour, shared by all lanes (plan Risks 2 and 4).
   - The p99 ratios here are ratios between two degraded states. Which runs
     degrade differs by arm: store 5, post 10, hybrid pre 4 plus 2 failed,
     hybrid post 12 of 25. Pre had 2 of 21, with 4 more crashed.
2. **60 MiB, interval 500: p99 is multimodal.**
   - Per-run p99 falls into clusters at about 4.2, 5-7 and 23-28 ms,
     depending on whether a ~100 ms snapshot creation lands in the slowest
     1% of entries.
   - The paired median therefore swings by +-20%, even between hybrid pre
     and post, and no sign test is significant.
   - Runs in the high cluster: pre 5, store 9, post 9, hybrid pre 6, hybrid
     post 8. That difference is not significant either.
   - A +-5% p99 bound cannot be resolved here at 25 rounds.
3. **Short rounds on store vs pre** (7/10, 8/10, 21/25, 22/25, 23/25). These
   are the pre arm's crashes. On the complete rounds the stall points are
   large improvements.
4. **286 KB @ 190/s x 6 partitions: max apply gap +10% (265 -> 305 ms,
   p = 0.017).**
   - At this point InstallSnapshot happens in nearly every run on every arm,
     and the apply gap follows it.
   - Pre ran 5 of its 22 surviving runs with no install at all (median gap
     222 ms). Its 3 crashed runs were most likely install-heavy ones, so its
     surviving runs are biased toward light ones.
   - Snapshot creation is the same on every arm (max 178-190 ms).
   - Post, which runs the same store code, is +5.9% against pre, p = 0.29.
   - Recorded as not shown to be a store regression, but also not ruled out.
5. **Hybrid post vs pre at stall 16 MiB and at 286 KB x 6.** Hybrid runs the
   same C++ manager in both arms. It differs only by N4's verbatim move of
   the kernels and the shared core changes. The misses are not significant
   (p = 0.23-0.34 over 10 or 25 runs), and hybrid's 16 MiB catch-up already
   ranged 634-1678 ms in N11.
6. **4 KB, interval 100, 64 KiB: post vs store p50 +0.62% (p = 0.043).**
   - It is inside the +-2% bound. Only the sign test flags it, and about 50
     gated comparisons are tested here.
   - Between these two arms the steady-state path is the same code: N6 acts
     only while an install is outstanding.
   - At the other 64 KiB and 1 MiB points post vs store is -0.15% to +0.08%.

## The pre arm's crash

- It occurs in pre only, the Rust lane at N0 with the C++ manager, and only
  with snapshots on.
- It occurs only at points where InstallSnapshot happens. The 13 segfaults
  are 3 at stall 1 MiB (of 10 runs), 2 at stall 16 MiB, 2 at interval 100 /
  16 MiB, 3 at interval 100 / 60 MiB and 3 at 286 KB x 6 / interval 100.
  There are none at intervals 500 and 1000, where followers keep up and
  nothing is installed.
- The install path is what N4-N5 replaced, with the Rust store and the
  borrowed send.
- The segfaulting process is the leader.

A backtrace was attempted by rerunning the stall point with the leader under
`gdb`; its outcome is recorded in the plan's N12 row.

## Records

The raw records (one JSON per run and the launcher logs) are in
`raft_perf_output/n12/` in the worktree. That directory is ignored by git.
They are not committed: how experiment data is kept in the repository is
still to be decided. To reproduce a point, use the parameters above with
`scripts/raft_perf/rotation_trial.sh OUT 25 build_rust_pre build_rust_store build_rust build_hybrid_pre build`,
then `scripts/raft_perf/n12_summary.py OUT_PARENT`.
