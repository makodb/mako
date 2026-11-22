# Multi-Paxos replication: deprecated

Decision by the repository owner, 2026-11-22, verbatim:

> Remove entirely Paxos related tests, only keep Raft related code. Mark all Paxos code deprecated.

Second decision by the repository owner, 2026-11-22, verbatim:

> flip the default to Raft.

Raft (`src/deptran/raft/`) is the supported and tested replication engine, and
the default one. The Multi-Paxos implementation stays in the tree and still
builds, but it is deprecated: nothing tests it, nobody should extend it, new
work goes to Raft, and it runs only when a run names it.

## What "deprecated" means here

- **Still built.** The Paxos sources are compiled into the same libraries as
  before (`DEPTRAN_CORE_SRC` and `TXLOG_HELPER_SRC` in the root
  `CMakeLists.txt`). No option turns them off.
- **Not the default; selectable only explicitly.** A process starts with
  `ReplicationType::RAFT` (`g_replication_type` in
  `src/deptran/replication_helper.cc`). Paxos runs only when a run names it,
  in one of two ways:
  - an explicit choice: `--replication paxos` on `dbtest` or
    `sto_tpcc_bench`, a call to `set_replication_type*`, or a `--replication`
    argument in the argv handed to `setup()`;
  - a `-f`/`-F` mode file whose `mode.ab` says `paxos` or `multi_paxos`
    (`config/paxos.yml` says `multi_paxos`).

  An explicit choice wins over the mode files; a mode file wins over the
  default. `ab: raft` (`config/raft.yml`) selects Raft the same way. The
  seventh argument of `bash/shard.sh`, the optional replication type argument
  of `examples/simpleTransactionRep.cc` and the third argument of
  `make_paxos_replication_configs` default to `raft`; pass `paxos` to get the
  deprecated engine. Before the second decision above all of these defaulted
  to Paxos.
- **Not tested.** The decision removes every test that ran Paxos replication:
  the `ci/ci.sh` suites `simplePaxos`, `shard1Replication`,
  `shard2Replication`, `shard1ReplicationSimple`, `shard2ReplicationSimple`
  and `shard2SingleProcessReplication`; the four CTests
  `test_sto_tpcc_protocol_{cpp,rust}_paxos{,_colocated}`; the gtest cases
  `ReplicationOutboxTest.*` and `PaxosLearnerPrefixTest.*`; and the
  `simplePaxos`, `paxos_async_commit_test` and `network_bench` programs with
  their scripts. A change to the files below is therefore checked by the
  compiler only.
- **One warning at start-up.** `paxos_impl::setup`
  (`src/deptran/paxos_main_helper.cc`) logs one line that contains the token
  `[PAXOS-DEPRECATED]`. The unified `setup()` dispatches there only after the
  process has resolved to Paxos, so a Raft or an unreplicated process never
  prints it. No other statement prints the token: a log that lacks it comes
  from a run that did not start Paxos. Do not move the warning into the
  dispatch macros or into `paxos_impl::register_*`. `init_env` in
  `src/mako/mako.hh` now detects the type from the mode files before it
  registers the leader-election callback, but `setup()` parses `mode.ab`
  itself and has the last word, so a registration can still reach an engine
  that does not run (`init_env` then registers again with the right one).
- **No `[[deprecated]]` attributes.** The build's flags start with `-w`, so an
  attribute would warn nobody today, and shared files name Paxos types
  (`src/deptran/frame.cc`, `src/deptran/replication_helper.cc`,
  `src/deptran/mako_commands.h`), so it would become an error the day
  warnings are turned into errors. The mark is this file, the one-line
  `// DEPRECATED:` notice near the top of each file below, and the warning.

## The deprecated file set

Each of these carries the one-line notice:

- `src/deptran/paxos/commo.h`, `commo.cc`: `MultiPaxosCommo`,
  `PaxosAcceptQuorumEvent`
- `src/deptran/paxos/coordinator.h`, `coordinator.cc`:
  `CoordinatorMultiPaxos`, `BulkCoordinatorMultiPaxos`
- `src/deptran/paxos/frame.h`, `frame.cc`: `MultiPaxosFrame`
- `src/deptran/paxos/server.h`, `server.cc`: `PaxosServer`, `PaxosData`
- `src/deptran/paxos/service.h`, `service.cc`: `MultiPaxosServiceImpl`
- `src/deptran/paxos/replication_outbox.h`: `ReplicationOutbox`,
  `ReplicationMessage`
- `src/deptran/paxos_worker.h`, `paxos_worker.cc`: `PaxosWorker`, the Paxos
  payload classes, `pxs_workers_g`
- `src/deptran/paxos_main_helper.cc`: namespace `paxos_impl`

Headers with neutral names whose only users are the files above (no notice
was added to them):

- `src/deptran/coordinator.h` (`janus::Coordinator`)
- `src/deptran/distribution.h`
- `src/deptran/concurrentqueue.h` (vendored third-party code; leave its text
  alone)

Still needed to launch the deprecated engine by hand, and kept for that
reason: `config/paxos.yml`, `config/1leader_2followers/generator.py` with its
`template_paxos1_shardidx*.yml` templates, and `src/mako/update_config.sh`,
which generates the untracked `paxos<N>_shardidx<S>.yml` files. A hand launch
names the engine, for example `bash bash/shard.sh 1 0 <threads> <role> 0 1
paxos` for each of the roles `localhost`, `learner`, `p2` and `p1`.
`scripts/legacy/run_experiment.py` passes that seventh argument, because its
four-role topology is the Paxos one.

## Shared with Raft: NOT deprecated, whatever the name says

Many identifiers say "paxos" for historical reasons and are used by the Raft
path. They stay, and they are not deprecated:

- `ElectionState` in `src/deptran/paxos_worker.h`
  (`src/deptran/raft_main_helper.cc` includes the header for it).
- `enum PaxosStatus` in `src/deptran/paxos/commo.h`
  (`src/deptran/raft/raft_worker.cc` includes the header for it), and
  `mako::PaxosStatus` in `src/mako/lib/common.h`.
- The unified replication API in `src/deptran/replication_helper.h`: `setup`,
  `setup2`, `shutdown_paxos`, `add_log_to_nc`, the `register_*` functions,
  `get_epoch` and the rest. Each one dispatches to `raft_impl` when the type
  is Raft. `shutdown_paxos()` is the Raft teardown too.
- `enum ReplicationType`, `g_replication_type` and the functions around
  them: the shared selector. Its default is `RAFT`; the `PAXOS` enumerator is
  how the deprecated engine is named.
- `register_paxos_leader_callback`, `setup_paxos_leader_callbacks`,
  `setup_paxos_follower_callbacks` and `prepare_paxos_args` in
  `src/mako/mako.hh`: called for both engines.
- The `--paxos-config`/`-F` and `--paxos-proc-name`/`-P` flags and the
  `BenchmarkConfig` accessors named after them: Raft runs pass them too.
- `MAKO_PAXOS_CONFIG_DIR`, the `/tmp/mako_paxos_cfg_*` directories and the
  helpers in `examples/simple_transaction_rep_port_utils.sh`
  (`make_paxos_replication_configs` and the others): the Raft suites call them
  with `raft`.
- `bash/shard.sh`: the launcher of the Raft suites and of the unreplicated
  ones.
- `TxLogServer` and the `TXLOG_SERVER_SITE_*` macros in
  `src/deptran/scheduler.h`, `Frame::GetFrame` in `src/deptran/frame.cc`,
  `src/deptran/replication_quorum.h`, `server_status.h` and
  `benchmark_control_rpc.*`: both servers use them.
- `reg_learner_action`: the apply-callback hook Raft uses. Only the learner
  *role* is Paxos-only (see below).

## Paxos-only regions inside shared files

No file notice can cover these; they are deprecated with the rest:

- `abstract service MultiPaxos` in `src/deptran/rcc_rpc.rpc`, and the
  `MultiPaxosService`/`MultiPaxosProxy` classes rpcgen generates from it into
  `src/deptran/rcc_rpc.h`. Do not delete the service: rpcgen assigns wire ids
  from the services present, and Raft's ids
  (`src/deptran/raft/rpc_ids.txt`) are checked against the same header.
- The Paxos payload kinds in `src/deptran/mako_commands.h`: `BulkPaxosCmd`,
  `BulkPrepareLog`, `HeartBeatLog`, `SyncLogRequest`, `SyncLogResponse`,
  `SyncNoOpRequest`, `PaxosPrepCmd` (kinds 5 to 11).
- `MODE_MULTI_PAXOS` (`src/deptran/constants.h`) and the `multi_paxos`/`paxos`
  branch of the `ab` key in `src/deptran/config.cc`.
- The learner role: the failover cases in `src/mako/mako.hh` that follow the
  `is_using_raft()` early returns, the learner failure monitor started in the
  same file for the `LEARNER_CENTER` cluster, the `LEARNER_CENTER_INT` and
  `"learner"` branches in `src/mako/benchmarks/bench.cc`, and the `"learner"`
  process branches and `Config::UpgradeFromLearnerToLeader` in
  `src/deptran/config.cc`. Raft has no learner.
- `janus::is_using_paxos()` in `src/deptran/replication_helper.h`: no caller.

## Do not edit

- `src/deptran/rcc_rpc.h`: generated by rpcgen; a hand edit reverts at the
  next regeneration.
- Anything between an `#if RUSTYCPP_RUST` line and its `RUSTYCPP:GEN-END`
  marker in `src/deptran/mako_commands.h`, `src/deptran/scheduler.h`,
  `src/deptran/communicator.h` and `src/deptran/raft_main_helper.cc`: each
  block is pinned by a `rust_sha256` in its `RUSTYCPP:GEN-BEGIN` marker, and
  `scripts/raft_dsl.sh --check` compares the last three files with their
  regeneration.
- `src/deptran/concurrentqueue.h`: vendored.
- Anything under `src/srpc`: a vendored subtree. Its
  `tests/rpc_marshallable_proxy_test.cc` includes `deptran/paxos_worker.h`,
  and its `tests/rpc_replication_config_generation_test.sh` calls
  `ensure_paxos_replication_configs`; Mako builds neither.
- The code of `src/deptran/paxos/server.h` and `server.cc`:
  `scripts/raft_field_census.py` treats every file named `server.h` or
  `server.cc` under `src/deptran` as a Raft server carrier, so a `this->X`
  there that names a Raft field counts as a violation. Comments are invisible
  to it.
- The notice lines must contain neither of the two rusty-cpp annotation
  tokens (an at sign followed by `safe` or `unsafe`): the borrow checker
  reads those from comments.
