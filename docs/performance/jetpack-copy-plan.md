# Copying jetpack's performance test onto Mako's Rust Raft

Status: plan, 2026-09-29. Sources: jetpack `c03e318e` (github.com/stonysystems/jetpack, branch `jetpack`,
at `~/jetpack`); Mako worktree `/home/users/zyang2/mako-srpc-adopt` (branch `srpc-subtree-forward`,
head `26a9b008d`). Companion: [jetpack-methodology-vs-mako-raft.md](jetpack-methodology-vs-mako-raft.md).

## Short answer

Almost all of jetpack's perf test can be copied. What gets copied, and how:

- **Scripts, configs and output formats (about 45% of the work): verbatim or constant-only edits.**
  - Scripts: `10-run_all.sh`, `derive_fixed_conc.py`, the summary and audit scripts, `merge_latency_csv.py` and `parse_cpustat.py` copy with zero edits.
  - Figure scripts: the camera-ready figure scripts change only their host-count, prefix and protocol-list constants.
  - Configs: 134 yml files are already byte-identical in Mako, and the rest copy verbatim.
  - Output: the `.res` text and 11-column CSV layout copy with `%` format strings converted to `{}` format.
- **The client and server request path (about 50%): restored from Mako's own git history, not jetpack's tree.**
  - Mako carried the same Janus client/transaction stack (ClientWorker, CoordinatorNone/Classic, SchedulerNone/Classic, memdb, rw bench, Classic Dispatch RPC), already moved onto Mako's in-tree RPC and rusty types, until it was retired on 2026-08-23 (`4fc3ba6a9`, `a3569a10a`, `e9c61ecab`, `2e85325b1`, `83f7d74fb`).
  - *Checked after the workflow:* those files predate the srpc subtree (adopted 2026-09-24). They include `rrr/...` headers (for example `a3569a10a^:src/deptran/classic/scheduler.cc:5` includes `rrr/misc/serializable.hpp`), so restoring them also means following phase 8's rrr → srpc API changes. That work belongs in the adapter estimates of steps 6-11.
  - We restore that stack, then paste in jetpack's newer deltas:
    - the open-loop desync delay and random sleep;
    - the `cli2cli_` mid-third windowing;
    - `RW_VALUE_SIZE`;
    - the 12-slot stats and 11-column CSV.
  - The replication side is rebound to today's `RaftSpecific`.
- **Reimplemented (about 5%):** an ~40-line ssh/scp shim, the run root, a Python venv, and the duration-driven `deptran_server` lifetime.

Two decisions shape everything:

1. **Restore Mako history, and do not build jetpack's tree next to Mako.** A "two-world" build that compiles jetpack's own C++ against a C-ABI bridge is not buildable on this host:
   - no boost headers anywhere: `rrr/reactor/coroutine.h:9` needs `boost/coroutine2`, and `__dep__.h:49-52` includes boost;
   - `bin/rpcgen` needs python2, which is not installed;
   - gperftools is missing;
   - `wscript:188-212` links mongocxx, etcd, zookeeper and cpprest unconditionally.

   The wire is not compatible either: srpc replies carry an extra v64 `server_instance_id` (`src/srpc/rpc/server.rs:1263-1272`) that rrr does not write (`~/jetpack/src/rrr/rpc/server.cpp:117-118`). So a stock jetpack client cannot talk to Mako replicas.
2. **The client reaches the leader through a restored Classic `Dispatch` RPC (Route B), not an in-process `add_log_to_nc` shim (Route A).**
   - jetpack's layout puts clients in every replica process (`config/30c1s5r5p-zoo.yml` spreads c01-c30 across all 5 processes).
   - `10-run_all.sh` fails and retries a point unless every host's `.res` has `Mid throughput is` and a CSV.
   - `derive_fixed_conc.py:117-119` needs all 5 servers' files.
   - Under Route A the follower processes get zero throughput, because `add_log_to_nc` returns false off-leader (`src/deptran/raft_main_helper.cc:1164-1170`).
   - Route A survives only as a single-process smoke path (`run_native_local.sh` layout).

## Where copied code lives (provenance and licensing)

**Licence.** jetpack is MIT, "Copyright (c) 2019 Shuai Mu" (`~/jetpack/LICENSE`).
- Every file copied from jetpack keeps its text unchanged and sits under a directory holding a verbatim `LICENSE.jetpack`.
- Every C++ hunk pasted from jetpack into a Mako file carries a `// jetpack c03e318e <path>:<line>` marker comment, which satisfies the "above copyright notice ... included in all copies or substantial portions" clause.
- Code restored from Mako history is Mako's own code, already under Mako's terms; that lineage is Janus, by the same author, also MIT. It needs no new notice, only a provenance marker.

**Layout.** The layout keeps the three origins apart and greppable:

| Origin | Location | Marker |
|---|---|---|
| jetpack verbatim scripts | `bench/jetpack/scripts/` (+ `LICENSE.jetpack`, `SOURCE` file containing `c03e318e`) | unchanged file; edits only via separate overlay files (e.g. `experiment_defs.sh` sources `experiment_defs.orig.sh`) |
| jetpack yml not already in Mako | `bench/jetpack/config/` | unchanged file |
| Mako-history restores | original historical paths (`src/memdb/`, `src/deptran/classic/`, `src/deptran/none/`, `src/bench/rw/`, ...) | first line `// restored from <commit>^:<path>` and every later edit reviewable against `git show <commit>^:<path>` |
| jetpack deltas into restored C++ | inside the restored files | `// jetpack c03e318e <path>:<line>` |
| new glue | `bench/jetpack/shim/`, `src/deptran/jetpack_perf/` | normal Mako code |

Everything C++ builds only under a new CMake option `MAKO_JETPACK_PERF=ON`, so the production tree is unchanged when it is off.

**Relation to CLAUDE.md's Rust-first rule.** This is a test-harness restore of pre-existing C++. Each restored TU goes on the borrow-check exclusion list, with the reason documented in CMakeLists.txt as CLAUDE.md requires. Do not use it as a reason to widen C++ elsewhere.

## Component table

Route key:
- **V**: verbatim, or already live in Mako.
- **CA**: copy with adapter.
- **H**: restore from Mako history, then overlay the jetpack deltas.
- **R**: reimplement.
- **X**: exclude.

Rows marked ⟲ were overturned by the challenge pass; rows marked ✎ carry a correction from it.

### Scripts, analysis, environment

| Component | jetpack source | Route | Adapter | Effort | Evidence |
|---|---|---|---|---|---|
| ssh/scp PATH shim + run root | `scripts/10-run_all.sh:150-157,181-190,215-246`; `run_single_exp.sh:47-146` | R | See the shim notes after this table. | 1-2 h | Every call site is `ssh user@ip "<shell string>"` with the redirection inside the quotes (`10-run_all.sh:224`, `run_single_exp.sh:95`). |
| `experiment_defs.sh` | `scripts/experiment_defs.sh:1-535` | CA | Copy it as `experiment_defs.orig.sh`. The new `experiment_defs.sh` is a 6-line overlay: `ZOO_ORIGIN_PROTOCOLS=(none_raft)`, `ZOO_JETPACK_PROTOCOLS=(rule_raft)`, `ZOO_CONCS_ARRAYS=(RAFT_CONCS)`, `ALL_FASTPATH_MODES=()`. It must be an overlay because `10-run_all.sh:92-100` copies the arrays right after sourcing. | h | `:50-51` lists copilot, mongodb, etcd and zk; `:66` adds rule_* at -m 100/101. |
| `10-run_all.sh` exp 0/1/2 | `scripts/10-run_all.sh:1-508` | CA (0 edits to the file) | See the `10-run_all.sh` notes after this table. | h | `:79-80` emits `zoo${i}` for i=0..4, but `config/30c1s5r5p-zoo.yml:14-18` names the processes zoo1..zoo5 (upstream bug, verified). |
| `run_native_local.sh` | `ae/local/scripts/run_native_local.sh:1-102` | CA | Delete the image check (`:49-55`). Replace the docker block (`:72-93`) with a local `timeout ... ./build/deptran_server ... -P localhost -N $LABEL`, then copy the CSV out. Use `CLIENT_CFG=client_open_raft.yml`. Build `LABEL` with `build_result_prefix ...-server0`. | h | The original needs docker (`:51,:72`) and leaves the CSV as `not_set.csv` inside the container (`config.cc:55`). |
| `ae/local/run.sh` + `ae/reproduce_local.sh` | `ae/local/run.sh:1-614` | CA (partial) | Keep phases exp0_native, exp1, exp2, quick and summary. Set `NATIVE_GROUP_A=(raft)` and empty the other groups. Drop the rule_* entries (`:309-310,:389-390,:447-448,:551-552`), docker preflight (`:219-236`), phase_build's docker branch, recovery and tla. The quick zipf list `:377` names `rw_zipf_1.0`, which has no config; use `rw_zipf_1`. | h | Read in full. `CONCS_FULL[raft]` is identical to `RAFT_CONCS`. |
| `run_single_exp.sh` | `scripts/run_single_exp.sh:1-233` | CA | `:84`: drop the `build/docker_libs/ld-linux-x86-64.so.2` prefix. Otherwise the process comm is ld-linux and `pkill deptran_server` (`:48,:117`) misses. `:95`: `export SERVER_CORE_ID=$((SERVER_CORE_ID+i))`, and pass the per-host core to parse_cpu_usage at `:224`. Symlink `scripts/setup.json`. Use a zoo1..zoo5 site yml on 127.0.0.1. | h | Read in full. |
| `gen_client_config.sh` | `scripts/gen_client_config.sh:65-69` | CA | Change the 5 host IPs to 127.0.0.1. | min | |
| `run_adaptive_sweep.sh` | `scripts/run_adaptive_sweep.sh:1-346` | CA | Change the heredoc hosts (`:113-118`) to 127.0.0.1. The script assumes zoo2 is the leader (`:84-90,:186-188`), so the Mako leader must be s201 (step 13). | min | Parses `server average` (`:137`) and tolerates it missing (`:151`). |
| `run_tier1_batch.sh` / `run_max_throughput_regression.sh` / `run_bisection_sweep.sh` | same | CA | tier1: `PROTOS=("raft none_raft.yml 0")`; run once per lane with distinct labels. regression: Mako floors. bisection: verbatim. | min | |
| `08-build_and_test_run_local.sh` | `:1-111` | CA | `:43` none_copilot → none_raft. Do not pass `full`/`build` (waf). Either print `Deleted one.` at teardown or drop the grep at `:102`. | min | The string comes from jetpack `server_worker.cc:327`. |
| `09-build_and_test_run_wan.sh` | `:1-570` | CA | Constant block `:9-22`. For single-host failover, change `:441` to `pkill -9 -f 'deptran_server.* -P <replica>( \|$)'`. | h | Recovery evidence targets jetpack recovery; only the kill skeleton transfers. |
| `ci_regression.sh --lane wan` | `:43-56` | CA | `MODES=(none_raft)`. ✎ It hardcodes `config/client_open.yml` (`:138,:261`), so jetpack's copy must be used (see the yml row). | min | tc lane self-reports BLOCKED without root (`:171-180`). |
| `sweep_benchmark.sh` | `:238-247`, `docker/etcd/run-etcd-test.sh:825-957` | CA (low priority) | Replace `docker run` with a local copy of `run_benchmark` minus the etcd steps, and set `WAN_DELAY_MS`. | h | Redundant with the 10-run_all path. |
| ⟲ `run_akkio_exp.sh` | `scripts/run_akkio_exp.sh:125-215` | CA (was X) | Keep `run_deptran_variant` verbatim. IPs → 127.0.0.1; `ZOO_DIR`; drop the ld-linux prefix `:163`; per-replica `SERVER_CORE_ID`; `gen_akkio_client_config.sh:75-79` hosts → 127.0.0.1. main becomes `run_deptran_variant raft-rust none_raft.yml 0; MAKO_RAFT_LANE=cpp run_deptran_variant raft-cpp none_raft.yml 0`. | h | etcd appears only in start/stop and the log scp. Produces the exact `log/<variant>/` layout `merge_latency_csv.py:4` reads. |
| `derive_fixed_conc.py` | `scripts/derive_fixed_conc.py:1-422` | V | Run from `<root>` so `fixed_conc.json` lands where `10-run_all.sh:103` reads it. | min | Defaults: site `30c1s5r5p-zoo`, servers zoo0..zoo4 (`:77,:86,:131,:139`). |
| summary/audit scripts (`generate_summary`, `sanity_check`, `csv_audit`, `generate_tables`, `generate_experiment_report`, `generate_cpu_figure`, `timeout_audit`, `res_file_utils`) | `scripts/*.py` | V | None. `generate_cpu_figure` needs matplotlib. | min | All hardcode `30c1s5r5p-zoo` and zoo0..4, which is what the 10-run_all path emits. |
| `build_per_protocol_tables.py` | `:1-237` | V | Optionally add lane labels to `PROTOCOLS` (`:42-52`). | min | |
| `merge_latency_csv.py` | `:1-394` | V | Feed it the run_akkio / run_single_exp `log/<variant>/` layout. | min | Stdlib only. |
| `parse_cpustat.py` | `:1-73` | V | Call it once per replica with that replica's core. | min | |
| `gen_tput_p90_figures.py` | `scripts/camera-ready/:44,:90,:150,:253,:269-316,:418-424` | CA | Constants: `NHOSTS=5`, prefix `30c1s5r5p-zoo`, suffix `zoo{i}`, `BANNED_CONC={}`, `GRID_PROTOCOLS=[("Raft","raft")]`, trimmed `COMPARE_LINES`. Symlink `<result-dir>/log`. Keep `-d 30` (`:127` window 25-35 s). | h | The median at `:239` is safe for n=1 or 5. |
| `gen_latency_cdf.py` | `:31,:39-53,:86,:119` | CA | Same constants, plus `FIXED_CONC['raft']` from `fixed_conc.json`. | h | Reads CSV column 5 (`:99`). |
| `gen_workload_axis_figures.py` | `:65,:120,:194` | CA | Same constants, plus the conc row `('Raft','raft',<fixed>)`. | h | Workload names match 10-run_all exp1/2. |
| `98-kill.sh` | | V | None. | min | |
| `scripts/test_*.py`/`.sh` | | V | Run with pytest. Point `test_experiment_defs.*` at `experiment_defs.orig.sh`. | min | They pin the copied parsers' behaviour. |
| Python env | `ae/local/requirements.txt` | R | `python3 -m venv ~/venvs/jetpack-ana && pip install numpy pandas matplotlib pytest`. | min | The requirements file pins Python-2-era packages; `import matplotlib` fails on this host. |
| docker/backend/provisioning scripts (`reproduce_evaluation`, `run_full_sweep`, `rerun_failed_points`, `00-07`, `aws_*`, `start_*`, etcd/zk plots, `ae/tla`) | | X | | | They need docker, etcd/zk/mongo or multi-host provisioning; the shim replaces 00-07. `tsv_to_md.sh` is copyable only if sweep_benchmark is adopted. |

**Shim notes (ssh/scp row).** Put `shim/ssh` and `shim/scp` on PATH.
- `shim/ssh` strips the `-o/-i/-p/-q/-T/-n` options and `user@host`, then runs `exec bash -c "$*"`. The `exec` keeps the shim's own command line from matching `pkill -f deptran_server` (`:186`).
- `shim/scp` strips any `[user@]host:` prefix, then copies unquoted so globs expand, and ignores errors.
- The run root must be a git repo with at least one commit (`10-run_all.sh:156-161`).
- `setup.json` sets `environment: zoo`. The aws branch hardcodes `/home/ubuntu/code/JetPack` at `:65`.

**`10-run_all.sh` notes.** Run it as `PATH=<root>/shim:$PATH scripts/10-run_all.sh --exp 0`. It needs:
- a local `config/30c1s5r5p-zoo.yml` with processes zoo0..zoo4 and `host: {zooN: 127.0.0.1}`;
- a binary whose basename is `deptran_server`.

### Config and yml

| Component | jetpack source | Route | Adapter | Effort | Evidence |
|---|---|---|---|---|---|
| yml fragments | `config/rw*.yml`, `YCSB_*.yml`, `concurrent_*.yml`, `client_open_raft.yml`, `none_raft*.yml` | V | Copy the missing ones into `bench/jetpack/config/`: `client_open_raft`, `none_raft`, `none_raft_lease`, `rw_akkio*`, `rw_readonly_1000000`, and 24 `concurrent_*`. ✎ **Use jetpack's `client_open.yml` (max_undone 200), not Mako's (5000).** `run_single_exp.sh:84`, the sweeps and `ci_regression.sh` hardcode it, and the difference is 25x offered load. Keep it in `bench/jetpack/config/` so Mako's `config/client_open.yml` is untouched. | min | `cmp`: 134 identical; `client_open.yml`, `failover.yml` and `30c1s5r10p.yml` differ. |
| site topology | `config/60c1s5r5p.yml`, `30c1s5r5p-zoo.yml` | CA | `60c1s5r5p.yml` is verbatim (127.0.0.1-5). The zoo files keep `site:`/`process:` and rewrite `host:` to 127.0.0.1. The 10-run_all variant uses zoo0..zoo4 and the run_single_exp variant zoo1..zoo5. | min | Zoo hosts refuse SSH. |
| `cc: none` acceptance | `config.cc:559-567` | CA | Mako `config.cc:483-486`: `verify(0)` only if `cc` is present and not `none`. Restore `none_raft.yml` from `4fc3ba6a9^`; it is byte-identical to jetpack's. | min | Diff is empty. |
| `-m` flag | `config.cc:80,149-151` | CA | Add `m:` to getopt at Mako `config.cc:89` and parse-and-ignore it. Without it, CreateConfig returns -2 (`:192-207`). | min | `experiment_defs.sh:269,271` always passes `-m`. |
| `bench:` / `bench_update_weight:` | `config.cc:333-338,496-521,613-666` | H | Restore LoadBenchYML and UpdateWeights from `6976bf3c5^:src/deptran/config.cc:305-311,533-588`, which are already `{}`-formatted. Population goes to the restored sharding (next row). | h | `git log -S LoadBenchYML`. |
| ⟲ `schema:` / `sharding:` | `config.cc:339-344,668-760` | H (was X) | Restore LoadSchemaYML (`6976bf3c5^:config.cc:590`), LoadShardingYML (`:661`), the dispatch at `:313,:316` and `sharding_`. Without them the restored memdb stays empty and the RW Query fails. | h-1 d | Required by the TPC-C sharding row. |
| client/site/process/host parsing | `config.cc:314-480,762-800` | V | None. `raft_leader_locale` (jetpack `:358-360`) is missing but no perf yml sets it. | none | Diffed. |
| ✎ `Config::retry_wait()` | `coordinator.cc:40` | CA | Add a `retry_wait_` field (jetpack default) and accessor. It is missing in Mako (grep of config.h/.cc). | min | Correction. |
| ✎ `rw_benchmark_para_` | `config.h` (jetpack) | CA | Missing from Mako `config.h`. Restore it with the bench block (`n_table_` from `population.history`). | min | Correction. |
| `Config::IsSiteLocal` | `config.cc:912-921` | V | Paste in as is. All the fields it reads exist (Mako `config.h:67,115,153,154`). | min | |

### Output, statistics, CPU

| Component | jetpack source | Route | Adapter | Effort | Evidence |
|---|---|---|---|---|---|
| `Distribution`, `Frequency` | `src/deptran/scheduler.h:38-162` | V (live) | Include `deptran/distribution.h` (`:22-196`). | none | Same `pct()` (sort, floor(n·p), clamp) and byte-identical `statistics()` (`%7s%9zu`/`%7s%9.2f`). |
| Log line format | `src/rrr/base/logging.cpp:46-76` | V (live) | None. srpc `logging.rs:51-72` emits the same tag, timestamp and ` \| `. | none | Mako passes file=null, line=0 (`srpc_log.h:42-45`); no regex depends on it. |
| global stats + `client_shutdown` merge | `s_main.cc:205-218,421-462`; `client_worker.h:91-107` | CA | Copy the globals and body. Drop JETPACK_PROF/LATENCY_DEBUG. `%zu` → `{}` at `:452`. Take the 12-slot field list from jetpack, not from history (history has only `request_latency_`). | h | |
| end-of-run `.res` lines | `s_main.cc:997-1009,1110-1133,1172` | CA | Convert formats only (`%s`/`%d`/`%zu` → `{}`, `%.2f` → `{:.2f}`). Keep `throughtput` (sic) and `Mid throughput is` byte-exact. **Mandatory:** `srpc_log.h:42` is `std::format_string`, so an unconverted `%.2f` prints literally and breaks every regex. | h | Regex users: `gen_tput_p90_figures.py`, `merge_latency_csv.py`, `10-run_all.sh:268-290`, `run_adaptive_sweep.sh:135-142`. |
| 11-column CSV dump | `s_main.cc:1011-1060` | CA | Copy the ofstream body; convert the 3 Log_info lines. Keep the pre-cleanup dump and skip the redundant second one (`:1174-1216`). Do **not** use history's 3-column version (`f15276f77^:s_main.cc:653-679`). | h | `gen_latency_cdf.py:99` reads col 5. |
| CPU kernels + `SERVER_CORE_ID` | `scheduler.cc:31-120`; `s_main.cc:919-926` | CA ⟲ | Copy CpuStatSnapshot, ReadCpuStatsInternal, ComputeCpuUsage and SampleCpuUsage as free functions into `src/deptran/jetpack_perf/cpu_stat.{h,cc}`. Drop `StartCpuMonitorIfNeeded`: it needs rrr coroutines (`:72,:87`), and its own comment says SampleCpuUsage supersedes it. (The raft-iface row that called it verbatim is overturned.) | h | No such symbols exist in Mako. |
| client-side `getUsage` sampler + report | `s_main.cc:755-866,955-990` | CA | Functions verbatim. The report block comes from jetpack, not from history (history's is `#ifdef AWS` and lacks `server average`). Formats go to `{:.4f}`/`{:.3f}`. | h | |
| Cpu-usage-leaders / Queue-depth fallback | `s_main.cc:1134-1171` | CA | Use the free CPU functions. Queue-depth always prints `ave -1.0000 count 0`, because `GetQueueDepthForRule` belongs to the retired Rule (`d0d0fa617`). | h | |
| JETPACK_PROF / LATENCY_DEBUG / DB_CHECKSUM / CPU_PROFILE | `s_main.cc:414-520,1072-1105,1218-1222` | X | | | Off by default; they read retired Rule members. |
| tdigest dumps | `10-run_all.sh:246` | X | | | jetpack's C++ never writes them (grep); the scp calls are `\|\| true`. |

### Workload and data plane (server side restored from history)

| Component | jetpack source | Route | Adapter | Effort | Evidence |
|---|---|---|---|---|---|
| `ZipfDist` | `src/bench/tpca/zipf.h:1-123` | H | Restore `83f7d74fb^:src/bench/tpca/zipf.h`. It differs from jetpack only on lines 42 and 45, in format strings. | min | diff |
| ⟲ RwWorkload generator (`GetTxRequest`, `GetId`, `RW_VALUE_SIZE`) | `src/bench/rw/workload.cc:54-154`, `workload.h:18-44` | H + jetpack delta | Restore `e9c61ecab^:src/bench/rw/workload.{h,cc}` and `src/deptran/workload.{h,cc}`, keeping `RegisterPrecedures`. Paste jetpack's `RW_VALUE_SIZE` (`:62-76,:99-113`, `value_size_`) with one Log_info in `{}` form. Output stays `input_={{0,Value(id)},{1,Value(v)},{2,Value(pad)}}`. The earlier POD swap is dropped. | h | The diff vs jetpack is only that block plus formats. |
| RW procedures + RWChopper + RWBenchmarkSharding | `src/bench/rw/procedure.cc:1-91`, `sharding.h:1-10` | H | Restore `e9c61ecab^:src/bench/rw/{procedure.*,sharding.h}`. procedure.cc differs from jetpack by 1 line; sharding.h is identical. | h | |
| TPC-C sharding/constants + `deptran/sharding.*` | `tpcc_real_dist/sharding.{h,cc}`, `deptran/sharding.cc:316-376` | H | Restore from `83f7d74fb^` and `e9c61ecab^`: the headers and sharding only, no TPC-C procedures. | h | RWBenchmarkSharding inherits TpccdSharding. |
| memdb (28 files) | `src/memdb/*` | H ✎ | Restore `2e85325b1^:src/memdb/` plus `MEMDB_SRC` (`CMakeLists.txt:923`). ✎ The work is more than include renames: `rrr_log.h`/`rrr::Log_fatal` appear in row.cc:6,110, value.cc:6, table.cc:4 and schema.cc:4; `using rrr::verify` in utils.h; and every TU needs the `__dep__.h`/`std_compat.hpp`-first ordering (`__dep__.h:3-9`). Budget **days**, not hours. Do not take jetpack's copy: it needs boost/crc and `rrr::ALock`. | 1-3 d | 12 files are identical to jetpack; row.h differs by 313 lines (Arc). |
| `TxRequest`/`TxReply`/`TxWorkspace`/`TxData`/`TxPieceData`/`VecPieceData` | `procedure.h:1-512`, `procedure.cc` | H ✎ | Restore `e9c61ecab^:src/deptran/procedure.{h,cc}`. ✎ `procedure.cc:5` includes `rrr/misc/serializable.hpp`, which is gone; port it to `import srpc.serializable`. **Wire kind 4:** `legacy_raft_log_payload.cc:223` registers `LegacyVecPieceData` at kind 4 (`mako_commands.h:71`). Under `MAKO_JETPACK_PERF` compile out that registration and register `VecPieceData`. Rust never decodes `cmd` (`rt/src/rpc.rs:163-170`), so it replicates on every lane. | 1-2 d | |
| command/marshal-value/multi_value/RW_command | `command.h`, `RW_command.cc:168` | H ✎ | Restore the `e9c61ecab^` files. `command.h:5` includes `rrr/rrr.hpp` (gone); `namespace rrr` → `srpc` in marshal-value.h. `GetCurrentMsTime` becomes the free `jpclient::GetCurrentMsTime` (4 lines, from jetpack `RW_command.cc:168-172`). | h-1 d | |
| `Tx`/`TxClassic` | `tx.h`, `tx.cc`, `classic/tx.h` | H | Restore `e9c61ecab^:tx.*` and `a3569a10a^:classic/tx.h`. `commit_result` is `Arc<BoxEvent<int>>`, which is safe to set cross-thread (`reactor.rs:288-300`). `ev_execute_ready_` is a Cell and stays on the Tx thread. | h | |
| `TxnRegistry`/`RegP`/`PROC`/`benchmark_registry` | `txn_reg.h:1-91` | H | Restore from `e9c61ecab^`. Keep Mako's ProcHandler, which has no `Executor*`. | h | 7 diff lines. |
| TpcCommit/Prepare/Empty/Batch commands | `classic/tpc_command.*` | V (live) | Use `src/deptran/tpc_command.{h,cc}`; do not copy jetpack's MarshallDeputy format. | none | `server_cc.rs:995-1001` batches them. |

### Server request path

| Component | jetpack source | Route | Adapter | Effort | Evidence |
|---|---|---|---|---|---|
| ✎ `TxScheduler` stateful base | `scheduler.h:330-785`, `scheduler.cc` | CA from H | Restore `4fc3ba6a9^:src/deptran/scheduler.{h,cc}` as a new `class TxScheduler` in `tx_scheduler.{h,cc}`. `TxLogServer` is now an interface (`scheduler.h:1-62,214`), so the name cannot be reused. Keep the Tx/MTxn maps, `reg_table`, `GetTable`, `CreateRepCoord`, `SetPartitionId`. Exclude the Jetpack recovery plane (`2c1e7007d^:scheduler.cc:471-1244`). Change `rep_sched_` to `RaftSpecific*`. ✎ Its hidden includes `epochs.h`, `kvdb.h`, `RW_command.h` and `classic/tpc_command.h` are deleted today: restore or strip them. ✎ Delete its own `class Distribution` (`:22`) and `class Frequency` (`:125`), which clash with `distribution.h`. | 1-2 d | |
| ⟲ `CreateRepCoord` + learner wiring | `scheduler.cc:306-322`, `server_worker.cc:94-102` | H (was V) | Take `CreateRepCoord` from `cb9f1484f^:scheduler.cc:164-185`; it builds `new CoordinatorRaft` directly. Bind the learner with `rep_sched_->reg_learner_action(LearnerAction)` (`scheduler.h:65,219`). | h | There is no `app_next_` field in Mako. |
| `SchedulerClassic` | `classic/scheduler.{h,cc}:28-398` | H | Restore from `a3569a10a^`. Derive it from TxScheduler, drop `override` from `Next`, have `IsLeader` forward to RaftSpecific, delete IsFPGALeader/RequestVote, and keep `Submit(cmd.clone())` so the WRONG_LEADER ret_ is visible. cli2tx/tx2tx are already present. | h-1 d | Every diff hunk is types or format. |
| `SchedulerNone` | `none/scheduler.{h,cc}` | H | Restore from `4fc3ba6a9^` (4 diff lines in .h). Drop the read-lease block (`scheduler.cc:25-35`). | h | No `HasReadLease` in Mako. |
| ⟲ `CoordinatorRaft` | `raft/coordinator.{h,cc}:1-299` | CA from H (was V) | Restore `e9c61ecab^:src/deptran/raft/coordinator.*` plus the base fields from `c14c9a1ea^:coordinator.h`. Mapping: `svr_` becomes `RaftSpecific*`; Start becomes `Start(cmd_,&i,&t)==RaftStartResult::APPENDED`; `commitIndex` becomes `CommitIndex()`; the term check becomes `!IsLeader()`; delete the `ready_for_replication_` kick. WRONG_LEADER builds `View(n, GetLeaderHint(), 0)` and calls an injected `learner_`. Drop `paused_`/`jetpack_status_`. **Keep the 1 ms poll** for method parity. | 1 d | jetpack's copy needs boost and its own raft/server.h. |
| ✎ learner binding | `server_worker.cc:94-102`, `raft/server.cc:641` | CA | hybrid/cpp lanes: bind `SchedulerClassic::Next` directly. Rust lane: post each apply to a **newly created** C++ `PollThread` via `OneTimeJob`. ✎ SetupService returns before creating `svr_poll_thread_worker_` under `MAKO_RAFT_LANE_RUST` (`server_worker.cc:68-78`), so this PollThread must be new, and must be the same one that owns ClassicServiceImpl and the Tx IntEvents. Do not route through RaftWorker: it throws on non-LogEntry inners (`raft_worker.cc:1033-1057`). | h | |
| ✎ Classic `Dispatch` RPC | `service.cc:85-305`, `rcc_rpc.rpc:201` | H | Restore the Classic block from `9840dab63^:src/deptran/rcc_rpc.rpc`, keeping Dispatch and IsLeader, and regenerate with `bin/rpcgen`. Restore ClassicServiceImpl from `a3569a10a^` with `srpc::DeferredReply`. Handlers have a const receiver, so `dtxn_sched_` becomes a pointer member. Rust lane: it runs on its own srpc::Server on `port+delta`, because Raft owns the port (`raft_lane.h:28-31`). ✎ **rpc-id gate:** `CMakeLists.txt:1197-1211` runs `rpcgen_rust.py --ids src/deptran/raft/rpc_ids.txt` and fails if a Classic id collides with a frozen Raft id (`rpcgen_rust.py:174-188`); check it and pin the Classic ids. | 1 d | |
| Frame for MODE_NONE | `frame.cc:130-460` | CA from H | Add a separate `TxFrame` from `4fc3ba6a9^:frame.cc` (CreateScheduler, CreateTxnCommand `:134`, CreateSharding `:79-91`, CreateRpcServices). Do not touch the replication `Frame::GetFrame`. | h | Mako `frame.h` has 3 pure virtuals. |
| ServerWorker transaction half | `server_worker.cc:40-200` | CA from H | Paste the `4fc3ba6a9^:server_worker.cc:73-182` lines into today's `server_worker.cc` after `rep_sched_` is created: the tx_sched_ setup, the schema loop, PopulateTables, RegisterPrecedures, the learner, and Classic in SetupService. Drop the recovery cross-links. | 1 d | |
| ✎ `deptran_server` process | `s_main.cc:311-464,940-1050` | CA/R | ✎ This is **not** a string rename. Outside `RAFT_TEST_CORO`, the rust-lane `SetupCommo` returns after `WaitForStartup` (`server_worker.cc:121-139`); the thread exits and main shuts down at once (`s_main.cc:96-148`). On cpp/hybrid only site 0 runs the loop (`:170-173`). Fix: rewrite main on jetpack's shape. Launch servers, launch clients, run getUsage for `-d`, join the clients, print stats, then shut down. Build it outside `if(RAFT_TEST)` (`CMakeLists.txt:1742-1746`) so it links production Raft (5 ms heartbeat). Keep Mako's RunServers thread-per-site and setup barrier. | 1-2 d | |
| jetpack Raft core / RaftCommo / RaftService / RaftFrame / two-world facade + C-ABI bridge | `raft/server.*`, `commo.cc`, `service.cc`, `frame.cc` | X ⟲ | The facade and bridge rows are overturned: the route is unbuildable (see decision 1). | | This is the system under test; copying it would benchmark jetpack's Raft. |
| read-lease path | `none/scheduler.cc:18-35` | X | | | Mako has no lease; it is off in none_raft.yml. |
| recovery plane / failover hooks | `scheduler.cc`, `raft/coordinator.cc:30-33,85-94` | X | | | Retired in `e048406a6`; restorable later from `2c1e7007d^`. |
| ✎ Replicated command encoding (bytes across the bridge) | `classic/scheduler.cc:239-250` | X | It was only needed for the two-world build. On this route `TpcCommitCommand{cmd_=VecPieceData}` replicates natively. | | |

### Client side

| Component | jetpack source | Route | Adapter | Effort | Evidence |
|---|---|---|---|---|---|
| ✎ Client `Coordinator` base | `coordinator.h:29-212`, `coordinator.cc:22-82` | CA from H | Restore `c14c9a1ea^:src/deptran/coordinator.h` and `e9c61ecab^:coordinator.cc` as `janus::jpclient::Coordinator` in `src/deptran/jetpack_perf/coordinator.h`. The namespace avoids clashing with the live `janus::Coordinator` used by Paxos. Drop the quorum/ccsi/TXN_STAT members. ✎ The constructor needs `retry_wait()`, which is added in the config rows. | 2 h | |
| ⟲ `CoordinatorClassic` DoTxAsync/Reset/Restart/End/Report | `classic/coordinator.cc:20-99,173-215,703-784` | H (was CA) | Restore `a3569a10a^:src/deptran/classic/coordinator.{h,cc}` as is. `CreateTxnCommand` (`:63`) comes from TxFrame with RWChopper, so no `JetpackRaftCmd` shim is needed. Drop only the verify(0) forwarding branch. | h | |
| ⟲ `DispatchAsync`/`DispatchAck` + `BroadcastDispatch` + leader views | `classic/coordinator.cc:217-251,336-436`; `communicator.cc:90-280,473-572,1777-1830` | H (Route B; was CA Route A) | Restore DispatchAsync (`a3569a10a^:classic/coordinator.cc:216-251`) and DispatchAck. Restore BroadcastDispatch (`c14c9a1ea^:communicator.cc:462-537`), LeaderProxyForPartition (`:168-253`), Connect/WaitConnectClientLeaders (`:63-122`) and UpdatePartitionView/GetPartitionView/GetLeaderForPartition (`:1607-1660`). Bind the regenerated ClassicProxy to today's RpcPeer/srpc::Client (`communicator.h:31`). Replace the fixed `WAN_WAIT` (`:516`) with jetpack's runtime `_wan_wait_to_site`. | 2-3 d | GetReadyPiecesData is at `e9c61ecab^:procedure.h:407`. |
| ⟲ `CoordinatorNone::GotoNextPhase` | `none/coordinator.cc:39-112`, `.h:1-13` | V | Copy **jetpack's** file (not history's `6505e23ce` version, which lacks every `cli2cli_` and window line). Only the Log_info formats change. `cmd_is_write_ = !((TxData*)cmd_)->IsReadOnly()` compiles as written once TxData is restored. | 30 min | Window arithmetic `:46-48`, appends `:75-81`, commit_time `:94-95`. |
| `ClientWorker` header + ctor | `client_worker.h:1-143`; `.cc:828-894` | CA | Use the jetpack field list: 12 `cli2cli_` slots, `dispatch_time_distribution_`, `commit_time_`, `frequency_`, `cpu_usage_leaders_`, `queue_depth_`. Drop the mongo/rule/OneArmedBandit fields. Use history `6505e23ce:client_worker.h` for the rusty types (`poll_thread_worker_`, std::mutex/condvar). Keep `creation_time_(GetCurrentMsTime())`. Build `commo_` as in history, plus jetpack's SetLeaderCallback without NAIVE_EPAXOS. Build tx_generator_ via the restored benchmark_registry. | 2-3 h | |
| `ClientWorker::Work` | `client_worker.cc:198-534` | CA | jetpack text line by line, with API spellings from `6505e23ce:client_worker.cc:198-406`. The adapter is in the brief: `create_sp_int_event`, `wait_timeout`, `Fiber::create_run_impl`, `OneTimeJob::new_`, `wait_until_gte` (timeout 0 at history `:362`), `Time::now(false)`, `{}` logs. Keep the desync delay (`:333-339`), the throttle (`:367-402`), the random 0.5-1.5 s sleep (`:449-452`), the `_inuse_`-then-push order (`:471-475`), the finish job, the watchdog and the Throughput line. Leader-wait uses the restored `WaitConnectClientLeaders`. Drop RegisterPrecedures client-side, ccsi, failover (`:220-263`) and `:265-311`. Do not copy history's paused/qe block or its fixed 1 s sleep. | ½ d | srpc names verified at `reactor.rs:506-535,935,2044-2052`, `misc.rs:84`. |
| `DispatchRequest` | `client_worker.cc:709-770` | CA | Use history's `6505e23ce:client_worker.cc:491-529` event spellings. WRONG_LEADER goes to `commo_->UpdatePartitionView(par, view)`. | 1 h | |
| `FindOrCreateCoordinator`/`CreateCoordinator` | `client_worker.cc:125-196`; `frame.cc:227-234` | CA | `new jpclient::CoordinatorNone(coo_id, benchmark, nullptr, id)` inlined; keep `clientworker_creation_time_` and `client_worker_`. | 30 min | History went through MultiPaxosFrame, which is wrong for Raft. |
| `client_launch_workers` | `s_main.cc:242-309` | H + jetpack delta | Restore `f15276f77^:src/deptran/s_main.cc:70-200`, then add jetpack's core-skip rule (`:296-301`) and `server_core_id`. | 2-3 h | |
| `client_shutdown` + CSV | `s_main.cc:198-218,421-462,997-1050` | CA | jetpack's version; see the Output table. | 1-2 h | |
| n_concurrent / max_undone / open-closed | `config.cc:180`, `client_open_raft.yml` | V (live) | None. Mako's `n_concurrent_` is uint16_t (`config.h:75`), which is fine for jetpack's sweeps. | none | |
| `wan_delay_us` + `_wan_wait` + `_wan_wait_to_site` | `communicator.h:15-46`; `communicator.cc:19`; `s_main.cc:900-911` | CA | Paste into `jetpack_perf/wan.{h,cc}`. `NeverEvent Wait(d)` becomes `create_sp_never_event()->wait_timeout(d)` or `Fiber::sleep(d)`. It must run in a fiber, never on the apply thread. Add a CMake `-DSIMULATE_WAN`. | 1 h | Mako history had a fixed 50 ms (`54332be30^`); take jetpack's runtime version. |
| ⟲ client↔leader WAN hops | `communicator.cc:560-571`; `classic/coordinator.cc:335-361` | CA (was V) | Put `_wan_wait_to_site` in the restored BroadcastDispatch and at the top of the restored DispatchAck. | h | The jetpack host function is not copied whole. |
| WAN on Raft sends, Rust lane | `raft/commo.cc:38,112,179` | CA | In `src/deptran/raft/rt/src/transport.rs` `send_append_entries_with` (`:395`), `send_empty_append_entries` (`:412`) and `broadcast_vote` (`:555`), when delay > 0: create the pending/sink now, encode the cmd into a `Vec<u8>` now, and send from a `Fiber::create_run(move \|\| { sleep; ...async })`. Do not sleep inline: the phase-1 loop (`server_cc.rs:1232`) would stack the delays to N·D. Read `WAN_DELAY_MS` once via `OnceLock`. | 1 d | Only the placement is copied from jetpack. |
| WAN on Raft sends, cpp/hybrid | same | CA | Same fiber-per-send pattern in `src/deptran/raft/commo.cc` SendAppendEntries2 (`:26-110`), BroadcastVote (`:112`), `*Cb` (`:207,:280`) and SendInstallSnapshot (`:157`). | h | `f90b93499^` had WAN_WAIT at the jetpack sites. |
| `SERVER_CORE_ID` pinning | `s_main.cc:242-360,916-925` | CA | Client loop is verbatim. Server: call `pthread_setaffinity_np(pthread_self())` inside the worker thread **before** RaftWorker creates its poll/apply/submit threads, which inherit the mask. The existing `PinServerThread` (`s_main.cc:23-38`) pins from outside and races with those threads. | h | |
| dead or unreachable client code: old Work/Dispatch in `/* */`, RequestDone/Forward*/retrive_statistic, 2PC/sync/failover methods, file-signal sync, ccsi, CoordinatorRule | `client_worker.cc:25-123,536-626,628-656,772-817,265-366`; `classic/coordinator.cc:102-171,253-322,439-700,798-838`; `rule/` | X | Restoring `a3569a10a^` whole brings the 2PC methods along unused, at no cost. Pass `nullptr`/`rusty::None` for ccsi. | none | verify(0) entry points; `if(false)`; `-b` is never passed (`experiment_defs.sh:245-273`); rule_raft is the JetPack protocol (`settings.md:232-238`). |
| failover job | `client_worker.cc:146-168,220-263,657-707` | X (deferred) | A Mako port exists at `6505e23ce:client_worker.cc:158-604`. | 1-2 d later | Needs `FailoverPauseSocketOut` RPCs, which Mako no longer serves. |

## Implementation plan (one step at a time, each buildable and testable)

Every step ends green, and its commit message says what was copied, from where, and which lines were adapted. Build on the host with `cmake --build <dir> -j32` under long timeouts, per project practice.

**Step 0: environment and run root (R, 2 h).**
- Create the venv (`numpy pandas matplotlib pytest`).
- Create `bench/jetpack/shim/{ssh,scp}` and a run-root builder script, `bench/jetpack/mkroot.sh`. It: `git init` + 1 commit; symlinks `build/deptran_server` (basename kept); fills `config/` and `scripts/`; creates `results/recent_csv/`; writes `setup.json` (zoo, 5×127.0.0.1).
- Verify: `PATH=<root>/shim:$PATH ssh u@127.0.0.1 "echo ok > /tmp/x"` writes locally; `scp u@h:<root>/a* <dst>` expands the glob; `pkill -f deptran_server` does not kill the shim.

**Step 1: analysis scripts verbatim (V, 1 h).**
- Copy `derive_fixed_conc.py`, `res_file_utils.py`, the summary/audit scripts, `merge_latency_csv.py`, `parse_cpustat.py`, `build_per_protocol_tables.py`, `98-kill.sh` and all `scripts/test_*` from `~/jetpack/scripts/` to `bench/jetpack/scripts/`, plus `LICENSE.jetpack` and `SOURCE`.
- Verify: `pytest bench/jetpack/scripts` passes unchanged. This is jetpack's own fixture suite, so it proves the copies behave identically.

**Step 2: drivers and figure scripts (CA, ½ d).**
- Copy `experiment_defs.sh` → `.orig.sh` and add the 6-line overlay.
- Copy `10-run_all.sh` untouched, plus `run_single_exp.sh`, `gen_client_config.sh`, `run_adaptive_sweep.sh`, `run_tier1_batch.sh`, `run_akkio_exp.sh`, `08`, `09` and `ci_regression.sh`, with the edits in the table.
- Copy `scripts/camera-ready/gen_{tput_p90_figures,latency_cdf,workload_axis_figures}.py` with the constant edits, and the de-dockerised `run_native_local.sh`.
- Verify:
  - `bash scripts/test_experiment_defs.sh` against `.orig.sh` passes;
  - `source experiment_defs.sh; generate_*_matrix` yields only none_raft × RAFT_CONCS;
  - `10-run_all.sh --exp 0 --dry-run`, if supported, or with a stub `deptran_server` that prints `Mid throughput is 1.00` / `Dumped to x with 1 lines data` and writes a CSV, runs one point per host through the shim and exits "all succeeded".
  - Then run the figure scripts on those stub outputs to exercise the parsers end to end.

**Step 3: yml configs (V, 30 min).**
- Copy the missing yml files (table) into `bench/jetpack/config/`, including jetpack's `client_open.yml`, `60c1s5r5p.yml` and the two local `30c1s5r5p-zoo` variants (zoo0..4 and zoo1..5, hosts 127.0.0.1).
- Verify: `cmp` against `~/jetpack/config` shows only the host-block edits in the site files; `grep max_undone bench/jetpack/config/client_open.yml` prints 200.

**Step 4: config parser adapters (CA+H, ½ d).**
- `-m`; `cc: none`; `retry_wait_`; restore `bench:`/`bench_update_weight:` and `schema:`/`sharding:` from `6976bf3c5^`; `rw_benchmark_para_`.
- Verify: a tiny `config_dump` test loads the exact `-f` stack from `build_deptran_cmd` (`experiment_defs.sh:245-273`) for every rw*/YCSB*/concurrent_* combination with `-m 0`, and prints `n_concurrent_`, `client_max_undone_`, `txn_weights_`, `dist_`, `range_` and `coeffcient_`. Compare them with values read from the yml by `yq`/python; they must be identical.

**Step 5: output formats and CPU (CA, ½ d).**
- Add `src/deptran/jetpack_perf/{stats_print,csv_dump,cpu_stat,wan}.{h,cc}`: the jetpack blocks with converted formats, and a `server_core_id` global.
- Verify:
  - a unit test fills synthetic `Distribution`s (known samples), runs the print and dump, and checks the output. `gen_tput_p90_figures.py`'s STAT_RE and `gen_latency_cdf.py` column 5 must parse it; `derive_fixed_conc.py` must choose the expected knee.
  - `grep '%' ` in the converted strings is empty.
  - The ReadCpuStats output on a busy-loop core reads about 100%.

**Step 6: memdb restore (H, 1-3 d).**
- `git show 2e85325b1^:src/memdb/<f>` → `src/memdb/<f>` for all 28 files; `MEMDB_SRC` under `MAKO_JETPACK_PERF`.
- Port the rrr includes and logs, and fix the header order.
- Verify: restore memdb's historical tests if any exist at `2e85325b1^`; otherwise write a 30-line test that creates a `history` table, inserts and queries a row via TxnUnsafe, and reads column 1.

**Step 7: command/procedure/tx/txn_reg/marshal (H, 2-3 d).**
- Restore from `e9c61ecab^` and `a3569a10a^:classic/tx.h`. Resolve kind 4 (register `VecPieceData`; compile out `LegacyVecPieceData` under the flag).
- Verify: a round-trip test serializes a `TpcCommitCommand{cmd_=VecPieceData{rw write piece with Value(i32), Value(str pad)}}` via `Command` and decodes it back equal; `LegacyVecPieceData` is absent from the registry.

**Step 8: workload, rw bench, sharding, zipf (H + jetpack delta, 1 d).**
- Restore `83f7d74fb^:src/bench/tpca/zipf.h`, the `tpcc/workload.h` constants and the `tpcc_real_dist/sharding.*`, `deptran/sharding.*`, `bench/rw/*`, `deptran/workload.*` and `benchmark_registry.*`. Paste jetpack `RW_VALUE_SIZE`.
- Verify:
  - `PopulateTables` with rw_1000000 fills 1,000,000 rows;
  - 1e6 `GetTxRequest` draws give a read/write ratio equal to `txn_weights_` within 0.5%, and uniform keys over `range_`;
  - under `rw_zipf_1`, the top-10 key shares from `Frequency` match a python ZipfDist reference to within 1%;
  - `RW_VALUE_SIZE=100` writes carry a 100-byte `input_[2]`.

**Step 9: server request path (H/CA, 3-4 d).**
- Add `TxScheduler`, `SchedulerClassic`, `SchedulerNone`, `CoordinatorRaft` (rebound to `RaftSpecific`), `TxFrame`, the ServerWorker transaction half, and the learner binding (a new PollThread on the rust lane).
- Verify, with a no-client in-process test in the style of `raft_bench.cc`:
  - 3 replicas; the leader's `SchedulerNone::Dispatch` of 10k write pieces returns SUCCESS after apply;
  - every replica's memdb holds identical `history` rows, compared by checksum;
  - Dispatch on a follower returns WRONG_LEADER with the leader hint;
  - run on all three lanes (`MAKO_RAFT_LANE=rust|hybrid|cpp`).

**Step 10: `deptran_server` lifetime rewrite and Classic RPC (CA/H/R, 2-3 d).**
- Rewrite main on jetpack's shape, duration-driven, built outside `RAFT_TEST`.
- Restore the Classic Dispatch block and regenerate; check and pin the ids in `rpc_ids.txt`; restore `ClassicServiceImpl`; on the rust lane, run a separate srpc::Server on `port+delta`.
- Verify: the build passes the rpcgen_rust id gate; `deptran_server -f none_raft.yml -f 30c1s5r5p-zoo.yml -f rw_1000000.yml -f client_open_raft.yml -f concurrent_1.yml -d 30 -m 0 -P zoo0` stays up for 30 s and exits 0; an srpc test client's Dispatch to the leader returns SUCCESS.

**Step 11: client stack (H/CA/V, 3-4 d).**
- Add `jpclient::Coordinator`; restore `CoordinatorClassic`, the communicator client half and `WaitConnectClientLeaders`.
- Copy jetpack's `CoordinatorNone` verbatim and jetpack's `ClientWorker` with the API translation.
- Add `client_launch_workers`, `client_shutdown`, the `.res`/CSV print and the WAN hops.
- **No-consensus check (method sanity, required before any Raft number is reported).** Add a test-only `JETPACK_PERF_NOCONSENSUS=1` in `ClassicServiceImpl`: it replies right after `SchedulerNone` executes the piece and skips `OnCommit`. Run `WAN_DELAY_MS=20` across `concurrent_{1,50,300,500}`. Checks:
  - `Mid throughput` must equal the offered load, 60 × min(N, max_undone) req/s (the `client_open_raft.yml` comment's formula), to within ±5%. The 60 is the client-site count of the 60-client layouts; see Unverified. Record C × min(N, max_undone) for the layout used.
  - At N=500, throughput must plateau at `max_undone`=300.
  - p50 latency must equal the injected client↔leader round trip (2 × 20 ms = 40 ms) plus under 1 ms.
  - Repeat with `WAN_DELAY_MS=0`: sub-ms p50.
  - `count(6)+count(7)==count(5)` must hold, otherwise `verify(0)` fires.
- Then enable consensus. Expected p50 ≈ 2D (client) + D per replication round (one-way on AE, replies undelayed), plus ≤1 ms for the CoordinatorRaft poll and ≤1 ms for the apply idle sleep.

**Step 12: WAN on Raft sends and core pinning (CA, 1-2 d).**
- Rust transport fiber-per-send; cpp commo the same; `SERVER_CORE_ID` pinned before the child threads are created.
- Verify:
  - with 1 client at `WAN_DELAY_MS=20`, p50 ≈ 60 ms on both lanes;
  - with 5 followers, latency does not grow as N·D (proves the sends do not stack);
  - `/proc/<pid>/task/*/status` `Cpus_allowed_list` shows every Raft thread on the pinned core.

**Step 13: leader placement (CA, h).**
- Call `RaftSpecific::SetPreferredLeader` (`scheduler.h:240`) for the replica the scripts assume leads: locale 0 for the 10-run_all/zoo0 layout, s201/zoo2 for run_adaptive_sweep.
- Verify: `IsLeader` is true on the expected process after startup in 10 of 10 runs.

**Step 14: full runs (V).**
- `10-run_all.sh --exp 0`, then `derive_fixed_conc.py`, then `--exp 1,2`, then the camera-ready figures. Repeat the whole sequence with `MAKO_RAFT_LANE=cpp` and `hybrid` into separate exp dirs. Also run `run_tier1_batch.sh` and `run_akkio_exp.sh` for per-host and cross-host latency.
- Verify: `sanity_check.py` (1-RTT/2-RTT under WAN_DELAY_MS=20) and `csv_audit.py` pass unchanged.

Rough total: 3-4 weeks of focused work. Steps 0-5 (about 3 days) give a fully working jetpack analysis toolchain before any C++ request path exists.

## Honest limits

**Not copied, and why:**
- **jetpack's own Raft** (`raft/server.*`, `commo.cc`, `service.cc`). It is the system under test.
- **Read lease, recovery plane, JetPack rule_raft fast path, failover experiment.** Mako has none of these (no `HasReadLease`), and the recovery plane was retired in `e048406a6`. The failover port is deferred (`6505e23ce`).
- **Docker, backend (etcd/zk/mongo) and tc/netem runs.** There is no docker and no root. WAN is software delay only, which is what jetpack's own zoo runs use (`WAN_DELAY_MS=20`).
- **Multi-host.** Everything shares one 64-core machine and loopback, so "per-host CPU" columns are machine totals. `pkill -f deptran_server` in `09:441` and `98-kill.sh:37` kills every replica.

**Method differences that remain after copying (report them, don't hide them):**
- Mako allows 1 in-flight AppendEntries per follower with ≤256 entries (`server_cc.rs:1104`, `server.cc:233`); jetpack pipelines up to 8000 (`constants.h:219-230`). WAN throughput ceilings differ; tune `MAKO_RAFT_APPEND_BATCH_MAX_ENTRIES` and report it.
- On the rust lane, apply is a separate thread with a 1 ms idle sleep (`server_h.rs:3320,~3370`), plus the PollThread hop into the Tx thread. This adds up to about 1 ms that jetpack's same-thread apply does not.
- The replicated payload is Mako's `Command`/`TpcCommitCommand` encoding, not jetpack's MarshallDeputy, so bytes per entry differ.
- `CoordinatorRaft`'s term check becomes `!IsLeader()`, because `RaftSpecific` has no term getter. A leader that changes term but stays leader is not detected. Adding `CurrentTerm()` would be a Rust-side change.

**The restore is a forward port, not a revert (correction).**
- The snapshots span 12 commits of one day (`6505e23ce` 00:25 → `2e85325b1` 18:24), predate `47fe7a97c` (rrr→srpc) and `3e102604d` (SRPC API sync, const handlers, module imports), and are consistent only with their own trees.
- Pick one base snapshot per subsystem and expect API skew between them.
- The historical whole-tree none_raft path ran against the old C++ RaftServer fields (169 raft commits ago), so it cannot simply be revived.

**Unverified:**
- **The "60" in the offered-load formula.** It is taken from the `client_open_raft.yml` comment. My reading is that it counts client sites (60 in the 60c layouts, 30 in `30c1s5r5p-zoo`) at about 1 req/s per coroutine (mean 1 s sleep). Step 11 must confirm it empirically before using it as a pass criterion.
- **How long the restores take.** memdb, procedure, scheduler and communicator have not been compiled against today's srpc and C++23 module build; the estimates are extrapolated from the diffs.
- **Classic ids.** Whether the regenerated Classic RPC ids collide with the frozen Raft ids.
- **Rust-lane learner hop performance.** The throughput and latency cost of the rust-lane learner hop has not been measured.
- **`derive_fixed_conc.py` on other layouts.** It works only for the zoo0..zoo4 layout; others need the 2-line `main()` edit (`:290-291`).
- **Leader placement.** Whether `SetPreferredLeader` reliably places the leader on the replica the scripts assume, across all three lanes.
