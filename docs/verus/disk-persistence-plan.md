# Disk persistence for the Rust Raft: the work plan
**Status.** A plan; nothing is built. It implements
[disk-persistence.md](disk-persistence.md) ("the design", §3-§7). Code is cited
at `44d07a3ee` as `path:line`, under `src/deptran/raft/` unless a path starts
with `src/deptran/`, `src/mako/`, `src/srpc/`, `src/rusty-rustc/`, `bash/`,
`scripts/`, `ci/`, `examples/`, `docs/`, `config/` or `.github/`, or is a root
file (`CMakeLists.txt`, `Dockerfile.ubuntu24`, `apt_packages.sh`); `rocksdb/c.h`
is the RocksDB 9.11.2 header in `~/.local/mako-deps/usr/include/`. A bare
`:line` is in the file named last. Sizes are days of focused work.

**Milestones**, each leaving a working server. **A, crash-safe (P0-P6)**: every
step's changes reach a write-ahead log (WAL) before anything they produced
leaves; no segment is deleted, and recovery replays the whole WAL; the kill
tests pass. **B, bounded (P7)**: the RocksDB base, the applier, checkpoints that
delete old segments. **C, snapshots (P8)**: image files; until then a disk build
fails closed on a state that names a snapshot. **Size.** 34-53 days, 25-37 for
milestone A, whose critical path on two streams (P1 beside P2, P4 beside P5, the
harness beside both) is 14-21 days.

| Phase | What | Needs | Beside | Days |
|---|---|---|---|---|
| P0 | build switch, `raft-store` crate, runners and run directories, the verified list | - | - | 1-3 |
| P1 | core: persist note, `ObserveTerm`, `Restore` | P0 | P2 | 4-6 |
| P2 | `raft-store`: WAL, crash points, local check, atomic creation | P0 | P1 | 4-6 |
| P3 | shell: records, flusher | P1, P2 | - | 3-4 |
| P4 | shell: outputs wait for the WAL | P3 | P5 | 4-6 |
| P5 | shell: recovery | P1, P3 | P4 | 4-5 |
| P6 | process-kill tests | P2; P4, P5 to pass | harness from P0 | 5-7 |
| P7 | base and applier | P6 | - | 4-6 |
| P8 | snapshots as files | P7 | - | 3-5 |
| P9 | proofs and documents | P1, P8 | - | 2-5 |

## The user's decisions (2026-10-08)
**Only core steps write term and vote.** The InstallSnapshot reply path writes
`current_term_` and `vote_for_` itself (`src/server_h.rs:2268-2269`), then steps
`StepDown` (`:2281`), which keeps both (`core/src/node.rs:1311`, `:1317`,
`:1329`), so no persist note covers the raise; `OnInstallSnapshotLocked` does
the same (`src/server_h.rs:2414-2415`, then `:2438` or `:2440`). P1 adds
`Event::ObserveTerm { term, stopped, failover }` (row F20): a newer term is
raised, the vote and leader hint cleared, and the server steps down as
`SettleElection` does (`core/src/node.rs:887-898`); the proven `StepDown` arm
(`core/src/event.rs:516-533`) stays. The reply's taint (`src/server_h.rs:2260`)
narrows to its accept branch, which writes the volatile peer table outside
`step` (`:2299-2300`): no record, but a replay stops there. The snapshot paths'
writes (`:2561-2562`, `:2602`, `:2920-2921`) keep shell-built records.
`ObserveTerm` is false in `coupled` (`core/src/coupling.rs:2585-2613`): the
spec's step-down (`:614`, `:636`) needs a modeled message, so Verus checks
`inv()` only; it never steps under `MAKO_RAFT_VERIFIED_GATES=1`, which turns
snapshots off. The design's §2 (last bullet), §3 "Records" and Decision 4 say
this; its §3 and §4 must add `stopped` and `failover` to `ObserveTerm { term }`.

**A build switch selects disk mode** (§1): off is today's binary, the disk code
compiled out; environment variables only tune a disk build; the core has no
feature. **Persistence is tested by killing processes** (P6), each one Raft
server and its RPC server, at random and at named crash points, some simulating
a power cut; the seed's in-process restarts and lab cases from id 17 are
dropped, and the lab and Tier 1 run in disk builds as regression checks only.
**One departure from the design** (P4). The flusher sends the replies it made
durable, and the fibers poll: §3 ("Replies are held", "The flusher wakes the
waiters") and §4 (the wake job) change. Both were chosen when a poll-thread
job waited up to 1 ms; the Lion merge made that wake immediate and fiber
sleeps whole-millisecond timers (P4), so the fibers' half is open again
(§3, decision 6).

## The user's decisions (2026-10-09): store hygiene
Four additions, each placed in its phase below. **Stores live on the local
disk**, `/var/tmp/raft-wal-${USER}` (ext4 on `/dev/sda2`, 1.7 TB free), not on
tmpfs, which is RAM: on 2026-10-09 tmpfs leftovers starved the tests until the
kernel OOM killer took down three sessions. **A store must be on a local
filesystem**: the store refuses to open anywhere else, so it can never land in
the NFS home (P2). **Creation is atomic**: a store is built in a side
directory and renamed into place, so a crash while creating it leaves no
half-made store that blocks both kinds of relaunch (P2, P5). **Stale stores
are swept**: deletion at exit does not run when a launcher is killed, so every
launcher first deletes the run directories no live run holds (P0, P6).

## 1. Ground rules
**Branch.** `raft-disk` from `srpc-subtree-forward`, merged forward as that
moves and pushed to `backup`; PR #92 merges without it. **The switch.**
`option(MAKO_RAFT_DISK ... OFF)` sits beside `RAFT_TEST` (`CMakeLists.txt:463`)
and joins the feature list `RAFT_TEST` fills (`:1230-1233`) for the cargo build
(`:1250`) and the raft-rt stamp (`:1362`); `Cargo.toml:45-55` declares
`raft_disk`, `rt/Cargo.toml:32` forwards it. Shell code tests
`cfg!(feature = "raft_disk")` in expressions (as `src/server_h.rs:3614` tests
`raft_test`), so both builds type-check the disk path; `#[cfg]` marks only
feature-only items (as `:43-98`, `src/lib.rs:13-20`).

**Parameters.** `SetupInternal` reads three knobs (`src/server_h.rs:1856-1869`,
`:1882-1896`, `:1924-1932`) and `InitializeSnapshotManagerLocked` one
(`:2028-2039`), by `raft_env_u64` (`:1809-1839`) over the getenv kernel
`raft_env_lookup` (`server.cc:929-942`); `MAKO_RAFT_SNAPSHOTS` has its own
kernel (`:798-802`), five use `ParseEnvUint64OrDefault` (`:164-182`,
`:226-240`), the recorder `std::env::var` (`src/server_h.rs:3501-3516`). The new
ones use `std::env::var` after `:1896`, `raft_env_u64`'s digits-only parse and
`FailClosed` (`:2848`); no new kernel (`server.cc:922-928`).

| Variable | Meaning | Default |
|---|---|---|
| `MAKO_RAFT_DATA_DIR` | root of the stores; must be a local filesystem (P2) | `/var/tmp/raft-wal-${USER}` |
| `MAKO_RAFT_FLUSH_DELAY_US` | sleep after each `fdatasync` | 0 |
| `MAKO_RAFT_SEGMENT_BYTES` | segment size | 64 MiB |
| `MAKO_RAFT_CHECKPOINT_BYTES`, `_SECS` | applier thresholds (P7), above the segment size (design Decision 13) | 256 MiB, 10 |
| `MAKO_RAFT_CREATE` | `1`: this launch creates the cluster (design Decision 10) | unset |
| `MAKO_RAFT_DISK_VERIFY` | `1`: compare the WAL's replay with the core at shutdown (P3) | unset |
| `MAKO_RAFT_CRASH` | `<point>[:<n>][:powercut]` (P2, P6) | unset |
| `MAKO_RAFT_KILLTEST_DIR` | evidence for the kill-test checker (P6) | unset |

**The data directory**, one per server (site ids are unique per configuration,
`src/deptran/config.cc:339`, `:350`), on the local disk: `/var/tmp` is ext4 on
`/dev/sda2` (1.7 TB free on 2026-10-09). Not tmpfs: `/tmp` and `/dev/shm` are
RAM, and the tests need it (on 2026-10-09 tmpfs leftovers ran the host out of
memory). Not the NFS home, which the store refuses (P2). Never under
`/tmp/${USER}_*`, which `examples/run_rocksdb_test.sh:27` deletes whole and
`ci/ci.sh:84` in part. A real `fdatasync` here takes about 180 us for 4 KiB and
1.6 ms for 1 MiB (p50; a RAID controller with a write cache), so
`MAKO_RAFT_FLUSH_DELAY_US` adds to a real sync; tmpfs stays available by
setting `MAKO_RAFT_DATA_DIR`, for runs that model a device by the delay alone.

**Run directories.** Every launcher goes through one helper,
`scripts/raft_disk/store_dir.sh` (sourced; `run.py` has its Python twin). It
(1) **sweeps**: deletes each `raft-wal-$USER/*` run directory whose `RUN.lock`
no process holds (`flock -n` succeeds), as the kernel drops a dead launcher's
lock; (2) **makes** a fresh `mktemp -d` run directory and holds `RUN.lock` on a
file descriptor for the launcher's lifetime, so a concurrent sweep skips it;
(3) **checks** free space (refuses under 8 GB); (4) **deletes** the directory
at exit by a `trap`. A killed launcher leaves its directory to the next sweep;
`ci/ci.sh`'s `cleanup_processes` (`:78`) calls the sweep too. The launcher sets
`MAKO_RAFT_CREATE=1` on each server's first launch, so each retry
(`ci/ci.sh:517-536`, `:544-563`) and a direct `ci/ci.sh` run get their own.
```
${MAKO_RAFT_DATA_DIR}/<run>.XXXX/
  RUN.lock                       flock()ed by the launcher while it runs
  <site>-<partition>/
    LOCK                         flock()ed: a second opener fails closed
    wal/00000000000000000001.seg named by first sequence number
    base/                        RocksDB (P7)
    images/<S>-<T>.img           snapshot images (P8); .tmp while written
  <site>-<partition>.creating/   only while being created (P2); never opened
```
**Gates, on the host** (no Docker here; CI builds memory mode in Docker):
```bash
source ~/mako-verus-env.sh   # dep libs, clang-22, VERUS_PIN, GLR, RESULTS
cmake -S . -B build_rust_disk -G Ninja -DCMAKE_BUILD_TYPE=Release -DMAKO_USE_RAFT=ON -DMAKO_RAFT_DISK=ON
cmake --build build_rust_disk -j32   # build_rust: the same, OFF
(cd src/deptran/raft && cargo test --release -p raft-core -p raft-replay -p raft-store)
scripts/verus/tier1.sh <phase> rust rustdisk      # lab + four suites per lane
BUILD_DIR=build_rust_disk ./ci/ci.sh raftKillTest # P6
scripts/verus/core_check.sh && python3 scripts/raft_field_census.py  # clippy, ledger, correspondence --check, Verus; census
```
Verus must report 0 errors; `scripts/verus/core_trusted.txt` (`:3`, `blocks_for`
alone) only shrinks; the spec pin (`verus/spec/SPEC_VERSION.toml`) stays, since
`ObserveTerm` is uncoupled and `Restore` reuses step-aside. New core lines carry
ledger tags (`scripts/verus/ledger_lint.py:57`) for the A.2 rows P1 adds
(`docs/verus/modification-plan.md:957-981`), F20 `ObserveTerm` (decided), F21
the persist note and F22 `Restore` (§3); for A.2's lab case per row (`:959`)
they offer the snapshot cases, the core tests (F21 changes no behaviour) and
`raftKillTest` (no restart enters the lab). No export is added; the census
(`scripts/raft_field_census.py:43`) finds no C++ naming new fields.

**Performance** (§5). Memory mode must not move; disk mode is estimated in
§5 and measured against the estimates. `scripts/verus/gate_point.sh` takes
rounds and bounds from `docs/verus/gate-params.md:16-21`; arms are trees
(`scripts/raft_perf/rotation_trial.sh:31-39`): `build_rust_predisk`, the branch
point; `build_rust`, the branch in memory mode; `build_rust_disk`. CP1 (end of
P4), CP2 (end of P7): memory G1 and G2 gated against `build_rust_predisk`; disk
G1 and G2 at D = 1 ms (CP2: and D = 0) against `build_rust`, reported beside
§5's estimates, measured as `gate_point.sh` does (G1 by
`scripts/raft_perf/paired_stats.py`; G2 traced, by
`scripts/verus/two_follower_rounds.py`, `scripts/verus/gate_point.sh:44-46`,
`:57-61`). CP3 (P9): memory G1-G7 against `build_rust_predisk`, the final check
that memory mode did not move; disk as at CP2. No sweeps. Disk G2 writes about
155 MB/s per replica (design §5), so it runs with `DUR=6`.

## 2. The phases
### P0. The switch, the store crate, the runners
**Today.** `RAFT_TEST` (`CMakeLists.txt:463`, `:486-489`) is the only switch.
Cargo's inputs, a glob and a manifest list (`CMakeLists.txt:1215-1225`,
`:1251-1256`; a missed directory leaves `libraft_rt.a` stale, `:1209-1214`),
miss `replay/`, which the shell links (`Cargo.toml:69`), a defect (§3);
raft-rt's tests alone gate the build (`CMakeLists.txt:1346-1374`). The lab tree
gets `-DRAFT_TEST=ON` alone (`ci/ci.sh:466-471`), Tier 1 `build_rust` only
(`scripts/verus/tier1.sh:53-62`), clippy two feature sets
(`scripts/verus/core_check.sh:13-17`). **Changes.**
- `CMakeLists.txt:463` -> the option; `:1230-1233` -> features from both;
  `:1215-1225`, `:1251-1256` -> `store/`, `replay/`; `:1346-1368` -> a stamp
  `raft_store_test` (on `store/tests/*.rs`) running `cargo test` on
  `store/Cargo.toml` without the features `raft_rt_test` passes
  (`CMakeLists.txt:1362`), which `raft-store` lacks and cargo would refuse.
- `Cargo.toml:30` -> member `"store"` beside `.`, `rt`, `core`, `replay`
  (`:29-30`); `:55` -> `raft_disk = []`; `:57-70` -> `raft-store`,
  unconditional; `rt/Cargo.toml:32` -> forward `raft_disk`.
- `ci/ci.sh:466-471` -> `-DMAKO_RAFT_DISK` from `${BUILD_DIR}/CMakeCache.txt`; a
  disk lab gets a fresh store, `MAKO_RAFT_CREATE=1`, `MAKO_RAFT_DISK_VERIFY=1`;
  the count (`ci/ci.sh:489-490`) holds, as no lab case is feature-only.
- `scripts/verus/tier1.sh:53-62` -> lane `rustdisk` on `build_rust_disk`,
  refusing under 8 GB free in the root; `scripts/verus/core_check.sh:13` ->
  clippy with `raft_disk` too.
- `scripts/verus/verify_core.sh:44-66` -> §4's gate: Verus with
  `--output-json --time-expanded`, every name in
  `scripts/verus/verified_functions.txt` present and verified, and the wider
  trust scan. Fixes B26.
- `examples/raft_bench.sh:385-389`, `:291-318`, and each Raft example script
  beside `TEMP_PAXOS_DIR` (`examples/test_1shard_replication_raft.sh:28-34`) ->
  source `scripts/raft_disk/store_dir.sh`: sweep, a fresh locked run
  directory, a free-space check, deletion at exit (§1, "Run directories").
  `ci/ci.sh:78` (`cleanup_processes`) -> also run the sweep.

**New.** `store/Cargo.toml` (package `raft-store`; std only, no `rusty`, no
kernel, so its tests need no C++), `store/src/lib.rs`,
`scripts/raft_disk/store_dir.sh` and its test (two launchers sweeping at once
keep each other's run; a SIGKILLed launcher's run is swept by the next),
`build_rust_disk`,
`build_rust_predisk` (the branch point, the memory arm of §5),
`scripts/verus/verified_functions.txt` (§4's 349 names). **Tests.** Both trees
build with both stamps; the gate fails on a scratch copy of the core with a
proof made `external_body`, with an `assume(true)` and with a function removed.
**Done when.** `core_check.sh` and `scripts/raft_dsl.sh --check`
(`CMakeLists.txt:257-260`) pass. **Size.** 1-3 days.
**Risks.** A second tree costs 10-30 minutes a build; the lab tree is named from
`BUILD_DIR` (`ci/ci.sh:466`), so only the cache read keeps a disk lab off
`build_rust`.

### P1. The core: the persist note, `ObserveTerm`, `Restore`
**Today.** `RaftCore::step` (`core/src/event.rs:371-535`) matches `Event`
(`:24-108`) at `:398`; `CoreOutput` holds actions and log lines, nothing a proof
reads (`core/src/output.rs:124-133`, `:177-183`). Each term write
(`core/src/node.rs:663`, `:759`, `:889`, `:1902`; `core/src/heartbeat.rs:1359`)
comes with a vote write; the vote changes alone at `core/src/node.rs:685`, the
commit at `core/src/heartbeat.rs:122` and `core/src/node.rs:2148`, the log only
through `append` and `truncate_from` (`core/src/log.rs:314-323`, `:404-416`);
`wf` and `view` read only `RaftLog`'s four fields (`:119-130`, `:138-150`,
`:193`). Nothing loads saved state. **Changes.**
- `core/src/output.rs:124-133` -> `persist_: Option<PersistNote>`.
  `core/src/log.rs:119-130` -> `low_write_`, set in `new` (`:214-222`), lowered
  by `append` and `truncate_from`; `take_low_write` resets it with `wf`, `view`,
  base and length unchanged; `compact_through` and `reset` (`:532`, `:600`)
  leave it, as their callers push their own records (P3).
- `core/src/event.rs:398-534` -> copy term, vote, commit before the match; after
  it take the watermark and set the note if anything changed: exact by
  construction, even for a cut then an equal-length append (design Decision 4).
- `core/src/event.rs:24-108` -> `ObserveTerm { term, stopped, failover }`,
  `Restore { term, vote, commit, entries }` (owned, as `Propose`'s `cmd`, `:36`;
  no note), `Reply::Restored(bool)` (`:110-127`), `admits` (`:240-275`): a term
  below `raft_index_limit()`, and for `Restore` `!gated_` and an empty log;
  ensures (`:380-396`) and arms; `core/src/coupling.rs:2612` -> both false.
- `core/src/node.rs:310-324` -> `observe_term` (the body of `:887-898`,
  `:900-901`) and `restore`, which refuses (`Restored(false)`, unchanged) a gap
  after S, an entry without a value or of term 0, falling terms, a first term
  below `snapterm_`, a last above `term`, `term` at the ceiling (an exec copy of
  the spec-only `core/src/log.rs:22-24`), a commit outside S..last, a non-member
  vote (`core/src/node.rs:444-461`); else appends and pushes
  `apply_range(S, commit)` (`core/src/output.rs:85-90`).
- `src/server_h.rs:2261-2283` -> step `ObserveTerm`; the taint (`:2260`) moves
  to the accept branch (`:2286-2300`; see above). `:2409-2441` -> a newer term
  steps `ObserveTerm`, else `StepDown`/`SetFollower` as today; the sender's hint
  (`:2421-2424`) follows the step (`set_is_leader(false)` keeps it,
  `core/src/node.rs:1254-1262`).
- `replay/src/lib.rs:314-348` -> a `P <hard> <term> <vote> <commit> <log_from>`
  line per note; the writer (`:110-266`, `:350-447`) and parser (`:477-727`)
  learn the new variants. `docs/verus/modification-plan.md:981` -> rows F20-F22.

**New.** `PersistNote`, `RaftCore::observe_term`, `::restore`,
`RAFT_INDEX_LIMIT`; `core/tests/{persist_note,observe_term,restore}.rs` over
Setup's events (`core/tests/step_checked.rs:31-39`, less `EnterGates`, `:36-37`,
for `restore.rs`, as it sets `gated_`, `core/src/node.rs:319-322`), shared
through a new `core/tests/common/mod.rs`, the first comparing each event's note
with a before-and-after diff; a shadow in `replay()`
(`replay/src/lib.rs:754-794`) that folds `P` lines into a saved state and
compares it with the core after every step. **Tests.** The cargo tests; fresh
lab, G1 and G4 recordings through `core_replay`
(`replay/tests/core_replay.rs:7-8`); `core_check.sh`; Tier 1 memory lane. **Done
when.** All pass, Verus has 0 errors, `core_trusted.txt` and the ledger stay
clean. **Size.** 4-6 days. **Risks.** `inv()` and `ginv()` read `raft_log_`
(`core/src/node.rs:146-150`, `core/src/coupling.rs:305-315`): without
`take_low_write`'s frame every arm's proof breaks. The reply path steps
`StepDown` even when not leading (`src/server_h.rs:2281`); `ObserveTerm` picks
as `SettleElection` (`core/src/node.rs:895-899`), a change memory builds see,
covered by the snapshot cases (`src/lab_snapshot_cases.rs:1370-1384`).

### P2. `raft-store`: the WAL and the crash points
**Today.** Raft persists no state to files; it writes only diagnostics (the
recorder, `src/server_h.rs:3501-3529`; the trace kit, `server.cc:1685`), and its
snapshot store is a memory slot (`rt/src/snapshot.rs:39-41`). `Cargo.lock` has
no CRC, libc or RocksDB crate (the shell's CRC32 table is C++'s,
`src/snapshot_format_hpp.rs:98-121`). The crates abort on panic
(`Cargo.toml:80-84`), as design Decision 14 wants. **Changes.** None outside the
new crate. **New** (`store/src/`, std only). `crc.rs`: CRC32C by SSE4.2 through
`std::arch` when `is_x86_feature_detected!` finds it, else a slice-by-8 table
(0.13 against 0.65 us/KiB, §5), both tested against the check value `0xE3069283`
of `123456789`. `fs.rs`: `StoreFs`, as `RealFs` (`sync_data`; `sync_all` on
directories) and `MemFs`, both with a ledger of synced lengths and of creations
and renames no directory sync covered, which `MemFs::crash()` applies.
`record.rs`: `Record<P> { seq, hard, replace_from, entries, snapshot, last }`,
payloads through a `Codec` trait (no C++ type). `segment.rs`, `wal.rs`: a header
with design §5's fields and a CRC32C; batches of length, CRC32C, records;
rotation at `MAKO_RAFT_SEGMENT_BYTES`, the new header and directory synced
before the old segment closes; a reader that checks identity and contiguity,
deletes a last segment whose header is short or fails its CRC (it holds no
records), cuts a bad batch only at the end of the last segment, else fails
closed with a reason. `local.rs`: before anything else, the store reads
`/proc/self/mountinfo` (std only), finds the mount holding the data directory
(the longest mount-point prefix of its canonical path) and opens only on an
allow-list of local types, `ext4`, `xfs`, `btrfs`, `tmpfs`; anything else,
`nfs`/`nfs4` included, fails closed naming the type and the mount, so a
mistyped `MAKO_RAFT_DATA_DIR` can never write to the NFS home. `create.rs`:
**atomic creation**. A store is built in `<site>-<partition>.creating/` (`LOCK`,
segment 1 with its header; P7 adds the base), every file and the directory
synced, then renamed to `<site>-<partition>/` and the parent directory synced;
only the rename makes a store exist. A leftover `.creating` is deleted by a
creating launch, which then starts over; a launch without `MAKO_RAFT_CREATE`
that finds only a `.creating` fails closed with "creation interrupted; relaunch
with MAKO_RAFT_CREATE=1"; one that finds both deletes the `.creating`.
`queue.rs`: `RecordQueue<P>`, numbered under the caller's
`mtx_`. `flusher.rs`: take all, encode, write, `sync_data`, sleep, publish
`Durable { seq, last, commit }`, `notify_all`; an I/O error panics. `crash.rs`:
`crash_point(name)` from `MAKO_RAFT_CRASH` prints `crash <name>` to stderr, then
SIGKILLs through a local `extern "C"` (`kill`, `getpid`; no `libc`); `:powercut`
first truncates open segments to their synced length and undoes uncovered
creations and renames. Points: `wal.write.half`, `wal.write.done` (before
`fdatasync`), `wal.sync.done` (before publishing), `wal.rotate.created`,
`wal.rotate.dirsync`, `create.files` (inside the side directory),
`create.rename` (after the rename, before the parent's sync). **Tests.**
`store/tests/wal_crash.rs`: a crash at every step of a run with three
rotations, mid-header included, reads back a prefix holding every published
batch; damaged stores fail closed with their reasons.
`store/tests/create.rs`: a crash at every creation step, plain and power cut,
then both relaunches: a creating one yields a fresh store, one without the
flag fails closed or opens the finished store, never a half-made one.
`store/tests/local.rs`: mountinfo samples (NFS, ext4, tmpfs, a bind mount,
a path under a nested mount) give the right verdict.
**Done when.** These pass, clippy clean. **Size.** 4-6 days. **Risks.** A
missing directory sync shows only in a power cut (`MemFs`, P6). CI builds Rust
1.91.0 (`Dockerfile.ubuntu24:33`); only P3 and P6 decode real commands.

### P3. The shell: records and the flusher
**Today.** The step wrappers (`src/server_h.rs:1510-1546`) call the core and the
recorder; `AppendLocal` steps into a private output (`:3604-3607`). The snapshot
paths write saved fields outside `step` (`:2919-2929`, `:2561-2610`); compaction
drops only entries at or below S (`:1414-1442`, `core/src/helpers.rs:388-401`).
The apply thread starts at `src/server_h.rs:1946`, joins at `:3947-3952` and
`:2661-2666`. **Changes.**
- `src/server_h.rs:1510-1546` -> in disk builds a note becomes a record under
  `mtx_`: hard state, `replace_from`, and each entry's term and a handle clone
  from `log_from` on (`RaftLog::get`, `core/src/log.rs:266`).
- `src/server_h.rs:2920-2929` -> push `snapshot(S, T, image: None, keep)`;
  `:2602` -> push `install(S, T, keep: retain_suffix (:2537), commit: S)`.
- `:902-1037` -> `disk_: OnceLock<Arc<DiskShell>>`, published by `rpc_ready_`
  (`:1947-1948`); in disk builds `:1896` -> parameters, `LOCK`, a new store (an
  old one fails closed until P5); `:1946` -> start the flusher; `:3947-3952`,
  `:2666` -> join it.
- `:3921-3953` -> with `MAKO_RAFT_DISK_VERIFY=1`, replay the WAL and compare
  term, vote, commit, boundary and entries (term, `command_digest`,
  `:3541-3552`) with the core; a mismatch aborts, a match prints
  `[DISK-VERIFY] site=<s> ok records=<n>`.

**New.** `src/disk.rs` (canonical: an entry beside `rust-modules.toml:97-103`,
`src/lib.rs` regenerated by `scripts/raft_dsl.sh --rewrite`): `DiskShell`,
`ShellCodec` (`raft_command_encode`, `src/server_h.rs:607`, into a `Vec`, as
`fnv_emit` feeds a hash, `:3554-3557`), `record_from_note`, the verify.
**Tests.** Tier 1 (both lanes) and one disk `raft_bench` run, verify on. **Done
when.** All pass, the lab prints five `[DISK-VERIFY]` lines and the bench three;
the replication suites' coverage is best effort, as their followers die by
SIGTERM at `SIG_DFL` (`src/mako/benchmarks/dbtest.cc:246`,
`examples/test_1shard_replication_raft.sh:108-115`). **Size.** 3-4 days.
**Risks.** A follower batch (256 entries, `server.cc:233-241`) costs 256
refcount bumps under `mtx_` (`src/rusty-rustc/src/lib.rs:399-407`). The flusher
reads commands off the poll thread; their `save`s are `const`
(`src/deptran/replication_log_entry.h:29`, `src/deptran/tpc_command.h:44`).

### P4. The shell: outputs wait for the WAL
**Today.** The RPCs run inline on the poll thread (`rt/src/rpc.rs:386-404`,
`src/srpc/rpc/server.rs:1477-1479`): `__dispatch__` lends the request to
`dispatch`, which calls the handler and replies (`rt/src/service.rs:97-99`,
`rt/src/rpc.rs:419-492`). The tick sends after its section
(`src/server_cc.rs:85-176`), the campaign after `StartElection`'s
(`src/server_h.rs:3280-3333`), the apply thread right after its pop
(`:2697-2776`). Since the Lion merge the poll thread has no fixed 1 ms poll:
a job another thread adds (`PollThread::add`,
`src/srpc/reactor/reactor.rs:3082-3087`) wakes its driver at once, through
Lion's waker and the epoll eventfd (`:1007-1022`,
`src/srpc/reactor/epoll_wrapper.rs:186-202`); `Start`'s wake is such a job
(`rt/src/seam.rs:154-164`). A fiber sleep or event timeout is a Lion timer in
whole milliseconds, rounded up, which may fire up to 1 ms early and sleep
again (`src/srpc/reactor/reactor.rs:4618-4652`), so an idle fiber sleep under
1 ms takes 1-2 ms.

**Decision: the flusher sends the replies it made durable.** A follower's reply
is on every commit's path. The decision was made (2026-10-08) when a
poll-thread job waited up to 1 ms; since the Lion merge a job costs one
cross-thread wake (above), so the remaining gain is one hop fewer per reply,
not a millisecond. A held reply needs only the xid, the pending guard, the
weak connection and the response, all `Send` (`src/srpc/rpc/server.rs:132-150`;
`sconn_reply` reads only the xid and the connection's instance id,
`:1246-1284`). Any thread may send: a sender off the connection's poll thread
that finds its outbound buffer empty and the connection idle writes the frame
to the socket itself, under the outbound lock, and wakes the writer task only
for what `send(2)` did not take (`tcpconn_send_frame`,
`src/srpc/rpc/tcp_channel.rs:719-786`); no separate flush is needed. So after
publishing d the flusher sends, in hold order, every reply d covers. The fibers
poll the durable number with `raft_fiber_sleep_us` (`src/server_cc.rs:30`), as
collect (`:290`) and the vote wait (`rt/src/seam.rs:283`) poll; at idle each
poll step is a 1-2 ms timer (above), where a wake job setting their
`IntEvent`s would be seen at once (decision 6, §3). `Start`'s wake of the
heartbeat fiber (`src/server_h.rs:4066`) is now immediate too. Cost: half a day
over the poll-thread list. **Changes.**
- `scripts/rpcgen_rust.py:526-578` -> also emit `dispatch_held` (owning the
  request) and `HeldReply::send` (a header-only `Request`, reply);
  `dispatch` stays for memory builds and test doubles; `rt/src/rpc.rs` is
  regenerated (`CMakeLists.txt:1187-1202`).
- `rt/src/service.rs:97-99` -> in disk builds `dispatch_held`, then the queue's
  `last_seq` (after the inline handler: an upper bound on its tail), then
  `disk_hold`; no `Serve*` or export change.
- `src/server_cc.rs:124` -> in disk builds, the tick's tail; after `:130-132`
  poll, then send (`:139`); InstallSnapshot stays unwaited under the lock
  (`:101-123`). `src/server_h.rs:3289-3291` -> the campaign's tail; poll after
  `:3293-3295`, then broadcast (`:3327-3333`).
- `src/server_h.rs:2739-2741` -> in disk builds, for a leader, wait on the
  store's condvar while `id` exceeds the durable last index, timing out to
  recheck `apply_thread_running_` so the join (`:3947-3952`) cannot hang. With
  two or more servers the wait never blocks (design §3): a commit needs a
  follower's acknowledgement, and the leader sends only entries its WAL holds.
  Mako reads the role again (`raft_worker.cc:1116`); a node that becomes leader
  between the two reads is harmless, as its log was on disk before its vote
  requests left, and a committed entry is on a majority's disks.
- `rt/src/transport.rs:254-262` -> in disk builds, `drain` first closes Raft's
  gate (a new `CloseAdmissionForDrain`, as `src/server_h.rs:3922-3932`) via
  `served` (`rt/src/transport.rs:106`): srpc ignores its admission flag (only
  its accessors touch it, `src/srpc/rpc/server.rs:1024-1029`; the refusal it
  documents, `:62-66`, is never sent; dispatch checks nothing, `:1437-1446`), a
  held request stays pending (`:1424`), and the 5 s drain (`raft_worker.cc:253`,
  `:587-593`) could time out; its comment (`:250-252`) is rewritten.

**New.** `HeldReplies`, `wait_durable`, `wait_index` in `store/src/flusher.rs`;
in `src/disk.rs`, `disk_poll_durable` and `disk_hold`, which reads `durable_seq`
and pushes under `HeldReplies`'s mutex; the flusher takes that mutex only after
publishing d, so no reply waits for a later flush; `store/tests/held.rs`, a hold
racing a publish included. **Tests.** `held.rs`; `rt/tests/rpc_wire_golden.rs`
(wire bytes unchanged); Tier 1 both lanes, the disk one at
`MAKO_RAFT_FLUSH_DELAY_US=1000`, where replies are both held and sent at once,
over RPC (the lab calls `Serve*` directly, `src/lab.rs:520-551`); CP1. **Done
when.** All pass and no drain times out. **Size.** 4-6 days. **Risks.** At 5 ms
replies miss their round: the heartbeat is 5 ms (`server.h:169-173`), collect's
limit too (`src/server_cc.rs:198-211`). The fibers' durable polls step in
1-2 ms timers at idle; if CP1 shows them, the remedy is the wake job, which
the reactor now serves at once (`src/srpc/reactor/reactor.rs:1007-1022`).

### P5. The shell: recovery
**Today.** `SetupInternal` (`src/server_h.rs:1847-1972`) runs snapshot recovery
(`:1898-1907`; snapshots off, it fail-stops on uncovered progress,
`:2008-2026`), `Configure` (`:1914`), `EnterGates` (`:1933`; S = 0 and a log
from 1 only, `core/src/node.rs:317`), then threads, `rpc_ready_` and fibers
(`src/server_h.rs:1946-1970`); campaigns start at `:1194-1198`. A server starts
empty (B6); survivors stop re-dialling a dead peer after 15.5-46.5 s (B21;
`rt/src/transport.rs:281`, `src/srpc/rpc/client.rs:1604`,
`src/srpc/rpc/reconnect_policy.rs:20-29`); and a node cannot start while a
peer is down (B22, `raft_lane_rust.cc:43-55`). **Changes.**
- `src/server_h.rs:1896-1898` -> first the local-filesystem check (P2); no
  store: create one, atomically (P2), only under `MAKO_RAFT_CREATE=1` (which
  fails closed on an existing store, and deletes a leftover `.creating`); a
  `.creating` alone without the flag fails closed; else read the
  WAL (identity: site, partition, a member fingerprint from
  `LoadCurrentConfig`'s kernels, `:3647-3655`), cut a torn tail, fold 1..d,
  truncate the cut segment, start one at d+1. A snapshot in the state fails
  closed until P8; every refusal goes through `FailClosed`.
- `:2195-2202` -> no term bump when this server recovered a store (`disk_` set),
  as `Restore` overrides it; without one it bumps as today, which lab case 73
  asserts in disk builds too (`src/lab_snapshot_cases.rs:1319`).
- `src/server_h.rs:1914-1924` -> unless just created, decode the payloads
  (`raft_command_from_bytes`, `server.cc:438-450`) through
  `raft_entry_from_command` (`src/server_h.rs:3561-3573`), step `Restore` under
  `mtx_` and run its actions (`APPLY_RANGE`, `:1625-1629`); `Restored(false)`
  fails closed. Then the durable atomics, `seq := d`, `recovered_commit`.
  `:1194-1198` -> no campaign while `GetAppliedIndex()` (`:1290-1293`) is below
  `recovered_commit`: Mako's leader callback (by role, `raft_worker.cc:1116`)
  does not replay.
- `rt/src/transport.rs:281` -> in disk builds, a policy with no retry limit
  and waits below the shortest non-preferred election timeout, 0.5 s
  (`server.cc:201`): `max_retries: 0`, waits from 50 ms doubling to 200 ms,
  25-300 ms with jitter (public fields,
  `src/srpc/rpc/reconnect_policy.rs:9-16`; `aggressive()`, `:31-40`, waits up
  to 5 s, time for several campaigns, each deposing the leader), set through
  `set_reconnect_policy` (`src/srpc/rpc/client.rs:1837-1842`). A survivor then
  re-dials a restarted peer before its first campaign. Fixes B21.
- `rt/src/transport.rs:276-303`, `:867` -> in disk builds, `add_peer` tries
  for 1 s, then leaves the site to a dial thread that connects a fresh
  `Client` every 200 ms and fills the site's slot (a `OnceLock` per configured
  site in `peers`, `:107`); a send to an empty slot fails at once, as after a
  close. A fresh `Client` keeps the slot's one-shot `OnceLock` simple; since
  the Lion merge the thread could also re-dial a `Client` the fibers use, as
  `Client` is `Send + Sync`, its connection slot a `Mutex`
  (`src/srpc/rpc/client.rs:1552-1569`). A new export, called after
  the loop at `raft_lane_rust.cc:43-55`, waits until each partition has a
  majority connected, itself included (120 s, then fail as today). Fixes B22.

**New.** `store/src/state.rs` (`SavedState::apply`, shared by recovery, the
verify and the applier), `store/src/recover.rs`, points `recover.cut` and
`recover.segment`. **Tests.** `store/tests/recover.rs` (damaged stores refuse; a
last segment with a short header is deleted; recovery killed at each step and
rerun converges); Tier 1 disk lane. **Done when.** These pass, the harness's
`follower` scenario (P6) rejoins and its `down` scenario starts. **Size.** 4-5
days. **Risks.** A bad payload can abort
(`src/srpc/misc/serializable.rs:713-720`): only checksummed bytes are decoded.
Startup decodes the whole WAL on the poll thread, where setup is posted
(`src/deptran/raft_main_helper.cc:501-511`, `raft_worker.cc:411-417`). The
fingerprint covers ids, not addresses (`server.cc:964-983`).

### P6. Process-kill tests
**Today.** Nothing restarts a Raft process (`examples/raft_bench.sh:392-436`
only kills; `ci/ci.sh:615-628` is disabled). A starting node aborts unless all
peers accept within 120 s (`CONNECT_TIMEOUT`, `rt/src/transport.rs:97`, which
`add_peer` gives the retry loop, `:276-303`; `raft_lane_rust.cc:50-52`).
`raft_bench` keeps its workload in the offering process (`raft_bench.cc:1214`,
`:1261`) and is the perf instrument
(`scripts/raft_perf/rotation_trial.sh:31-39`); `simpleRaft` fixes its ports and
roles (`examples/mako-raft-tests/simpleRaft.cc:57-65`). Neither is the node. The
harness below is built beside P1-P5. **Changes.**
- `CMakeLists.txt:1738` -> `add_apps(raft_kill_node ...)`; `ci/ci.sh:804-806` ->
  suites `raftKillTest`, `shard1ReplicationRaftRestart` (host only).
- `src/disk.rs`, in disk builds under `MAKO_RAFT_KILLTEST_DIR` -> a `reveal`
  line just before each output leaves (kind, term, vote, index, commit, tail):
  for a reply, by whichever thread sends it, from a copy `__dispatch__` moves
  into the held entry (handlers park what they revealed in a poll-thread
  thread-local, as `rt/src/snapshot.rs:289-314` parks images); at
  `src/server_h.rs:3327` and `src/server_cc.rs:167`; after `Restore`, a
  `recovered` line (term, vote, commit, last, d, term runs); `write_all` per
  line, as `src/server_h.rs:3522-3529`.

**New.**
- `raft_kill_node.cc`, C++ since processes get Raft and RPC servers only through
  the C++ replication helper; no Raft logic. It follows `raft_bench.cc`
  (`:1337`, argv `:1346-1373` without `-b`, callbacks `:1489-1491`, `setup2`
  `:1505`, shutdown `:2036-2040`), adding `--incarnation`, an exit code for a
  closed-failed `setup2` (`src/deptran/raft_main_helper.cc:942-945`), paced
  proposals `<proc>.<incarnation>.<n>` while leading (acknowledged at its own
  leader-role apply: `add_log_to_nc` only queues, `:1151-1184`), and events
  (start, ready, apply, ack, role) by `write(2)`, which SIGKILL cannot lose.
- `scripts/raft_kill/run.py` (stdlib Python): 3 or 5 nodes on ports 15000-19999,
  below the ephemeral range
  (`examples/simple_transaction_rep_port_utils.sh:148-155`) and `ci/ci.sh:131`'s
  bands; `MAKO_RAFT_REPLAY_DIR` and `MAKO_RAFT_KILLTEST_DIR` always set, under
  the run directory (`/var/tmp/raft-wal-$USER/<run>/evidence`), made, locked,
  swept and deleted as §1's "Run directories" says, the evidence kept on a
  failure (compressed to `$RESULTS`);
  `MAKO_RAFT_CREATE=1` on incarnation 0 only, `MAKO_RAFT_CRASH` on the armed
  launch only; scenarios `follower` (first), `leader`, `majority`, `all`,
  `points` (plain and `:powercut`), `recovery`, `create` (incarnation 0 killed
  at `create.*`, relaunched with and without the flag), `down` (one node stays
  down while another restarts, B22); restarts within 100 s; quiesce, SIGTERM,
  check.
- `scripts/raft_kill/check.py` and `test_check.py` (each check broken once): (1)
  every launch reaches `ready`, except one its own armed point killed (its
  `crash` line, P2); (2) no index with two ids, every acked id applied
  everywhere; (3) one leader per term and (4) one granted candidate per voter
  and term, from `settle` records (`replay/src/lib.rs:151-169`); (5) each
  `recovered` line covers the prior reveals: d ≥ tails, term ≥ terms, last ≥
  acknowledged index, commit ≥ commits sent, and at an equal term the same vote
  for reveals that show one (granted vote replies, the candidate's requests);
  (6) no `.tmp`, `.creating` or torn segment after a recovery; (7, P7) a
  bounded WAL; (8) after the run, its directory is gone and no run directory
  older than it remains unlocked (the sweep ran).
- `examples/test_1shard_replication_raft_restart.sh`: the 1-shard test
  (`examples/test_1shard_replication_raft.sh:36-43`, `:180-216`) with p1's
  `dbtest` SIGKILLed mid-run and relaunched onto its log.

**Tests.** `raftKillTest` once, at 1000 µs; `test_check.py`, the restart script,
Tier 1 disk lane. **Done when.** All pass (milestone A); a Mako-side restart
failure is recorded with its cause. **Size.** 5-7 days. **Risks.** A restarted
Mako follower re-applies control entries (`src/mako/mako.hh:181-341`), and a
no-op would hang it (`:280-285`), though Raft's no-ops stop at `server.cc:897`.

### P7. The base and the applier
**Today.** Raft calls no RocksDB, though its binaries link it after
`libraft_rt.a` (`CMakeLists.txt:1309-1322`, `:1331-1333`); neither Mako's
`RocksDBPersistence` (`src/mako/rocksdb_persistence.cc:131-132`) nor the unused
C++ `RocksDBLogStorage` (`rocksdb_log_storage.hpp:766-769`) fits (design §5).
The WAL only grows. **Changes.** `Cargo.toml:55` ->
`raft_disk = ["raft-store/rocksdb"]`; `CMakeLists.txt:1264` -> `rocksdb` in disk
builds; `src/server_h.rs:1896-1898` -> recovery merges base and WAL (design §4);
`:1946`, `:3947-3952` -> the applier runs beside the flusher, which offers it
each synced batch without blocking, on a bounded queue that drops one when full;
`catch_up(c, d)` then reads c+1..d from the segments with recovery's reader
(design §3, §4). **New.** `store/src/base.rs` (`Base`; `MemBase` drops unflushed
batches on `crash()`); `store/src/rocks.rs` (feature `rocksdb`): `rocksdb/c.h`
open `:147`, write buffer `:1235`, batch put and delete-range `:783`, `:844`,
write `:484` with the WAL off `:2081`, waiting flush `:701`, `:2153`, get
`:490`, iterators `:599`, tests linking `rocksdb`; `store/src/applier.rs`
(records in order, c in each batch; a checkpoint at the thresholds flushes, then
deletes segments wholly at or below c; a base error stops it). The base is
created only inside the `.creating` directory (P2): every later open passes
`create_if_missing = 0`, and an open RocksDB refuses (missing, corrupt, a
bad `CURRENT` or `MANIFEST`) fails closed with RocksDB's status string, never
repaired (`rocksdb_repair_db` can drop data) and never recreated empty. Points
`base.write`, `base.flush`, `base.delete`, `recover.base`. **Tests.**
`store/tests/base_crash.rs` (a one-slot queue too: the base must equal the fold
of 1..d); RocksDB tests by hand; `raftKillTest` with small thresholds and check
7; Tier 1; CP2. **Done when.** All pass (milestone B). **Size.** 4-6 days.
**Risks.** CI has RocksDB 8.9.1 unpinned (`apt_packages.sh:81`), the host
9.11.2. Without snapshots the base holds every entry.

### P8. Snapshots as files
**Today.** A snapshot is created and saved to memory under the apply gate and
`mtx_` (`src/server_h.rs:3063-3081`, `server.cc:1007-1050`); an install holds
both while one call prepares, saves and commits (`src/server_h.rs:4211-4241`,
`server.cc:848-886`), though `Commit()` may publish only after Raft saved the
bytes (`snapshot_callbacks.h:15-19`). **Changes.**
- `server.cc:1007-1050`, `src/server_h.rs:2861-2934` -> in disk builds, S and T
  under `mtx_`; create and write the file (design §3) under the apply gate
  alone; then `mtx_` to save, move the boundary, compact, push the record.
- `server.cc:848-886` -> in disk builds, prepare and commit kernels under
  `raft_catch` (as `:822-830`); `src/server_h.rs:4211-4241` -> first write and
  sync the file, ahead of the gate (`:4217-4220`), in the function both the
  service (`:4125`) and the lab (`src/lab_snapshot_cases.rs:458`) call (a new
  kernel lends Rust `data`'s bytes; `server.cc:455` only fills one); then design
  §4: `ObserveTerm`, prepare, record, release `mtx_`, wait durable, `Commit()`.
- Recovery injects the image (as `src/lab_snapshot_cases.rs:1220-1228`, via
  `src/server_h.rs:1266-1270`) for `:1898-1907`; `raft_kill_node.cc` gets
  snapshot callbacks (as `raft_bench.cc:1496-1501`) whose image is its applied
  (index, id) list; `Commit()` logs those as `snap` events, which check (2)
  counts as applied.

**New.** `store/src/images.rs`; recovery deletes every `images/*.tmp` (an
image whose rename never happened, so no record names it) and every image the
state does not name; points `image.rename`, `image.dirsync`, `image.delete`. **Tests.** Lab snapshot cases with verify; `raftKillTest` with
`MAKO_RAFT_SNAPSHOTS=1`; one bench snapshot run. **Done when.** All pass
(milestone C). **Size.** 3-5 days. **Risks.** An uncommitted staged install
aborts on destruction (`snapshot_callbacks.h:15-27`); an install blocks the poll
thread for a flush; Mako has no callbacks (design §2).

### P9. Proofs and documents
**Today.** `Restore`, `ObserveTerm` uncoupled; notes unproved; the host contract
rules out restarts (`docs/verus/host-contract.md:59-61`, `:150-152`); B6 open
(`docs/verus/bugs-found.md:27`, `:200`). **Changes.**
`core/src/coupling.rs:2585-2613` -> `Restore`'s premise with a ghost `prev`
(design §6) and `lemma_restore_ginv`, reusing `:475-506` as `:510-528` does,
`prev` cfg'd as `core/src/node.rs:109-116`; exactness proved (a ghost view at
the last take) or trusted in the host contract, never `external_body`; host
contract, B6, design status updated. **New.** `lemma_restore_ginv`. **Tests.**
`core_check.sh`, Tier 1, `raftKillTest`, CP3. **Done when.** 0 errors, trusted
list unchanged, CP3 holds. **Size.** 2-5 days. **Risks.** A cfg'd field in a
`verus!` enum variant is untried; the fallback is a ghost `RaftCore` field.

## 3. Open decisions and main risks
**Decisions.** (1) Approve F21, the persist note, and F22, `Restore`
(`docs/verus/modification-plan.md:987-988`); F20 is decided. (2)
`MAKO_RAFT_CREATE=1` or a helper flag for a creating launch. (3) Decided: the
reconnect policy and the majority start are disk-only, as memory builds must not
change (§5); B21 and B22 stay open there. (4) Exactness proved or trusted. (5) A
disk CI job (CI's Docker has no tmpfs, `.github/workflows/ci.yml:217`, which
no longer blocks it: the store is on disk, and the local check (P2) allows the
container's filesystem once `overlay` joins the allow-list for CI). (6) How the fibers learn of
durability: poll (the 2026-10-08 decision; since the Lion merge each idle poll
step is a 1-2 ms timer, P4) or the design's wake job, which the reactor now
serves at once (`src/srpc/reactor/reactor.rs:1007-1022`; the wake descriptor
this decision once asked for exists). Collect's own 1 ms step stays either
way. (7) A Mako-side restart failure: fix Mako's callbacks
or defer. (8) The leader's AppendEntries waits for its tail, as the design has
it, or only for the records holding what it sends, its term and its commit: §5
estimates the second halves the saturated loss (G2 -20% to -11% at D = 0).
(9) A store that fails closed has no automatic repair: wiping it makes the
node forget its vote and log, which is unsafe in Raft, so the node stays down
until someone decides (wipe and rejoin as a new member, or restore the files).
Proposed: out of scope; the failure message names the store and the reason.
**Risks.** A commit waits two flushes plus a leader hop; §5 estimates G1's p50
at +0.1%, +5% and +76% for D = 0, 0.2 and 1 ms, and G2 at -20% to -34%, a round
lost at 5 ms (P4); the base keeps every entry without snapshots (on the
local disk now, not RAM, but the core still holds the whole log in memory); recovery and Mako's re-apply grow with the log; until P9, restarts
and `ObserveTerm` are outside the proof and the kill tests are the evidence.

**Bugs.** Each bug found goes into `docs/verus/bugs-found.md`. It holds B21,
survivors stop re-dialling (fixed in disk builds by P5; decision 3), and B22, a
down peer blocks startup (likewise). Seven more found while planning are
B23-B29: srpc ignores admission (`src/srpc/rpc/server.rs:62-66`, `:1024-1029`,
`:1437-1446`; worked around in P4); `ParseEnvUint64OrDefault` takes `-1`
(`server.cc:146-162`; open); a no-op hangs a Mako follower
(`src/mako/mako.hh:280-285`; latent, open); `verify_core.sh` sees only
`external_body` (`scripts/verus/verify_core.sh:58-65`; fixed in P0, §4); cargo's
inputs miss `replay/` (`CMakeLists.txt:1215-1225`, `:1251-1256`; fixed in P0);
the replay format comment shows four sections (`replay/src/lib.rs:6`; open);
GitHub CI runs no Raft lab (`.github/workflows/ci.yml:291-353`; open).

## 4. What stays verified
**Today** (`a4b1eaa02`, `core_check.sh`, 2026-10-08): clippy is clean with and
without `raft_test`; the ledger lint finds 0 unregistered lines in 13 files;
the correspondence doc is current; Verus reports 387 verified, 0 errors, in
26 s, with `blocks_for` the only trusted function. With
`--output-json --time-expanded`, 349 core functions carry verification
conditions (279 exec, 55 proof, 15 spec, in 12 modules) and none fails. The
spec is v1 (`verus/spec/SPEC_VERSION.toml`).

**The rule.** No phase loses any of this. A function verified today stays
verified, unless the phase's commit removes it from the list below with the
reason, as `core_trusted.txt` records trust; the trusted list only shrinks;
the spec pin stays; Verus reports 0 errors. New core code (P1's note,
`ObserveTerm`, `Restore`; P9's lemma) is verified, never trusted.

**The gate** (P0). `verify_core.sh` adds `--output-json --time-expanded` and
fails when a name in `scripts/verus/verified_functions.txt` (the 349, committed
by P0) is missing or failed; a phase that adds a verified function appends it.
The trust scan (`scripts/verus/verify_core.sh:58-65`) sees only
`#[verifier::external_body]` with a lower-case `fn` within four lines (B26); P0
makes it read both attribute spellings of `external_body`, `external`,
`external_fn_specification` and `assume_specification`, and any `assume(` or
`admit()` in `core/src` (the core's own `admit` is a method,
`core/src/authority.rs:832`, called as `.admit(`, `core/src/heartbeat.rs:329`).
This fixes B26. `core_check.sh` passing is in every phase's "Done when" through
§1's gates.

**Where it could break.** Before P9 only P1 edits the core. Its note is taken
after the match, but every arm's proof reads `raft_log_`, so the low-write mark
needs its frame (P1's risk); the gate names any arm that stops verifying. The
new events are false in `coupled` (`core/src/coupling.rs:2612`), so the coupling
proofs of the other events do not change. P9 adds `lemma_restore_ginv`. The
shell (`src/`, `rt/`, `store/`) stays outside the proof, as today; the kill
tests (P6) are its evidence.

## 5. Performance
**Memory builds do not change.** With the switch off, a build differs from
today's only in P1's core. Every step copies term, vote and commit before its
match and compares them after it, and `append` and `truncate_from` lower a mark
(`core/src/event.rs:398-534`, `core/src/log.rs:314`, `:404`): about ten
instructions a step. G2's leader steps about once per entry (the append) and
four times per 256-entry round, so at 34,112 entries/s this is under 0.1% of a
core. `ObserveTerm` replaces direct writes only on the InstallSnapshot reply
paths, which no gate point takes (snapshots off). Everything else is disk-only:
records, the flusher, held replies, the fibers' polls and the apply wait (P3,
P4) test `cfg!(feature = "raft_disk")`, a constant the compiler folds; the drain
gate (P4), the reconnect policy and the majority start (P5), recovery, the
applier and the image files (P5, P7, P8) are in disk builds only. `raft-store`
is linked but not called. The recorder's `P` lines (P1) need
`MAKO_RAFT_REPLAY_DIR`, which no perf script sets.

**The check.** A third tree, `build_rust_predisk` (the branch point, memory
mode), is the arm the branch's memory build (`build_rust`) is gated against: at
CP1 and CP2, G1 and G2; at CP3, G1-G7 (P1 changes `step`, so the election
times too). Rounds and bounds are `docs/verus/gate-params.md:16-21`, `:41`:
p50 +2%, p99 +5% (G3 +2.4%, +6%), throughput -2%, G2's traced round +2%,
election times +10%; the paired noise is 1-5%, G3's p99 11% (CV_paired, the
same rows). A miss is fixed before the phase merges.

**Disk builds: the model.** One flush of n entries, K KiB in all, costs
```
F(n, K) = s + D + c*K + e*n
```
| Symbol | Meaning | Value | Source |
|---|---|---|---|
| D | the injected sync delay, `MAKO_RAFT_FLUSH_DELAY_US`: a device's `fdatasync` | 0, 200, 1000 us | runs |
| s | `fdatasync` on tmpfs, with its syscall | 1 us | measured |
| c | per KiB: tmpfs `write` 0.60, copy 0.07, CRC32C 0.13 (SSE4.2) or 0.65 (table) | 0.80 or 1.32 us | measured |
| e | per entry: encoding beyond the copy | 0.5 us | estimated; P3 measures |
| h | the hop that wakes the tick after `Start`: before the Lion merge the idle poll loop's 1 ms `epoll_wait` (`src/srpc/reactor/epoll_wrapper.rs:124` at `a4b1eaa02`); since, one cross-thread wake (`src/srpc/reactor/reactor.rs:1007-1022`) | was 1000 us, a wake seen 530 us later on average; now near 0, not measured | code; measured before the merge |
| q | collect's poll step (`src/server_cc.rs:198`) | 1000 us | code |
| T | a saturated round, B/X: G2's 256 entries of 4 KiB to each follower | 7,505 us (traced round 7,409, median of 10 runs) | bug-fix gate |
| L, X | memory baselines, median of rounds (bug-fix gate, `0633e1ffc`) | table below | measured |

Measured on this host (Xeon E5-2683 v4, `/dev/shm`, 2026-10-08): write plus
`fdatasync` takes 4 us for 4 KiB, 0.49-0.67 ms for 1 MiB and 8.5-10.3 ms for
16 MiB; copying runs at 15-16 GB/s; CRC32C at 1.55 GB/s by table and 7.7-8.3
GB/s with SSE4.2. `scripts/raft_disk/params.rs` measures these;
`scripts/raft_disk/model.py` computes the estimates below from them.

**The default store is now the local disk** (2026-10-09, §1), where the sync
is real: write plus `fdatasync` on `/var/tmp` (ext4, `/dev/sda2`) took 180 us
p50 and 298 us p99 for 4 KiB, 1.59 ms p50 for 1 MiB (200 and 50 calls). So a
run at D = 0 on `/var/tmp` sits near the D = 200 us column at low load, and
past it for large flushes. The model's s and c become those measurements;
`params.rs` gains a `--dir` to measure both filesystems, and CP1 runs on both.

*Low load* (G1, G3, G5: one entry a round). The leader's flush,
F1 = F(1, b), starts at `Start`, beside the up-to-1-ms hop (h1) that wakes the
tick, and the tick waits for what is left, seen on the next loop (P4). The
follower's flush holds its reply for F1, and collect sees replies only at its
1 ms steps, counted from the send; r is the memory round trip (assumed 0.1 ms
at 4 KiB, 0.4 ms at 286 KB; at 1 MiB it spans several steps, so the step costs
F1 on average):
```
W  = F1^2/(2h) + F1/2   if F1 <= h,   else F1
dC = q*(ceil((r + F1)/q) - ceil(r/q))
L_disk = L + W + dC
```
*Saturation* (G2, G4, G6: rounds of B entries, K_B KiB). The follower flushes
the round's batch before it replies, F_B = F(B, K_B). Under the design's tail
rule (§3, "What waits for the disk") the tick also waits for the burst `Start`
appended since the last round, about B entries, so about F_B again; waiting
only for the records holding the sent entries (decision 8) skips it, as those
entries are old under backlog:
```
X_disk = B / (T + k*F_B)        k = 2 (tail) or 1 (sent entries)
```
G4 and G6 are not traced; their B is 58 and 16 (the 16 MiB cap,
`server.cc:227`). Two bounds do not bind: the flusher's CPU, 1/(c*K/n + e),
over 1,200 entries/s at 1 MiB and 270,000 at 4 KiB; tmpfs, about 1.6 GB/s per
writer.

**Stale since the Lion merge (2026-10-09).** The low-load model hides the
leader's flush in h's hop, which the merge removed: a flush now adds to the
leader's side in full, plus whatever the tick's durable poll costs (a 1-2 ms
timer at idle, P4). The memory baselines L and X predate the merge too, whose
gate measured G1 p50 -17.8%, p99 -24.1% and G6 +11.5% against the pre-merge
build. The table is the pre-merge model; `model.py` recomputes it, with new
baselines, before CP1.

**The estimates** (SSE4.2 CRC32C; tail rule, sent-entries rule in brackets):

| Point | Memory | D = 0 | D = 200 us | D = 1 ms |
|---|---|---|---|---|
| G1 p50, 4 KiB at 240/s | 2,641 us | 2,643 (+0.1%) | 2,764 (+4.7%) | 4,646 (+76%) |
| G3 p50, 286 KB x 6 at 190/s | 3,362 us | 3,500 (+4%) | 3,665 (+9%) | 5,587 (+66%) |
| G5 p50, 1 MiB at 55/s | 8,489 us | 10,057 (+18%) | 10,530 (+24%) | 12,130 (+43%) |
| G2, 4 KiB | 34,112/s | 27,231 (-20%) [30,285, -11%] | 26,120 (-23%) [29,585, -13%] | 22,454 (-34%) [27,082, -21%] |
| G4, 286 KB x 6 | 2,859/s | 2,356 (-18%) [2,583, -10%] | 2,350 (-18%) [2,580, -10%] | 2,325 (-19%) [2,564, -10%] |
| G6, 1 MiB | 187/s | 143 (-23%) [162, -13%] | 142 (-24%) [162, -14%] | 140 (-25%) [160, -14%] |

With a table CRC32C, G2 at D = 0 falls to 24,460/s (-28%) and G5 rises 32%;
hence P2's `crc.rs` uses SSE4.2 through `std::arch` when the CPU has it. What
the numbers say:
- A follower's flush under about 0.9 ms hides in collect's 1 ms step, and a
  leader's mostly in the hop that wakes the tick: G1 costs nothing at D = 0
  and 5% at 0.2 ms. Past that a flush costs a whole step (G1 +76% at 1 ms), so
  CP1's runs at 1 ms pay the step in full. G3's r is assumed: at 0.2 ms, an r
  above 0.57 ms would add a step too.
- Throughput is lost to bytes more than to D: tmpfs's write, 0.6 us/KiB, is
  most of F_B, and D counts once per round of up to 16 MiB.
- The tail rule costs a second flush per saturated round; the sent-entries
  rule halves the loss. A wake descriptor (decision 6) removes only W's
  notice term, up to 1 ms at low load; collect's own step stays.
- Latency at saturation is queueing: about MAXOUT/X, G2 150 ms against 120.

**Checking the model.** CP1 runs disk G1 and G2 at D = 1 ms; CP2 adds D = 0
(§1). A measured change off its estimate by more than a quarter of the estimate
means a term is wrong: find it with the trace kit (`raft_trace_through`,
`src/server_cc.rs:31`) before changing code. G3-G6 in disk mode stay estimates:
G4 writes 0.8 GB/s per replica, three replicas on one host, more than one
local disk sustains, and without snapshots (P8) the base keeps every entry.
