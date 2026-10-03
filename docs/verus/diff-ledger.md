# Diff ledger

One row per changed hunk in `src/deptran/raft/{src,rt/src,core}/`,
`server.cc`, `raft_worker.cc`, `raft_main_helper.cc` (and, for Phase 0's
harness, `raft_bench.cc`), as [modification-plan.md](modification-plan.md)
§A.3 requires. Kinds are §A.3's M0-M12 (labelled "move, don't rewrite"
transformations) and §A.2's F1-F11 (numbered behaviour changes). Every
changed line carries `// [M<n>]` or `// [fix, F<n>]` on or just above it.
Line numbers are after the change. Paths are relative to
`src/deptran/raft/`.

## Phase 0 (all M0: env- or flag-gated instrumentation, inert when unset; no decision reads it)

| Commit | File:line (after) | Kind | What | Evidence |
|---|---|---|---|---|
| `fe9763f80` | `server.cc:6-8` | M0 | `<signal.h>`, `<time.h>`, `<unistd.h>` for the trace kit | builds, three lanes |
| | `server.cc:1620-1722` | M0 | trace kit kernels `raft_trace_at`, `raft_trace_through`, `raft_trace_now_us`, `raft_trace_enabled` behind `MAKO_RAFT_TRACE_FILE`: one cached flag when unset, no clock read | traced run reproduces the 12-stage breakdown (Phase 0 report); A/A G1/G2 on `verus-p0` |
| | `src/server_cc.rs:481`, `:666`, `:1233`, `:1243` | M0 | trace stages 8 (commit advanced), 3/4 (append sent) | as above |
| | `src/server_h.rs:1443`, `:3345`, `:3894` | M0 | trace stages 10 (apply pop), 9 (apply enqueue) | as above |
| | `rt/src/trace.rs` (new), `rt/src/lib.rs:7` | M0 | the Rust lane's hook slots, null unless installed; raft-rt's standalone cargo tests link no C++ | `cargo test: raft-rt` passes |
| | `rt/src/service.rs:120`, `:134-137` | M0 | trace stages 5/6 (follower accepted), through the hooks | traced run |
| | `rt/src/transport.rs:373-377` | M0 | trace stage 7 (leader got a success reply), through the hooks | traced run |
| | `raft_lane_rust.cc:25-33`, `transport_exports.h:31-34` | M0 | `Serve` installs the hooks only when tracing is on | traced run |
| | `raft_worker.cc:50-53`, `:797`, `:850`, `:864-867`, `:1011`, `:1243`; `raft_worker.h:98` | M0 | trace stages 0-2 (enqueue, submit, Start returned), 11 (Next) | traced run |
| | `server.cc:503-513` | M0 | `raft_lab_commit_log_enabled` (`MAKO_RAFT_LAB_COMMIT_LOG=1`), lab builds only (`RAFT_TEST_CORO`) | lab runs, three lanes |
| | `src/lab.rs:25`, `:49-50`, `:180`, `:189`, `:196`, `:200-268` | M0 | `LABLEADER` per waitOneLeader result; `LABCOMMIT` dump of the committed table per case (row-by-row copy, see bugs-found B10) | lab runs; comparator calibration |
| | `src/lab_cases.rs:31`, `:36` | M0 | dump at every case end (`passed`/`failed`) | as above |
| | `raft_bench.cc:126-172`, `:213-215`, `:262-263`, `:289`, `:316`, `:345`, `:370-374` | M0 | `--failover-out`: realtime stamps, record writer, option plumbing | G7 functional run; G7 A/A |
| | `raft_bench.cc:1423-1425`, `:1458-1464` | M0 | leadership callback stamps `new_leader_us`; apply callback stamps `first_commit_us` for a tagged seq-0 probe | as above |
| | `raft_bench.cc:1639-1690` | M0 | follower loop: arm, propose tagged probes once leader, write record, end markers | as above |
| | repo `examples/raft_bench.sh` (kill mode) | M0 | `leader_loss_us` stamp before SIGKILL, `--failover-out` plumbing, the run's JSON record | as above |
