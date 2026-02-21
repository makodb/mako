# Mako User Manual

## Table of Contents

1. [What This Manual Covers](#what-this-manual-covers)
2. [Current Feature Status](#current-feature-status)
3. [System Requirements](#system-requirements)
4. [Install and Build](#install-and-build)
5. [Quick Verification](#quick-verification)
6. [How To Run Mako](#how-to-run-mako)
7. [Configuration](#configuration)
8. [CLI and Runtime Options](#cli-and-runtime-options)
9. [Transport Backends](#transport-backends)
10. [Persistence and CPU Throttling](#persistence-and-cpu-throttling)
11. [Client-Server Mode Status](#client-server-mode-status)
12. [Testing and CI](#testing-and-ci)
13. [Docker Workflow](#docker-workflow)
14. [Known Limitations](#known-limitations)
15. [Troubleshooting](#troubleshooting)
16. [Documentation Map](#documentation-map)

## What This Manual Covers

This manual is a code-verified guide for running the current Mako repository.

It was produced by reconciling the `docs/` folder with the implementation in:
- `src/mako/`
- `examples/`
- `ci/`
- top-level build scripts (`Makefile`, `CMakeLists.txt`, `docker_build.sh`)

It intentionally avoids treating roadmap/plan docs as already-shipped behavior.

## Current Feature Status

Implemented and usable today:
- Local transaction tests (`simpleTransaction`)
- Paxos replication tests (`simplePaxos`, `dbtest` with replication enabled)
- Optional Raft replication path via runtime flag (`--replication=raft`) and `make mako-raft`
- Multi-shard execution, including single-process multi-shard mode (`--local-shards`)
- Runtime transport switch (`MAKO_TRANSPORT=rrr|erpc`)
- CPU throttling flags in `dbtest` (`--cpu-limit`, `--throttle-cycle`)
- RocksDB persistence path in replicated leader mode

Present but not fully integrated:
- Config-node startup flags (`--is-config-node`, `--config-node-addr`, etc.) are currently stubs in `src/mako/mako.hh`
- `simpleTransactionRep --client` path is still incomplete for end-to-end transactions in current code

## System Requirements

Recommended host OS:
- Ubuntu 24.04 (matches `apt_packages.sh` and Docker files)

Also used in project docs/README:
- Debian 12

Minimum practical hardware for local testing:
- 4 CPU cores
- 8 GB RAM
- 20 GB free disk

## Install and Build

### 1. Clone with submodules

```bash
git clone --recursive https://github.com/makodb/mako.git
cd mako
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

### 2. Install system dependencies

```bash
bash apt_packages.sh
```

### 3. Install Rust toolchain used by this repo

```bash
source install_rustc.sh
```

### 4. Generate/update config artifacts

```bash
bash src/mako/update_config.sh
```

### 5. Build

```bash
make -j$(nproc)
```

Common build variants:

```bash
make                 # default (Paxos build path)
make mako-raft       # enable Raft helper/binaries
make raft-test       # Raft lab test build mode
```

## Quick Verification

Fast sanity checks:

```bash
./ci/ci.sh simpleTransaction
./ci/ci.sh simplePaxos
./ci/ci.sh shardNoReplication
```

Full Paxos CI suite:

```bash
./ci/ci.sh all
```

Raft CI suite:

```bash
./ci/ci_mako_raft.sh all
```

## How To Run Mako

### Option A: Use existing test scripts (recommended)

These are the most reliable entry points:

```bash
bash examples/test_2shard_no_replication.sh
bash examples/test_1shard_replication.sh
bash examples/test_2shard_replication.sh
bash examples/test_multi_shard_single_process.sh
```

### Option B: Run binaries directly

These commands use `BUILD_DIR` when set, and default to `build` otherwise.

#### `simpleTransaction`

```bash
./${BUILD_DIR:-build}/simpleTransaction
```

Uses `MAKO_CONFIG` if set, otherwise falls back to a local config template.

#### `simplePaxos`

Pass a required role argument:

```bash
./${BUILD_DIR:-build}/simplePaxos localhost
./${BUILD_DIR:-build}/simplePaxos p1
./${BUILD_DIR:-build}/simplePaxos p2
./${BUILD_DIR:-build}/simplePaxos learner
```

Important:
- Each role command above is a long-running process and expects peer roles to be started concurrently (typically in separate terminals).
- Running only one role (for example, only `localhost`) will wait indefinitely for the rest of the cluster.

For multi-process orchestration, use:

```bash
bash examples/simplePaxos.sh
```

#### `dbtest` baseline (single shard, non-replicated)

```bash
./${BUILD_DIR:-build}/dbtest \
  --num-threads 6 \
  --shard-index 0 \
  --shard-config src/mako/config/local-shards1-warehouses6.yml \
  -P localhost
```

#### `dbtest` with replication (Paxos)

```bash
./${BUILD_DIR:-build}/dbtest \
  --num-threads 6 \
  --shard-index 0 \
  --shard-config src/mako/config/local-shards1-warehouses6.yml \
  -P localhost \
  -F config/1leader_2followers/paxos6_shardidx0.yml \
  -F config/occ_paxos.yml \
  --startup-timeout-sec 60 \
  --is-replicated \
  --replication=paxos
```

#### `dbtest` with replication (Raft runtime switch)

```bash
./${BUILD_DIR:-build}/dbtest \
  --num-threads 6 \
  --shard-index 0 \
  --shard-config src/mako/config/local-shards1-warehouses6.yml \
  -P localhost \
  -F config/1leader_2followers/paxos6_shardidx0.yml \
  -F config/occ_raft.yml \
  --startup-timeout-sec 60 \
  --is-replicated \
  --replication=raft
```

Important:
- In replicated configs like `config/1leader_2followers/paxos6_shardidx0.yml`, `-P localhost` starts only one role group.
- You must start peer role groups (for example `-P p1`, `-P p2`, and `-P learner`) concurrently, or startup can wait indefinitely.
- `dbtest` now prints an explicit startup warning when replicated mode is launched with `-P localhost` to highlight this requirement.
- Use `--startup-timeout-sec <seconds>` (or `MAKO_STARTUP_TIMEOUT_SEC`) to fail fast instead of hanging indefinitely.
- In non-interactive runs, when no timeout is provided, `dbtest` now applies a default startup timeout of 120 seconds.
  Use value `0` to explicitly disable the startup watchdog.
- For local end-to-end replicated runs, prefer `bash examples/test_1shard_replication.sh` or `bash examples/test_2shard_replication.sh`.

#### Multi-shard single-process mode

```bash
./${BUILD_DIR:-build}/dbtest \
  --num-threads 6 \
  --shard-config src/mako/config/local-shards2-warehouses6.yml \
  -P localhost \
  --local-shards=0,1
```

## Configuration

Mako currently supports two shard config formats in `transport::Configuration`.

### Format 1: Old format (used by most current scripts)

Example file:
- `src/mako/config/local-shards2-warehouses6.yml`

Expected keys include:
- `shards`
- `warehouses`
- shard address groups: `localhost`, `p1`, `p2`, `learner`
- memory ports: `memlocalhost`, `memlearner`, `memp1`, `memp2`

### Format 2: New format (`sites` + `shard_map`)

Example file:
- `config/mako_new_format.yml`

Key fields:
- `sites`: named endpoints (`name`, `id`, `ip`, `port`)
- `shard_map`: list of replica name lists per shard
- optional `warehouses` and memory port keys

When using this format with `dbtest`, pass `--site-name` so the process can map itself to shard/role.

### Helpful environment variables

- `MAKO_CONFIG`: used by some example binaries/scripts to override default config path
- `MAKO_TRANSPORT`: runtime transport backend (`rrr` default, or `erpc`)
- `MAKO_STARTUP_TIMEOUT_SEC`: optional startup watchdog timeout for replicated localhost startup (`0` disables)
- `BUILD_DIR`: lets CI/scripts use non-default build directories

## CLI and Runtime Options

`dbtest` options currently parsed in `src/mako/benchmarks/dbtest.cc`:

- `--num-threads`
- `--shard-index`
- `--shard-config`
- `--paxos-config` (repeatable)
- `--paxos-proc-name` (`-P`)
- `--site-name`
- `--local-shards`
- `--cpu-limit`
- `--throttle-cycle`
- `--sync-dir`
- `--replication` (`paxos` or `raft`)
- `--startup-timeout-sec` (optional fail-fast guard for replicated localhost startup)
- `--is-config-node`
- `--config-node-addr`
- `--config-db-path`
- `--config-port`
- `--is-micro`
- `--is-replicated`

Note: `dbtest` does not currently expose a standard `--help` output.

## Transport Backends

Runtime selection:

```bash
MAKO_TRANSPORT=rrr  ./${BUILD_DIR:-build}/dbtest ...   # default behavior
MAKO_TRANSPORT=erpc ./${BUILD_DIR:-build}/dbtest ...
```

Additional build context:
- `env.txt` influences eRPC transport build settings (`eth`, `ib`, `dpdk`)
- default `env.txt` value is `eth`

## Persistence and CPU Throttling

### RocksDB persistence

- Replicated leader path initializes RocksDB persistence in `init_env()`
- Default location pattern:
  - `/tmp/${USER}_mako_rocksdb_shard${shard}_leader_pid${pid}`

### CPU throttling

Per-worker throttling flags:

```bash
./${BUILD_DIR:-build}/dbtest ... --cpu-limit 5 --throttle-cycle 100
```

- `--cpu-limit`: percentage in `[0,100]`
- `--throttle-cycle`: duty cycle in ms

## Client-Server Mode Status

`simpleTransactionRep` modes:

```bash
./${BUILD_DIR:-build}/simpleTransactionRep <nshards> <shardIdx> <nthreads> <paxos_proc_name> <is_replicated> [replication_type]
./${BUILD_DIR:-build}/simpleTransactionRep --server <nshards> <shardIdx> <nthreads> <paxos_proc_name> <is_replicated> [replication_type]
./${BUILD_DIR:-build}/simpleTransactionRep --client <server_host> <server_port>
```

Current status in this repository version:
- `--server` mode starts and waits for shutdown
- `--client` mode can establish a connection in some setups
- end-to-end remote transaction operations are still incomplete (e.g. BeginTransaction failures observed)

For this reason, treat remote client mode as experimental and do not use it as a production workflow yet.

## Testing and CI

Primary CI script (`ci/ci.sh`) supports:

- `compile`
- `simpleTransaction`
- `simplePaxos`
- `shardNoReplication`
- `shardNoReplicationErpc`
- `shard1Replication`
- `shard2Replication`
- `shard2ReplicationErpc`
- `shard1ReplicationSimple`
- `shard2ReplicationSimple`
- `shard1ReplicationRaft`
- `shard2ReplicationRaft`
- `shard1ReplicationSimpleRaft`
- `shard2ReplicationSimpleRaft`
- `rocksdbTests`
- `multiShardSingleProcess`
- `shard2SingleProcess`
- `shard2SingleProcessReplication`
- `rrrTests`
- `cpuThrottlingScaling`
- `clientServer`
- `all`

Note: `shardFaultTolerance` is currently disabled in `ci/ci.sh`.

## Docker Workflow

Current Docker assets are Ubuntu 24-based.

Main commands:

```bash
./docker_build.sh build-image
./docker_build.sh build
./docker_build.sh test
./docker_build.sh shell
./docker_build.sh ci all
./docker_build.sh compose-up
```

Notes:
- `./docker_build.sh build` supports incremental rebuilds: it reuses a compatible `build_docker` cache and skips CMake reconfigure unless cache is missing/incompatible.
- `./docker_build.sh build` compiles core runtime binaries: `dbtest`, `simpleTransaction`, `simplePaxos`, and `simpleTransactionRep`.
- It also compiles RocksDB quick-test binaries used by `ci-quick rocksdbTests`: `test_rocksdb_persistence`, `test_callback_demo`, `test_ordered_callbacks`, `test_partitioned_queues`, and `test_stress_partitioned_queues`.
- If you open `shell`/`create`/`enter`/`compose-up` before running `build`, in-container `BUILD_DIR=build_docker ./ci/ci.sh ...` commands can fail due to missing binaries.
  Run `./docker_build.sh build` first (or use `./docker_build.sh ci <test>` which builds and runs in one step).
- `./docker_build.sh test` performs a focused Docker smoke test.
- It reuses `build_docker/dbtest` when that binary is Docker-compatible (RUNPATH contains `/workspace/build_docker`); otherwise it rebuilds `dbtest`.
- It then runs `BUILD_DIR=build_docker ./ci/ci.sh shardNoReplication`.
- Script-driven Docker commands (`build`, `test`, `ci`, `ci-quick`) disable core dumps by default to avoid large `core.*` files in the workspace after transient crashes.
  Set `MAKO_DOCKER_ENABLE_COREDUMP=1` to re-enable core dumps for debugging.
- `./docker_build.sh clean` removes `build_docker`, `target-docker`, and top-level `core.*` crash artifacts (using a helper container when host permissions block deletion).
- Image-dependent commands (`build`, `shell`, `test`, `ci`, `ci-quick`, `create`, `compose-up`) auto-build `mako-build:ubuntu24` when the image is missing locally.
- `./docker_build.sh shell`/`create`/`enter` set `BUILD_DIR=build_docker` by default so `./ci/ci.sh ...` in the container uses Docker-built artifacts.
- Persistent Docker dev containers use an init/reaper process (`--init` for standalone `mako-dev`, `init: true` for compose `dev`) to prevent zombie child-process buildup during repeated test runs.
- `./docker_build.sh shell`/`create`/`enter` auto-detect terminal availability: interactive TTY sessions use `-it`, while non-interactive environments fall back to non-TTY-safe Docker/Compose flags.
- In non-interactive mode, `./docker_build.sh shell` prints guidance instead of trying to open an interactive shell (running standalone `mako-dev` guidance when available, explicit `docker start` guidance when standalone exists but is stopped, direct compose-exec guidance when compose `dev` is already running, and compose-start guidance otherwise).
- If both standalone `mako-dev` and compose `dev` are running, `shell` guidance includes both access paths (`docker exec ... mako-dev` and `docker compose exec dev ...`).
- In non-interactive mode, `./docker_build.sh create` starts `mako-dev` in the background (detached) instead of trying to open an interactive shell.
- `./docker_build.sh create` reuses an existing standalone `mako-dev` container instead of failing on name conflicts.
- `./docker_build.sh create` and `./docker_build.sh enter` auto-upgrade legacy standalone `mako-dev` containers created without Docker init support, recreating them with `--init` so child processes are reaped correctly.
- `./docker_build.sh enter` falls back to compose `dev` when standalone `mako-dev` does not exist, and auto-starts/bootstrap the compose service if needed. In non-interactive mode it prints guidance, including the `docker compose exec -T dev /bin/bash -lc '<command>'` pattern.
- When both standalone `mako-dev` and compose `dev` exist, `./docker_build.sh enter` prefers standalone `mako-dev`; use `docker compose exec dev /bin/bash` to enter the compose container.
- For non-interactive usage against the standalone `mako-dev` container, run: `docker exec -e BUILD_DIR=build_docker mako-dev /bin/bash -lc '<command>'`.
- `docker compose` services also export `BUILD_DIR=build_docker` for the same reason.
- For compose-based sessions, use `docker compose exec dev /bin/bash` (works regardless of generated container name).
- For non-interactive compose usage (for example CI/headless), use `docker compose exec -T dev /bin/bash -lc '<command>'`.
- `./docker_build.sh ci <test> <jobs>` accepts an optional jobs argument.
- Example: `./docker_build.sh ci all 8` to run CI with 8 build jobs.
- Jobs-only shorthand is supported: `./docker_build.sh ci 8` is equivalent to `./docker_build.sh ci all 8`.
- `./docker_build.sh ci-quick <test>` skips rebuild and validates suite binaries up front (exists, executable, Docker-compatible RUNPATH); use `./docker_build.sh ci <test>` when `ci-quick` reports a binary issue.
- `rrrTests` is not supported in `ci-quick` because it runs broad CTest suites; use `./docker_build.sh ci rrrTests`.
- `shard1ReplicationSimple`, `shard2ReplicationSimple`, `shard1ReplicationSimpleRaft`, and `shard2ReplicationSimpleRaft` require `build_docker/simpleTransactionRep`; `test` alone may not produce that binary.

Relevant files:
- `Dockerfile.ubuntu24`
- `docker-compose.yml`
- `docker_build.sh`

## Known Limitations

- Some docs in `docs/getting-started/` and `docs/configuration/` are stale relative to code
- Config-node flags are present but full config-node integration is not complete
- `simpleTransactionRep --client` flow is still under active integration
- A large portion of `docs/plans/`, `docs/migration/`, and `docs/dev/` are design/plan artifacts, not guaranteed runtime behavior

## Troubleshooting

### Build fails due missing submodules

```bash
git submodule update --init --recursive
```

### Port already in use

```bash
pkill -9 -f dbtest
pkill -9 -f simpleTransactionRep
sleep 2
```

### Stale test artifacts

```bash
make clean
rm -rf /tmp/${USER}_mako_rocksdb_shard*
rm -f nfs_sync_*
```

### Wrong transport backend selected

```bash
echo "$MAKO_TRANSPORT"
# unset or set explicitly:
unset MAKO_TRANSPORT
# or
export MAKO_TRANSPORT=erpc
```

## Documentation Map

Good starting points in `docs/` for current users:

- `docs/index.md`
- `docs/getting-started/introduction.md`
- `docs/architecture/overview.md`
- `docs/architecture/multi-shard.md`
- `docs/developer/transport-backends.md`
- `docs/performance/cpu_throttling.md`

Use with caution (planning/design docs):

- `docs/plans/**`
- `docs/migration/**`
- `docs/dev/**`
- many `docs/rpc/phase*.md` files
