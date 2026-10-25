# Raft conversion, verification and disk persistence: git history

Every commit of PR #92 (branch `raft-rust` of `yangzeyu1026/mako`) since it left `mako-dev`, oldest first (merges and the srpc subtree squash left out). The PR was squash-merged, so none of these commits is in `mako-dev`'s history; they stay reachable through the PR:

```bash
git fetch origin pull/92/head
git show 4973a7576
```

The list ends at `190122dbf`; any later commits are in the PR.

| commit | date | author | subject |
|---|---|---|---|
| `4973a7576` | 2026-08-22 | Shuai Mu | raft: prepare vote messages for Rust DSL |
| `25909983d` | 2026-08-22 | Shuai Mu | raft: migrate vote messages to inline Rust DSL |
| `f5c770a35` | 2026-08-23 | Shuai Mu | raft: prepare primitive messages for Rust DSL |
| `55d6a55d4` | 2026-08-23 | Shuai Mu | raft: migrate primitive messages to Rust DSL |
| `368dce4ed` | 2026-08-23 | Shuai Mu | raft: migrate in-memory status enums to Rust DSL |
| `498125d1f` | 2026-08-23 | Shuai Mu | raft: prepare snapshot mode enums for Rust DSL |
| `cbfbcf61b` | 2026-08-23 | Shuai Mu | raft: migrate snapshot mode enums to Rust DSL |
| `bfd4f23bb` | 2026-08-23 | Shuai Mu | raft: migrate disk diagnostic POD to Rust DSL |
| `4b2e330b5` | 2026-08-23 | Shuai Mu | raft: migrate preferred leader predicate to Rust DSL |
| `c6a9b97b6` | 2026-08-23 | Shuai Mu | raft: compile extracted DSL in provenance gate |
| `b0760cd99` | 2026-08-23 | Shuai Mu | raft: close behavior-neutral Rust DSL boundary |
| `1071481de` | 2026-08-23 | Shuai Mu | raft: expand behavior-preserving Rust DSL ownership |
| `7f52613fe` | 2026-08-26 | Shuai Mu | raft: harden recovery, durability, and runtime liveness |
| `287283635` | 2026-09-07 | zyang2 | build: make CMake-version-specific values portable across 4.x |
| `4e427f74e` | 2026-09-07 | zyang2 | raft: give slotid_t/ballot_t one definition, fixing an ODR violation |
| `7ad823f81` | 2026-09-07 | zyang2 | docs: record raft Stage 2 prep plan, DSL examples, and fiber/RPC architecture |
| `84f1dea60` | 2026-09-07 | zyang2 | raft: let the DSL own INVALID_SITEID, dropping 5 injected parameters |
| `1f17d332e` | 2026-09-07 | zyang2 | docs: replace the Stage 2 TODO with a progress record |
| `207ff1fbd` | 2026-09-07 | zyang2 | raft: decode persisted ballots as signed, matching the writer |
| `9dee92ee1` | 2026-09-07 | zyang2 | raft: make the DSL clippy-clean for crate-level verification |
| `292a9bedf` | 2026-09-07 | zyang2 | raft: stand up the Stage 2 verification crate |
| `63249730e` | 2026-09-07 | zyang2 | raft: commit the Stage 2 crate lock file |
| `a33d88d29` | 2026-09-07 | zyang2 | raft: bring RaftSubmissionProgress under DSL ownership |
| `e57938888` | 2026-09-07 | zyang2 | raft: remove the per-carrier rustc stage, completing Option B |
| `76256a2fa` | 2026-09-07 | zyang2 | docs: record the async-persistence arm passing, closing the 155-case question |
| `f53f38048` | 2026-09-08 | zyang2 | docs: consolidate the Stage 2 record into a single file |
| `d288748f5` | 2026-09-09 | zyang2 | raft: cut the disk side, leaving a memory-only Raft (snapshots in memory) |
| `7c0479af0` | 2026-09-09 | zyang2 | docs: record the memory-only Raft branch |
| `3ba6edbc9` | 2026-09-09 | zyang2 | raft: reduce to Vote, AppendEntries and InstallSnapshot for a partitions-only model |
| `23b108454` | 2026-09-09 | zyang2 | docs: rewrite the memory-only Raft record as one coherent document |
| `241b43437` | 2026-09-10 | zyang2 | docs: plan a standalone Raft performance harness |
| `417731f00` | 2026-09-10 | zyang2 | docs: audit src/deptran/raft against the C++ to Rust readiness checklist |
| `52c2d6c62` | 2026-09-11 | zyang2 | raft: add a standalone Raft performance driver |
| `b7369ff7b` | 2026-09-11 | zyang2 | raft: launch raft_bench as a three-process cluster |
| `5137cd987` | 2026-09-11 | zyang2 | raft: sweep driver and record parser for the perf harness |
| `1486c0ac1` | 2026-09-11 | zyang2 | raft: saturation-curve and latency-CDF plots for the perf harness |
| `2585a128c` | 2026-09-11 | zyang2 | docs: the Raft performance harness manual and the workload profile it is built on |
| `3774ba937` | 2026-09-11 | zyang2 | raft: send the global site id, not the locale id, when requesting votes |
| `af6e65992` | 2026-09-11 | zyang2 | raft: sweep all three D5 configurations now that multi-group works |
| `906a1c3c6` | 2026-09-11 | zyang2 | raft: drop the last bug narrative from the sweep summary |
| `851c9cfe2` | 2026-09-12 | zyang2 | raft: make the perf harness able to state whether a change moved the number |
| `412c225a9` | 2026-09-12 | zyang2 | docs: document the regression criterion the harness now implements |
| `3ae871794` | 2026-09-12 | zyang2 | perf: record the pre-conversion Raft baseline, and correct two claims it falsifies |
| `c879dae80` | 2026-09-12 | zyang2 | docs: preserve the untracked migration notes and record a recovery plan |
| `c6086798a` | 2026-09-12 | zyang2 | docs: plan the Raft C++ refactor around what actually gates expressiveness |
| `ed82389ca` | 2026-09-12 | zyang2 | raft: make the perf harness an oracle for log integrity, not just speed |
| `d408d1f49` | 2026-09-12 | zyang2 | raft: wire RaftLabTest into CI, and make its verdict reach the exit code |
| `65205328c` | 2026-09-12 | zyang2 | raft/dsl: unlock the two gates that confined conversions to scalar predicates |
| `f060472e9` | 2026-09-12 | zyang2 | raft: convert ReplicationWakeGate to the DSL, and delete what was already dead |
| `340bd3d37` | 2026-09-12 | zyang2 | deptran: dissolve TxLogServer's implementation inheritance, and fix B1 with it |
| `b85c47e9f` | 2026-09-12 | zyang2 | scripts: stop regen_storage_dsl.sh --check from passing vacuously |
| `d2d395f94` | 2026-09-12 | zyang2 | docs: record what executing the Raft refactor plan actually produced |
| `4427129a9` | 2026-09-13 | zyang2 | raft: stop holding a dereferenced next_index_ cursor across InstallSnapshot |
| `482fc6546` | 2026-09-13 | zyang2 | deptran: include <mutex> where the mutex is declared, not transitively |
| `01d07f758` | 2026-09-13 | zyang2 | docs: correct the cpp_inherit finding, and record the multi-file constraint |
| `26e9107bd` | 2026-09-13 | zyang2 | deptran: record why TxLogServer's mutex was right before it was wrong |
| `43f436a17` | 2026-09-13 | zyang2 | docs: cross-carrier DSL references work; correct the claim that they do not |
| `5b5825659` | 2026-09-13 | zyang2 | deptran: convert TxLogServer to a DSL pub trait |
| `8fa80e798` | 2026-09-13 | zyang2 | docs: plan the RaftServer conversion outward from HeartbeatLoop |
| `149844219` | 2026-09-13 | zyang2 | docs: add the concurrency model and rrr's three inheritance patterns |
| `a6a7f92c7` | 2026-09-13 | zyang2 | docs: correct Step 1's container claim by running it |
| `313be117f` | 2026-09-13 | zyang2 | raft: extract HeartbeatAuthority, the first type carved out of HeartbeatLoop |
| `4a5969ada` | 2026-09-13 | zyang2 | raft: merge the per-peer index maps into one DSL-owned FollowerProgress |
| `1f28fd8f2` | 2026-09-13 | zyang2 | docs+test: close the two verification gaps, and correct the plan from execution |
| `39890ade4` | 2026-09-15 | zyang2 | raft: fix the commit-index defects in both heartbeat phases |
| `1cc23826f` | 2026-09-15 | zyang2 | raft: give both heartbeat phases one DSL-owned commit-index selector |
| `993393f55` | 2026-09-15 | zyang2 | raft: move the election timer loop itself into Rust |
| `fd49793dd` | 2026-09-15 | zyang2 | raft: fail the DSL gate when the transpiler silently drops a statement |
| `09f52e8f5` | 2026-09-15 | zyang2 | raft: move the heartbeat loop and its lifecycle into Rust |
| `7f7467995` | 2026-09-15 | zyang2 | raft: give Rust the heartbeat phase sequencing |
| `99765a06f` | 2026-09-15 | zyang2 | raft: give the peer-progress cluster to a DSL-owned dense table |
| `d3f2a935f` | 2026-09-15 | zyang2 | raft: delete the Timer that nothing reads |
| `fc2f7c929` | 2026-09-15 | zyang2 | raft: give the round scope to Rust, with access only through methods |
| `d165f4238` | 2026-09-15 | zyang2 | raft: give the read-index authority ledger and PHASE 3's scan to Rust |
| `cb0c4c207` | 2026-09-15 | zyang2 | raft: give the in-flight AppendEntries table to Rust, carrying the wire types |
| `7544d881a` | 2026-09-16 | zyang2 | raft: delete the dead code in server.{h,cc} |
| `761027be6` | 2026-09-16 | zyang2 | raft: rewrite the conversion plan for the RaftServer target, and start Step A |
| `659e166d5` | 2026-09-16 | zyang2 | raft: take mtx_ below the availability check in the InstallSnapshot callback |
| `09be9181d` | 2026-09-16 | zyang2 | raft: remove the recursive mutex |
| `d05852665` | 2026-09-16 | zyang2 | raft: give the mutex-guarded consensus cluster to a Rust type |
| `c94966626` | 2026-09-16 | zyang2 | raft: absorb the leadership and read-index clusters into RaftConsensusState |
| `7bf6d1f2d` | 2026-09-16 | zyang2 | raft: absorb the log cluster into RaftConsensusState |
| `d295a4842` | 2026-09-16 | zyang2 | raft: stop mutating committed log entries at send time |
| `3c6c739a1` | 2026-09-16 | zyang2 | raft: stop the log's read paths from creating entries |
| `c95621a33` | 2026-09-16 | zyang2 | raft: the log entry becomes a Rust-owned type |
| `2b359f186` | 2026-09-16 | zyang2 | raft: the log's container becomes a Rust-owned RaftLog |
| `cd8188d88` | 2026-09-16 | zyang2 | raft: delete min_active_slot_ and last_log_index_ |
| `bfb141422` | 2026-09-17 | zyang2 | docs: measure the log DSL conversion's performance, which regressed |
| `ae74b7648` | 2026-09-17 | zyang2 | raft: store the log in blocks, which removes the reallocation stall |
| `1d8edc6e3` | 2026-09-17 | zyang2 | raft: PHASE 0 of the heartbeat round becomes a Rust body |
| `95f9277c5` | 2026-09-17 | zyang2 | raft: PHASE 3 becomes a Rust body, and the commit decision is shared |
| `2a6ec1b7c` | 2026-09-17 | zyang2 | docs: measure which of RaftServer's remaining C++ is really concurrent |
| `fc023f0a6` | 2026-09-17 | zyang2 | raft: document the re-entrancy contract the mutex demotion created |
| `84c0a5c06` | 2026-09-17 | zyang2 | raft: PHASE 2's reply decision becomes a Rust body |
| `bda58df50` | 2026-09-17 | zyang2 | raft: make re-entering mtx_ abort with a message instead of hanging |
| `54fed1ad5` | 2026-09-17 | zyang2 | raft: catch every emitter TODO marker, not just the literal "// TODO:" |
| `f83537b7f` | 2026-09-17 | zyang2 | raft: gate the two DSL constructs that are lowered wrong in silence |
| `95d72c0a3` | 2026-09-18 | zyang2 | raft: move the log and the peer table into RaftConsensusState |
| `aac58e63e` | 2026-09-18 | zyang2 | raft: delete a dead member and finish migrating RaftServer's atomics |
| `125815145` | 2026-09-18 | zyang2 | raft: mirror this server's identity into RaftConsensusState |
| `24420a03b` | 2026-09-18 | zyang2 | docs: drop the migration notes that will not be read again |
| `f61383ed0` | 2026-09-18 | zyang2 | raft: OnRequestVote's body becomes Rust, and a signed/unsigned bug with it |
| `d4f7b4b2e` | 2026-09-18 | zyang2 | raft: OnAppendEntries' body becomes Rust |
| `09055daae` | 2026-09-18 | zyang2 | raft: lift out of RaftServer the three things a DSL struct cannot hold |
| `cfd311109` | 2026-09-18 | zyang2 | raft: gate the #[cpp_inherit] authentication footgun |
| `39eed1542` | 2026-09-18 | zyang2 | raft: RaftServer's state becomes a DSL struct (step E) |
| `b295d252a` | 2026-09-18 | zyang2 | raft: the lock guard, and the first thirteen bodies moved into Rust |
| `7f016868f` | 2026-09-18 | zyang2 | raft: rrr logging reaches the DSL, and twenty-seven more bodies move |
| `dc1ef5c97` | 2026-09-18 | zyang2 | raft: the kernel bridge, then setIsLeader and the election timer become Rust |
| `63f10fb44` | 2026-09-18 | zyang2 | raft: RequestVoteImpl becomes Rust, and #ifdef logs stop blocking conversions |
| `c5ea6348d` | 2026-09-18 | zyang2 | raft: startup, shutdown, submission and the apply queue become Rust |
| `25b1b95a0` | 2026-09-18 | zyang2 | raft: HeartbeatPhase2 becomes Rust, as a free function in the server.cc carrier |
| `79fce51c6` | 2026-09-18 | zyang2 | raft: the two heartbeat phases that were marshalling, and both inbound RPCs |
| `1e83ddebb` | 2026-09-18 | zyang2 | raft: HeartbeatPhase1's loop becomes Rust |
| `17c3e7af6` | 2026-09-18 | zyang2 | raft: SetupInternal and CreateSnapshotLocked become Rust |
| `ff7be1750` | 2026-09-18 | zyang2 | raft: OnInstallSnapshot and the apply thread's loop become Rust |
| `0e9b3ead5` | 2026-09-18 | zyang2 | raft: snapshot recovery becomes Rust |
| `0e27fe620` | 2026-09-18 | zyang2 | raft: the loops hold a typed server, and ten trampolines go away |
| `fddc21c7b` | 2026-09-18 | zyang2 | docs: the evidence for the 70% tranche |
| `be3e24a4b` | 2026-09-18 | zyang2 | raft: finish HeartbeatPhase1 -- the snapshot reply and the batch selection |
| `1786e01ea` | 2026-09-18 | zyang2 | raft: delete two opaque carriers, and put the round back in order |
| `071989b10` | 2026-09-18 | zyang2 | raft: the two RPC entry points hold a typed server, and eight kernels go |
| `bdd1dd9a8` | 2026-09-18 | zyang2 | raft: the apply queue moves into Rust, and the ODR pass stops mangling lambdas |
| `7855ebd85` | 2026-09-19 | zyang2 | raft: two more carriers become real Rust types, and the perf record is redone |
| `41648c4ba` | 2026-09-19 | zyang2 | raft: the apply queue and its epoch become one type inside one lock |
| `26ca518a3` | 2026-09-19 | zyang2 | raft: the opaque carriers stop lying about their size |
| `65e80b569` | 2026-09-19 | zyang2 | raft: the wake gate moves into RaftServerBase, and RaftServer holds no data |
| `252db7d9c` | 2026-09-19 | zyang2 | raft: the four wait methods become Rust; one reactor factory call is left |
| `8fac9f5f5` | 2026-09-19 | zyang2 | raft: the heartbeat round state becomes Rust, and the driver joins its phases |
| `f8b83c4e2` | 2026-09-19 | zyang2 | raft: the four election-timeout getters become one call returning the set |
| `34ce28b87` | 2026-09-19 | zyang2 | raft: the batch buffer becomes a Rust Vec, and stops being copied wholesale |
| `39f07d140` | 2026-09-19 | zyang2 | raft: the lock guards declare their destructors non-throwing |
| `68fe87ce1` | 2026-09-19 | zyang2 | raft: the per-slot log lookup becomes Rust, and FindRaftInstance goes |
| `244b9523c` | 2026-09-19 | zyang2 | raft: PHASE 1's payload selection stops asking C++ where the log is |
| `a124990d7` | 2026-09-19 | zyang2 | raft: shutdown and the apply-thread start become Rust |
| `37d308ed4` | 2026-09-19 | zyang2 | raft: doVote becomes Rust, and server.cc loses four banners describing nothing |
| `b97ecc24b` | 2026-09-19 | zyang2 | docs: the conversion log, and a structural map of what is Rust and what is C++ |
| `baf4a4371` | 2026-09-19 | zyang2 | docs: correct the concurrency section -- there is no global poll thread |
| `040d448c5` | 2026-09-19 | zyang2 | raft: take the callback-lifetime mutex below the availability check |
| `c550c1dff` | 2026-09-19 | zyang2 | docs: rewrite the structural map after five verification passes found 31 errors |
| `9aad32d4b` | 2026-09-19 | zyang2 | docs: the production Mako+Raft path passes all four replication suites |
| `7c7f55a87` | 2026-09-20 | zyang2 | raft: ten try/catch blocks become one exception boundary and a noexcept parse |
| `e4963b7ae` | 2026-09-20 | zyang2 | docs: claim 2 is no longer a blocker |
| `8a347e95c` | 2026-09-20 | zyang2 | docs: bring the structural map up to the exception-boundary change |
| `c4e56891c` | 2026-09-20 | zyang2 | raft: the env parse becomes Rust error handling, not a C++ helper |
| `19cfbb213` | 2026-09-20 | zyang2 | docs: claim 1a had the vtable backwards -- it is the weak half, not the strong |
| `526595eb3` | 2026-09-20 | zyang2 | docs: retire the migration write-ups; one plan from here to rustc-compiled Raft |
| `c83a34bc0` | 2026-09-20 | zyang2 | raft: step A -- RaftSpecific: TxLogServer, implemented for RaftServerBase |
| `738a8b7f2` | 2026-09-20 | zyang2 | raft: step B -- no code recovers the concrete server from a base pointer |
| `eb5482070` | 2026-09-20 | zyang2 | raft: step C1a -- nineteen kernels stop reaching into the struct |
| `90b6f828a` | 2026-09-20 | zyang2 | raft: step C1b -- the log, the batch, the decode and the config are Rust's |
| `bcc298560` | 2026-09-20 | zyang2 | raft: step C2 -- the lab suite and the test worker hold the layout of nothing |
| `2996cc572` | 2026-09-20 | zyang2 | raft: step C3 -- the shim constructor is one call; step C is done |
| `1a5f1c9c0` | 2026-09-20 | zyang2 | raft: paired-trial harness, and the plan's step C hash |
| `5acdad280` | 2026-09-21 | zyang2 | docs: A..C3 carries no detectable throughput cost -- 25 paired trials |
| `204751587` | 2026-09-21 | zyang2 | raft: step D1 -- nothing non-trivial crosses the boundary by value |
| `e60267d97` | 2026-09-21 | zyang2 | raft: step E -- Rust stays on the fiber; the constraints are written where they bind |
| `1d24bbf53` | 2026-09-21 | zyang2 | raft: step F1a -- the C ABI over the struct, written in Rust |
| `782b34f48` | 2026-09-21 | zyang2 | raft: step F1b -- the shim holds a pointer and forwards; it derives from nothing of the struct |
| `3cbcdcfe6` | 2026-09-21 | zyang2 | raft: step F1c -- hand-written C++ calls no method of the struct; F1 is done |
| `3b0d810dd` | 2026-09-21 | zyang2 | docs: F1 is done; F2 inventoried from every rusty:: path the crate uses |
| `1085fe9a9` | 2026-09-21 | zyang2 | docs: F1 carries no detectable throughput cost -- 25 paired trials; the log and CLAUDE.md catch up |
| `8b1cbfe97` | 2026-09-21 | zyang2 | raft: F2 slice 1 -- facade -> runtime: the crate is a staticlib and every carrier owns its C++ object |
| `87ce66914` | 2026-09-21 | zyang2 | raft: F2 slice 1c -- the Raft crate's 44 logging functions are a logger, not stubs |
| `424fe4c79` | 2026-09-21 | zyang2 | raft: F2 slice 2 -- the wake gate is Rust's: construction, reservation, and the job the reactor runs |
| `5e97e4884` | 2026-09-21 | zyang2 | raft: F2 slice 3 -- nothing crosses the seam as a C++ object, and the cutover's preconditions |
| `1de45affa` | 2026-09-21 | zyang2 | raft: F2.6 -- the cutover: the Raft server is compiled by rustc and linked as libraft.a |
| `7de35c702` | 2026-09-21 | zyang2 | docs: the cutover carries no detectable throughput cost -- 25 paired trials; F2 is done in the plan |
| `9f3f350ae` | 2026-09-21 | zyang2 | raft: F2.7 -- Rust calls Rust: the two RPC forwarder kernels and their exports are gone |
| `80433a59d` | 2026-09-21 | zyang2 | docs: F2.7's hash and the tree's measured numbers after it (95 kernels, server.cc 1,441 lines); the canonical-source constraint restated |
| `b0f692e64` | 2026-09-23 | Zeyu Yang | raft: the lab predicate is a cargo feature, not a C++ kernel |
| `4d74f8bcb` | 2026-09-23 | Zeyu Yang | raft: the lab cluster registers itself, with no new ABI |
| `0250cabb4` | 2026-09-23 | Zeyu Yang | raft: the lab fixture's invariant readers, in Rust and checked against the C++ |
| `5df4650ae` | 2026-09-23 | zyang2 | raft: Phase 3a -- the eleven replication lab cases are Rust |
| `b179c5f92` | 2026-09-23 | zyang2 | raft: Phase 3b -- all 25 lab cases pass under the Rust harness |
| `80f715dad` | 2026-09-23 | zyang2 | raft: Phase 4 -- the C++ lab harness and 41 of the 73 exports are deleted |
| `9ad3870a0` | 2026-09-23 | zyang2 | raft: follow the harness deletion through the comments and the borrow filter |
| `6f367f0bb` | 2026-09-23 | zyang2 | raft: comments describe the code, not the route the code took here |
| `cb8e4e6b3` | 2026-09-24 | zyang2 | srpc: fix two executable defects the C++ lane never exercised |
| `bbbd51d89` | 2026-09-24 | zyang2 | raft: the crate can take the Rust srpc lane; what still blocks linking both |
| `b21c36431` | 2026-09-24 | zyang2 | srpc-cmake: take the kernel list from srpc's manifest instead of repeating it |
| `bd42028ab` | 2026-09-24 | zyang2 | scripts: re-baseline mako's srpc canaries against the pulled subtree |
| `4a8e233eb` | 2026-09-24 | zyang2 | scripts: pin srpc's C boundary as a computed set, not four hand-listed copies |
| `179182a10` | 2026-09-25 | zyang2 | srpc: move rusty-rustc and rusty-cpp-markers out of the subtree |
| `95d085999` | 2026-09-25 | zyang2 | raft: retire cpp_value_init; the zero-init guarantee moves to the field census |
| `66d1a96c7` | 2026-09-25 | zyang2 | deptran, mako: follow srpc's API forward after the subtree pull |
| `157bc8837` | 2026-09-25 | zyang2 | srpc gate: reconcile every pinned expectation with the pulled subtree |
| `49defdf5e` | 2026-09-25 | zyang2 | docs: record the srpc adoption, and two refuted plans with it |
| `b0ee6d2e1` | 2026-09-25 | zyang2 | docs: preserve the earlier Raft/RPC investigations |
| `561670f76` | 2026-09-25 | zyang2 | raft: generate the Rust RPC slice, and let the two lanes link |
| `493881a8e` | 2026-09-25 | zyang2 | raft: generate the service dispatch and client proxy too |
| `c0706bc73` | 2026-09-25 | zyang2 | raft: generate AppendEntries too -- the payload needs no wire change |
| `82782a558` | 2026-09-25 | zyang2 | docs: record that 2c needs no wire break, and that stage 3 splits 3/2 |
| `b86f2dc81` | 2026-09-25 | zyang2 | rpcgen_rust: copy Copy requests into the proxy closure, do not clone |
| `66776cfed` | 2026-09-25 | zyang2 | tests: the canonical-source census drops by one, and why |
| `fa5a2b33a` | 2026-09-25 | zyang2 | raft: add the Rust RaftCommo landing pad -- not yet a port |
| `284860c8f` | 2026-09-25 | zyang2 | raft: drop commo_ from the server, and move the RPC gate into it |
| `156f0ac5a` | 2026-09-25 | zyang2 | docs: stage 3 is done -- what moved, what measured, and what did not |
| `41bc18569` | 2026-09-25 | zyang2 | build: generate rpc.rs from rcc_rpc.rpc instead of trusting a snapshot |
| `a353e30b4` | 2026-09-25 | zyang2 | raft: make the lab build clippy-clean so the gate can cover it |
| `a4c614ca8` | 2026-09-25 | zyang2 | docs: correct 3a -- a lane move replaces Raft's reactor, it does not add one |
| `08d619f66` | 2026-09-25 | zyang2 | build: watch srpc's Rust sources for the libraft rebuild (stage 0d) |
| `5d3591c95` | 2026-09-25 | zyang2 | deptran: Communicator's data becomes one Rust type, written once for both lanes |
| `f4a11eb53` | 2026-09-25 | zyang2 | docs: finish the plan -- close stages 0-3, open 3e, retire the stale prose |
| `5bcf6c32d` | 2026-09-25 | zyang2 | docs: superseded plan items should not render as open checkboxes |
| `18012740c` | 2026-09-25 | zyang2 | raft: delete the unused Rust commo; PeerRegistry superseded it |
| `ad19d6cc7` | 2026-09-25 | zyang2 | build: one copy of srpc's C kernels, compiled by one compiler (0b, 0c) |
| `458d75b5b` | 2026-09-25 | zyang2 | rpcgen_rust: the wire ids get a home that outlives the header (2b) |
| `6fd0da9cc` | 2026-09-25 | zyang2 | raft: the RPC service, on the Rust srpc lane (3e, first half) |
| `faff958b2` | 2026-09-25 | zyang2 | docs: close 2b, halve 3e, and correct two measurements 3a got wrong |
| `db1d21ff0` | 2026-09-25 | zyang2 | docs: tick 0b and 0c -- fixed in ad19d6cc7, the boxes were left open |
| `6e76caec9` | 2026-09-25 | zyang2 | raft: the RPC transport and all three send paths, in Rust (3e, second half) |
| `ea455e961` | 2026-09-25 | zyang2 | docs: a one-page C++/Rust correspondence table |
| `2a0deb21f` | 2026-09-25 | zyang2 | raft: prove the Rust send path over TCP, and fix the split-brain it exposed |
| `6de31e23a` | 2026-09-25 | zyang2 | docs: correct the kernel justification -- most are positional, not required |
| `d93451ecd` | 2026-09-25 | zyang2 | raft: the transport owns its server, and gets a C ABI (3e) |
| `551ee1a49` | 2026-09-25 | zyang2 | docs: enumerate 3e's cutover site by site, so it is executable |
| `93f818a53` | 2026-09-25 | zyang2 | raft: resolve the transport by server identity, and reroute the first site |
| `a8691d647` | 2026-09-25 | zyang2 | docs: give every open item a measured reason, not a dependency note |
| `2087d841b` | 2026-09-25 | zyang2 | docs: fix the malformed tables, and stop calling untimed claims "measured" |
| `ee9747171` | 2026-09-25 | zyang2 | docs: the plan was rendering as code blocks -- fix the indentation |
| `59b4708f1` | 2026-09-25 | zyang2 | docs: the converted tables read badly -- rewrite them as nested lists |
| `edd49ff35` | 2026-09-25 | zyang2 | docs: generate the correspondence file; a workflow found every number wrong |
| `b1ccc9104` | 2026-09-25 | zyang2 | docs: apply the audit to the plan -- 15 corrections, one of them mine |
| `49341b986` | 2026-09-25 | zyang2 | docs: the last three audit findings -- and the same error was in code comments |
| `538df5f8c` | 2026-09-25 | zyang2 | docs: clear the last ten audit findings, and stop the generator self-staling |
| `558374062` | 2026-09-26 | zyang2 | docs: the Raft RPC path on the pure Rust lane, call and response |
| `087df006d` | 2026-09-26 | zyang2 | docs: correct the Rust-lane call path -- Start does more, and touches C++ |
| `65754b66f` | 2026-09-26 | zyang2 | docs: two-lane Raft plan (revision 5), its validation, and the latency-regression evidence |
| `edc5db890` | 2026-09-26 | zyang2 | srpc reactor: evict timed-out events from the polling queues again |
| `4149bf438` | 2026-09-26 | zyang2 | perf: record R3 -- the reactor fix restores the pre-regression latency |
| `217cc26d4` | 2026-09-26 | zyang2 | raft: two lanes -- the core on the Rust srpc runtime (MAKO_RAFT_LANE=rust) |
| `b6141e723` | 2026-09-26 | zyang2 | Large payloads: bound AppendEntries batches by bytes; fix two srpc hot paths |
| `9a361eccd` | 2026-09-26 | zyang2 | raft-rt: no payload copies on the Rust lane; T3 and T4 measured |
| `2cd7f5e74` | 2026-09-26 | zyang2 | raft: Rust lane is the default; full sweep, S1/S2, a lab that cannot be hollow |
| `b73936735` | 2026-09-26 | zyang2 | srpc: keep timed-out events off the reactor's wait list (R4) |
| `c23e397e4` | 2026-09-27 | zyang2 | raft: the transpiled C++ lane (MAKO_RAFT_LANE=cpp), plan phases L2-L4 |
| `21a5d7a99` | 2026-09-27 | zyang2 | docs: plan revision 7 -- Phase N, the snapshot on the Rust lane |
| `b7ac74a0b` | 2026-09-27 | zyang2 | raft: N0 -- a snapshot-capable raft_bench, before any conversion |
| `a6ba19ee9` | 2026-09-27 | zyang2 | raft: N3-N5 -- the Rust lane's snapshot store, and InstallSnapshot on it |
| `39950ea3e` | 2026-09-27 | zyang2 | raft: N6 -- InstallSnapshot resend suppression on the Rust lane |
| `a22d391cc` | 2026-09-27 | zyang2 | raft: N7-N8 -- startup-recovery lab cases, and raft-rt's tests gate the build |
| `0225bb198` | 2026-09-27 | zyang2 | docs: N13 -- the snapshot store as it now is; plan status through N9 |
| `fffab9338` | 2026-09-27 | zyang2 | raft: N10-N11 done; a rotated multi-arm trial for N12 |
| `dd3c0ca9c` | 2026-09-28 | zyang2 | raft: N12 -- snapshot-enabled performance of the Rust-lane store |
| `000c41840` | 2026-09-28 | zyang2 | docs: N12 -- the pre arm's segfault is a use-after-free in N0's install send |
| `8a5d45db8` | 2026-09-29 | zyang2 | docs: N12 -- the raw records now live in ~/raft-test-results on zoo-003 |
| `b83630193` | 2026-09-29 | zyang2 | docs: the Raft project structure, and Paxos/Raft inheritance |
| `91277b4ac` | 2026-09-29 | zyang2 | docs: where a Raft entry's latency goes on the Rust lane |
| `89590677a` | 2026-09-29 | zyang2 | docs: conversion log through 09-29 -- phases 13-16 |
| `49aadf2ec` | 2026-09-29 | zyang2 | docs: the Jetpack test methodology, and whether it fits Mako's Raft |
| `26a9b008d` | 2026-09-29 | zyang2 | docs: add Rolis, MongoDB NSDI '21 and PODC '19 to the methodology report |
| `6d631055f` | 2026-09-29 | zyang2 | docs: the maximal-copy plan for running jetpack's perf test on Mako's Raft |
| `3d14557d8` | 2026-09-29 | zyang2 | docs: approved plan -- Rust vs C++ baseline Raft under Jetpack's test suite |
| `8a50533f4` | 2026-09-29 | zyang2 | raft_bench: Jetpack test mode, copied Jetpack scripts, WAN delay shim |
| `074ad88a9` | 2026-09-29 | zyang2 | raft_perf/jetpack: drive Jetpack's unedited scripts over the sweep |
| `dac2ea2d6` | 2026-09-30 | zyang2 | docs: Rust vs C++ baseline Raft under Jetpack's test suite |
| `8e4288b31` | 2026-09-30 | zyang2 | docs: say why timing to the leader's apply matches Raft's reply point |
| `933c0d090` | 2026-09-30 | zyang2 | docs: Raft entry size vs latency analysis |
| `1a346fde4` | 2026-09-30 | zyang2 | docs: delete 11 superseded Raft migration notes |
| `a6d677ef8` | 2026-09-30 | zyang2 | verus: prove the leader's commit rule (model of server_h.rs:881, server_cc.rs:633) |
| `3820650e5` | 2026-09-30 | zyang2 | docs/verus: single-thread and Verus-fit assessment of the Rust-lane Raft |
| `76ea6cb46` | 2026-10-01 | zyang2 | docs/verus: what the ghost-log refinement method needs, from its repo |
| `03b4c7eb6` | 2026-10-01 | zyang2 | docs/verus: modification plan for ghost-log refinement of the Rust-lane Raft |
| `33b88b564` | 2026-10-02 | zyang2 | tests: raft_messages checks padding under value-init, not brace-init |
| `6f97e50e8` | 2026-10-02 | zyang2 | build: take the import-std token from the running cmake, not a version table |
| `2997a466b` | 2026-10-02 | zyang2 | ci: mark the checkout safe for git and provide ripgrep in the container jobs |
| `dfb57370c` | 2026-10-02 | zyang2 | ci: vouch for the inspected directories instead of trusting every repo |
| `c1360f12b` | 2026-10-02 | zyang2 | srpc tests: is_some() instead of !is_none() (clippy 1.91 nonminimal_bool) |
| `8fc195615` | 2026-10-02 | zyang2 | docs: remove the AI-written Raft plan documents and the early paired-trial CSVs |
| `150be3e3b` | 2026-10-03 | zyang2 | docs/verus: revise the modification plan with the user's decisions |
| `fe9763f80` | 2026-10-03 | zyang2 | raft: Phase 0 instrumentation for the Verus plan (trace kit, lab view, failover timing) |
| `283e8a798` | 2026-10-03 | zyang2 | scripts/verus: Phase 0 gate scripts; forward GROUP in the perf trials |
| `99bda8b79` | 2026-10-03 | zyang2 | raft/verus/spec: freeze spec v1 (upstream d7e04ed7 + S1) |
| `571c07301` | 2026-10-03 | zyang2 | docs/verus: bug log, diff ledger, spec changes, Phase 0 report (in progress) |
| `b6e4aacb4` | 2026-10-03 | zyang2 | scripts/verus: g7.sh, the G7 election/failover runner |
| `c0197443f` | 2026-10-03 | zyang2 | docs/verus: lab comparator calibration (stop point 8), V3 reading, B1 is pinned |
| `5033ab728` | 2026-10-03 | zyang2 | docs/verus: bug-log rule in the plan; less testing between phases |
| `c5fa64ec5` | 2026-10-03 | zyang2 | raft: one RaftCore struct (Phase 1, M1/M2/M9) |
| `b75f285e2` | 2026-10-03 | zyang2 | raft-rt: count each voter once per campaign (F1) |
| `372b73e6f` | 2026-10-03 | zyang2 | raft: phase 1 re-checks leadership where it builds the append (F3) |
| `a38c5012e` | 2026-10-03 | zyang2 | raft: refuse AppendEntries entries whose term is below 1 (F4) |
| `771d238c5` | 2026-10-03 | zyang2 | raft: check the verified configuration at setup (F5) |
| `15160ea4d` | 2026-10-03 | zyang2 | raft lab: pin the unavailable voter's reply (case 13) |
| `590370719` | 2026-10-03 | zyang2 | docs/migration: regenerate the Raft correspondence after Phase 1 |
| `7bfa5e3cf` | 2026-10-03 | zyang2 | docs/verus: Phase 0 report complete; A/A bounds for G4-G7 |
| `a33064cc6` | 2026-10-03 | zyang2 | docs/verus: Phase 1 report, diff ledger, bug log |
| `3ab039029` | 2026-10-03 | zyang2 | raft: raft_verify and explicit panics become assert! (Phase 2, M10) |
| `4a8d2dc81` | 2026-10-03 | zyang2 | raft: RaftEntry caches its command's metadata (Phase 2, M6) |
| `2d657ae4c` | 2026-10-03 | zyang2 | raft: side effects become returned actions (Phase 2, M1/M3/M4) |
| `a85a19993` | 2026-10-03 | zyang2 | raft: fire the leader-change callback after mtx_ is released (F6) |
| `26362c622` | 2026-10-03 | zyang2 | docs/verus: Phase 2 report, diff ledger, bug log |
| `937a34cd8` | 2026-10-03 | zyang2 | raft: the heartbeat round as three core calls (Phase 3, M5; F7) |
| `496db6cd2` | 2026-10-03 | zyang2 | raft: the inbound RPC handlers as core calls (Phase 3, M5; M11) |
| `25aacb728` | 2026-10-03 | zyang2 | raft: the campaign as core calls, the tally counted by the core (Phase 3, M5; F1) |
| `8009d1aa7` | 2026-10-03 | zyang2 | docs/verus: coupling table v1, Phase 3 ledger, report (Tier 1) |
| `3deca501e` | 2026-10-03 | zyang2 | docs/verus: Phase 3 performance checkpoint (G2 fails) |
| `059531a77` | 2026-10-03 | zyang2 | docs/verus: Phase 3 G2 investigation (measurement only) |
| `2a50f7eff` | 2026-10-04 | zyang2 | verus: G2 gates on the traced two-follower round time (user decision) |
| `c62fc2519` | 2026-10-04 | zyang2 | raft: role, leader hint and commit index as lock-free mirrors (Phase 4, F8) |
| `366b25935` | 2026-10-04 | zyang2 | raft: the applied index through the core; the preferred leader a shell atomic (Phase 4) |
| `22a54332c` | 2026-10-04 | zyang2 | raft: no log compaction or snapshots under the verified gates (Phase 4, F5) |
| `7bc43e7e7` | 2026-10-04 | zyang2 | docs/verus: Phase 4 ledger, report, B14; the core-access census |
| `43c57e3ac` | 2026-10-04 | zyang2 | raft: remove the hybrid and cpp lanes from this branch (Phase 6 start, Q9) |
| `896cdfc85` | 2026-10-04 | zyang2 | raft: the core becomes its own crate, raft-core (Phase 6, M1, M7, M11) |
| `53bb170b4` | 2026-10-04 | zyang2 | raft-core: seven modules verify under Verus (Phase 6, panic/overflow freedom) |
| `2fc0dcdba` | 2026-10-04 | zyang2 | raft-core: the log verifies, layout and index arithmetic (Phase 6) |
| `92f571d3b` | 2026-10-04 | zyang2 | raft-core: RaftCore's invariant; node.rs verifies but the append handler (Phase 6) |
| `1121af91e` | 2026-10-04 | zyang2 | raft-core: drop an expression attribute plain cargo rejects |
| `0166c0a89` | 2026-10-04 | zyang2 | raft-core: SiteSet gets set semantics; the ledger and round keep it (Phase 6) |
| `de877ebc6` | 2026-10-04 | zyang2 | raft-core: the authority ledger's round ids, specified (Phase 6) |
| `cb15e7c62` | 2026-10-04 | zyang2 | raft-core: RaftCore's invariant covers peers, rounds and epochs (Phase 6) |
| `b269b4589` | 2026-10-04 | zyang2 | raft-core: the append handler verifies; the trusted surface is blocks_for (Phase 6) |
| `192e15c39` | 2026-10-04 | zyang2 | raft-core: the heartbeat round verifies; every core module is under Verus (Phase 6) |
| `62a237637` | 2026-10-04 | zyang2 | raft: the snapshot threshold and callback tokens leave RaftCore (Phase 6, M1) |
| `7aa01a586` | 2026-10-04 | zyang2 | raft-core: Setup's identity, membership and gate are core calls (Phase 6, F5) |
| `4962bd6d8` | 2026-10-04 | zyang2 | raft-core: step(Event) is the core's one entry point; the shell calls nothing else (Phase 6) |
| `affc46aa7` | 2026-10-04 | zyang2 | raft: the replay recorder, and raft-replay (Phase 6, plan A.4, M0) |
| `9665c8f3e` | 2026-10-04 | zyang2 | raft-core: step_checked drops what no genuine peer sends (Phase 6, F9) |
| `3d768bdcf` | 2026-10-04 | zyang2 | scripts/verus: the ledger lint; every core line is ghost, moved or labelled (Phase 6) |
| `0986e4b7f` | 2026-10-04 | zyang2 | scripts/verus: core_check.sh runs the ledger lint (Phase 6) |
| `ce967f6f7` | 2026-10-04 | zyang2 | docs: the correspondence doc after F9; core_check.sh checks its freshness |
| `51d100d1f` | 2026-10-04 | zyang2 | docs/verus: Phase 6 report and ledger; the coupling table's open work |
| `dfe6ba6d8` | 2026-10-04 | zyang2 | raft-core: the proof side imports spec v1 (Phase 8 start) |
| `0feae5ac7` | 2026-10-04 | zyang2 | raft-core: RaftLog's view, the entries in index order (Phase 8) |
| `274a72654` | 2026-10-04 | zyang2 | scripts/verus: core_check.sh runs the ghost-only lint from Phase 8 (plan's diff-2 lint) |
| `5f97ae6fe` | 2026-10-04 | zyang2 | raft-core: the coupling's foundation; Setup closes LoadConfig (Phase 8) |
| `426eb7e1d` | 2026-10-04 | zyang2 | scripts/verus: the ledger lint ends a proof fn's head at the body's brace |
| `6b5030ff4` | 2026-10-04 | zyang2 | raft-core: stutters, StepAside and ClientRequest keep ginv (Phase 8) |
| `4d8357131` | 2026-10-04 | zyang2 | scripts/verus: the ledger lint reads clause blocks and multi-line ghost lets |
| `033078d5b` | 2026-10-04 | zyang2 | raft-core: an inbound RequestVote is StepDown, then GrantVote or RejectVote (Phase 8) |
| `cc73145a0` | 2026-10-04 | zyang2 | raft-core: a started campaign is LTimeout (Phase 8) |
| `e2f63615e` | 2026-10-04 | zyang2 | raft-core: a settled campaign is ReceiveVoteGranted, BecomeLeader, StepDown or StepAside (Phase 8) |
| `a09862d2f` | 2026-10-04 | zyang2 | scripts/verus: the ledger lint stops at a bodiless spec fn, and reads Verus-only modules as ghost |
| `762573fc4` | 2026-10-04 | zyang2 | raft-core: an inbound AppendEntries is StepDown, then RejectAppendEntries or one FollowerAppendEntries per component (Phase 8) |
| `1d8babc9c` | 2026-10-04 | zyang2 | raft-core: V2 in the coupling invariant; the peer table's match indices and majority count specified (Phase 8) |
| `e60880223` | 2026-10-04 | zyang2 | raft-core: a heartbeat reply is StepDown, HandleAppendResponse or unseen (Phase 8) |
| `37319d389` | 2026-10-04 | zyang2 | raft-core: the commit advance is AdvanceCommitIndex; bugs-found B17 (Phase 8) |
| `ba5e0ed1b` | 2026-10-04 | zyang2 | raft-core: a heartbeat tick's AppendEntries are SendAppendEntries, one per component (Phase 8) |
| `998b86820` | 2026-10-04 | zyang2 | raft-core: step keeps the coupling; the per-node certificate and the cluster theorem (Phase 8) |
| `7220c62b9` | 2026-10-04 | zyang2 | docs/verus: the host contract; Phase 8's ledger rows; the coupling table's departures |
| `3a90e56c8` | 2026-10-04 | zyang2 | docs/verus: Phase 8 report; B17 reproduced at the core, its fix tried; B17 and B16 made exact |
| `9f95bd6da` | 2026-10-04 | zyang2 | docs/verus: B16 and B17 recorded, not fixed (the user's decision) |
| `d22fc4043` | 2026-10-04 | zyang2 | docs/verus: code-structure.md; B17's reachability corrected; B18 recorded |
| `61ab0f462` | 2026-10-04 | zyang2 | docs/verus: B19 recorded, every thread holds its own &mut RaftServerBase |
| `459c87c83` | 2026-10-04 | zyang2 | docs/verus: code-structure.md §11, what the verification work added |
| `d9c2cb245` | 2026-10-06 | zyang2 | docs/verus: B4's fate corrected; F4 fixes only part of it |
| `958cf8d64` | 2026-10-06 | zyang2 | docs/verus: disk persistence on verus-raft, a design and what Verus adds |
| `34a4bd6b3` | 2026-10-06 | zyang2 | docs/verus: disk persistence is simulated, on tmpfs, never on the NFS home |
| `124ee86f1` | 2026-10-06 | zyang2 | docs/verus: disk persistence, revised with the owner's follow-up questions |
| `a586a7f51` | 2026-10-06 | zyang2 | raft-core: the round end commits only while leading (F12, B17); reply and round-end premises weakened (B18) |
| `49efd82c1` | 2026-10-06 | zyang2 | docs/verus: diff ledger rows for F12 and the B18 premises |
| `924a4dfdd` | 2026-10-06 | zyang2 | raft: Start refuses a command without a value (F13, B16) |
| `21c287832` | 2026-10-06 | zyang2 | raft: the AppendEntries decoder refuses an entry later than the leader's term (F14, B4) |
| `ad7af330e` | 2026-10-06 | zyang2 | raft: a campaign is lost as soon as a yes quorum is out of reach (F15, B1) |
| `00881d428` | 2026-10-06 | zyang2 | raft-replay: drop two redundant borrows in a test's format! (clippy --all-targets) |
| `cd46bbf20` | 2026-10-06 | zyang2 | raft: dead stale-publication check removed (F16, B8); heartbeat interval an atomic (F17, B14); get_outstanding_logs counts the worker's own uncommitted submissions (F18, B7) |
| `74dab7c9a` | 2026-10-06 | zyang2 | raft: no thread holds &mut RaftServerBase (F19, B19) |
| `0633e1ffc` | 2026-10-06 | zyang2 | docs/verus: the bug-fix phase's records (F12-F19) |
| `a4b1eaa02` | 2026-10-06 | zyang2 | docs/verus: the bug-fix phase's report |
| `84cb901f7` | 2026-10-09 | zyang2 | docs: remove the performance results, phase reports and session notes |
| `da46ad7b4` | 2026-10-09 | zyang2 | srpc on Lion: mako-dev's srpc gate, plus pins for our local srpc patches |
| `44d07a3ee` | 2026-10-09 | zyang2 | docs: srpc line citations after the Lion merge |
| `78c29dffe` | 2026-10-09 | zyang2 | docs: Raft disk persistence design and work plan, cited at 44d07a3ee |
| `fb1ff2c58` | 2026-10-09 | zyang2 | raft-store: the WAL, crash points, atomic creation and the flusher (plan P2) |
| `5c1e5abaa` | 2026-10-09 | zyang2 | raft-core: persist note, ObserveTerm and Restore, with Restore coupled (disk plan P1, P9) |
| `4b891e2d2` | 2026-10-09 | zyang2 | raft disk mode: WAL, held outputs, recovery, RocksDB base, snapshot files, kill tests (disk plan P0, P3-P8) |
| `6c1acb9eb` | 2026-10-10 | zyang2 | raft: rename the shell crate's src/ to shell/ |
| `c83d55711` | 2026-10-10 | zyang2 | raft disk: B32 range deletes, collect fixes (F11a), stats, and the cost model checked |
| `81156ba65` | 2026-10-10 | zyang2 | raft disk: drain closes Raft's gate, majority start, kill-test evidence and checks 7-9, restart test |
| `037d4dd0c` | 2026-10-10 | zyang2 | verus gate: catch every trust spelling; docs: Restore's witness, premises, recovery as built |
| `21510dda2` | 2026-10-10 | zyang2 | raft kill tests: checks that cannot pass on bad evidence, deaths and stops checked, node beside its driver |
| `b640f413e` | 2026-10-10 | zyang2 | raft-store: recovery never leaves a store without a segment; review fixes and tests |
| `6b6a9b978` | 2026-10-10 | zyang2 | raft disk shell, rt, core: review fixes |
| `b278213e3` | 2026-10-10 | zyang2 | raft: delete dead files; test programs live with their drivers |
| `dc2c52728` | 2026-10-10 | zyang2 | raft: delete the dead C++ lane |
| `0c650e720` | 2026-10-10 | zyang2 | docs: Raft's one lane and where its test programs live |
| `d5bbb57e4` | 2026-10-10 | zyang2 | docs: regenerate the correspondence after the dead-lane removal |
| `02b17cf6a` | 2026-10-10 | zyang2 | raft: keep commo.h's live types; only RaftCommo was dead |
| `09e86728d` | 2026-10-11 | zyang2 | raft-store: write the crash line in one write; record CP7 |
| `5f4d964a1` | 2026-10-11 | zyang2 | raft: delete the C++ lane's leftovers; comments describe one lane |
| `83b6e56dd` | 2026-10-11 | zyang2 | docs: drop the disk work plan; the design describes disk mode as built |
| `27f0d900f` | 2026-10-11 | zyang2 | raft: delete the lane files; the transport header is raft_transport.h |
| `b10d56745` | 2026-10-11 | zyang2 | raft_perf: jetpack PROVENANCE names raft_bench_jetpack.h where it lives now |
| `4eec61324` | 2026-10-11 | zyang2 | docs: raft-book §11 describes disk mode; ci.sh's lab comment states no case count |
| `d658f5231` | 2026-10-13 | zyang2 | srpc, raft: the shutdown drain is a barrier (B23, B34) |
| `efa0633c8` | 2026-10-13 | zyang2 | raft-store: sync the store's whole path; free a segment only after a durable rotation (B35, B37) |
| `94cbfdd78` | 2026-10-13 | zyang2 | raft: no undefined behaviour in the RPC decoders or at embedder callbacks (B36, B38, B39) |
| `d9c230858` | 2026-10-13 | zyang2 | ci: a Raft store directory per shard in the 2-shard suites (B40) |
| `4c4fb9d28` | 2026-10-13 | zyang2 | docs: bugs-found B34-B40 from the PR #92 review and its validation; B23 fixed |
| `a41247aea` | 2026-10-15 | zyang2 | srpc reactor: a timed wait registers its deadline once it is WAIT (B54) |
| `3f721248a` | 2026-10-19 | zyang2 | raft: harden the wire and the store (B41-B52) |
| `c0b6af171` | 2026-10-19 | zyang2 | raft: pipeline AppendEntries over a long round trip (F23, B53) |
| `9088b8a69` | 2026-10-19 | zyang2 | verus: move the Raft spec to v3 (upstream 1bb2f142) |
| `feea8a0c4` | 2026-10-19 | zyang2 | raft: snapshots and restore become core steps (F24-F30, B55, B56) |
| `ec6753087` | 2026-10-19 | zyang2 | verus: persistence certificates and Ready marks (Part A, P1) |
| `fa95ceeb8` | 2026-10-19 | zyang2 | verus: the crash cut and Restore's premise (Part A, P3, P4) |
| `95e15587e` | 2026-10-19 | zyang2 | verus: spec v4 (W1) and the log's ghost prefix (Part B, stage 1) |
| `ca1ad3409` | 2026-10-19 | zyang2 | verus: the coupling holds over a compacted log (Part B, stage 2) |
| `c4b1e66ca` | 2026-10-19 | zyang2 | verus: snapshot creation inside the gate is a stutter (Part B, stage 3) |
| `fe8401a1f` | 2026-10-19 | zyang2 | verus: the leader's InstallSnapshot is LSendSnapshot (Part B, stage 4) |
| `ef0572962` | 2026-10-19 | zyang2 | verus: the install's reply is coupled inside the gate (Part B, stage 5a) |
| `e52f6f721` | 2026-10-19 | zyang2 | verus: the follower's install segments (Part B, stage 5b, log side) |
| `326bbd4ef` | 2026-10-19 | zyang2 | verus: the follower's install is coupled inside the gate (Part B, stage 5b) |
| `0c650d6de` | 2026-10-19 | zyang2 | verus: a restart's recovered snapshot is coupled (Part B, E7) |
| `3f2193ed4` | 2026-10-19 | zyang2 | raft: the verified gate admits snapshots (F31, F32) |
| `b44ac8184` | 2026-10-19 | zyang2 | verus: a step's persist note is exact (Part A, P2) |
| `35047db76` | 2026-10-20 | zyang2 | raft: keep process docs out of the repository; record the git history |
| `cde263649` | 2026-10-20 | zyang2 | scripts: drop the measurement results and history kept in tool files |
| `0382d99b0` | 2026-10-20 | zyang2 | raft: correct stale comments and docs; lock the Raft cargo builds |
| `ccb8f4de4` | 2026-10-20 | zyang2 | raft: name the WAL-failure signal; drop raft_dsl.sh's dead doc reference |
| `21944f3f1` | 2026-10-20 | zyang2 | raft: refuse a short append in a replay recording; refuse a long image name |
| `190122dbf` | 2026-10-20 | zyang2 | raft: delete two dead recovery helpers; check the member list is sorted |
