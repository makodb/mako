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

Full GA/CI matrix:

```bash
./docker_build.sh ci all
```

Local Paxos CI shortcut:

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
- Docker build flows now persist Cargo registry/git caches in workspace directory `.cargo-docker` (`CARGO_HOME=/workspace/.cargo-docker`) so repeated `build`/`test`/`ci` runs do not re-download crates each time.
- `./docker_build.sh build` compiles core runtime binaries: `dbtest`, `simpleTransaction`, `simplePaxos`, `simpleTransactionRep`, and `continuousTransactions`.
- It also compiles RocksDB quick-test binaries used by `ci-quick rocksdbTests`: `test_rocksdb_persistence`, `test_callback_demo`, `test_ordered_callbacks`, `test_partitioned_queues`, and `test_stress_partitioned_queues`.
- If you open `shell`/`create`/`enter`/`compose-up` before running `build`, in-container `BUILD_DIR=build_docker ./ci/ci.sh ...` commands can fail due to missing binaries.
  Run `./docker_build.sh build` first (or use `./docker_build.sh ci <test>` which builds and runs in one step).
- `./docker_build.sh test` performs a focused Docker smoke test.
- It reuses `build_docker/dbtest` when that binary is Docker-compatible (RUNPATH contains `/workspace/build_docker`); otherwise it rebuilds `dbtest`.
- It then runs `BUILD_DIR=build_docker ./ci/ci.sh shardNoReplication`.
- To reduce noisy timeout/retry flakes in Docker smoke runs, `./docker_build.sh test` uses a default wait timeout of 180 seconds.
  Override with `MAKO_DOCKER_TEST_MAX_WAIT_SECONDS=<seconds>` (preferred), or `MAKO_MAX_WAIT_SECONDS=<seconds>` when the Docker-specific override is unset or invalid.
- `MAKO_DOCKER_TEST_MAX_WAIT_SECONDS` and `MAKO_MAX_WAIT_SECONDS` must be positive integers; invalid values now emit warnings and fall back to safe defaults across Docker smoke plus no-replication/replication/single-process example scripts instead of causing integer-expression errors.
- Script-driven Docker commands (`build`, `test`, `ci`, `ci-quick`) disable core dumps by default to avoid large `core.*` files in the workspace after transient crashes.
  Set `MAKO_DOCKER_ENABLE_COREDUMP=1` to re-enable core dumps for debugging.
- Script-driven Docker commands (`build`, `test`, `ci`, `ci-quick`) now run as host UID:GID by default to keep `build_docker`, `target-docker`, and generated logs writable from the host checkout.
  Override with `MAKO_DOCKER_SCRIPT_USER=root` if root-owned artifacts are intentionally desired.
- When stale root-owned `build_docker`/`target-docker`/`.cargo-docker` directories exist from older runs, these commands now auto-normalize ownership before executing.
- `./docker_build.sh clean` removes `build_docker`, `target-docker`, `.cargo-docker`, and top-level `core.*` crash artifacts (using a helper container when host permissions block deletion).
- Image-dependent commands (`build`, `shell`, `test`, `ci`, `ci-quick`, `create`, `enter`, `compose-up`) auto-build `mako-build:ubuntu24` when the image is missing locally.
- `./docker_build.sh shell`/`create`/`enter` set `BUILD_DIR=build_docker` by default so `./ci/ci.sh ...` in the container uses Docker-built artifacts.
- Interactive standalone `shell`/`create`/`enter` sessions print a tip that `BUILD_DIR` is preset to `build_docker` for CI/scripts.
- Standalone `shell`/`create`/`enter` exec sessions now run as host UID:GID by default (via `docker exec --user ...`) to avoid writing root-owned files into the checkout. Override with `MAKO_DOCKER_DEV_USER=root` if root shells are required.
- Compose-backed `shell`/`enter` sessions now also run `docker compose exec` with host UID:GID by default (via `--user ...`) for the same reason. Use `MAKO_DOCKER_DEV_USER=root` if root compose shells are required.
- Persistent Docker dev containers use an init/reaper process (`--init` for standalone `mako-dev`, `init: true` for compose `dev`) to prevent zombie child-process buildup during repeated test runs.
- `./docker_build.sh shell`/`create`/`enter` auto-detect terminal availability: interactive TTY sessions use `-it`, while non-interactive environments fall back to non-TTY-safe Docker/Compose flags.
- In non-interactive mode, `./docker_build.sh shell` prints guidance instead of trying to open an interactive shell (running standalone `mako-dev` guidance when available, recovery-safe standalone guidance when standalone exists but is stopped, direct compose-exec guidance when compose `dev` is already running, and compose-start guidance otherwise). For stopped standalone containers, guidance points to recovery-safe script flows rather than raw `docker start`: use `./docker_build.sh create` for explicit standalone recovery, or stop compose services first and then use `./docker_build.sh enter` if you need standalone.
- `./docker_build.sh shell` now requires standalone `mako-dev` to stay running for a short stability window and pass image/workspace/init/keepalive checks before advertising direct `docker exec` commands, so transient or mismatched legacy containers are redirected to recovery-safe guidance.
- If both standalone `mako-dev` and compose `dev` are running, `shell` guidance includes both access paths (`docker exec ... mako-dev` and checkout-scoped compose commands like `MAKO_COMPOSE_PROJECT=... docker compose exec ...`).
- In non-interactive mode, `./docker_build.sh create` starts `mako-dev` in the background (detached) instead of trying to open an interactive shell.
- In interactive TTY mode, `./docker_build.sh create` now also bootstraps `mako-dev` with a persistent keepalive command and then attaches via `docker exec`, so exiting the shell keeps the container running for later `enter` sessions.
- If compose `dev` is already running for this checkout (current project or stale project), `./docker_build.sh create` warns that it is starting/recovering standalone in parallel; once standalone is running, `./docker_build.sh enter` will prefer standalone. The warning also includes project-scoped teardown commands (`MAKO_COMPOSE_PROJECT=... docker compose down`) so you can clean up compose sessions explicitly.
- When multiple stale compose projects exist for this checkout, `./docker_build.sh create` now prints explicit per-project cleanup commands (`MAKO_COMPOSE_PROJECT=... docker compose down`) so you can clean up without manually reconstructing project IDs.
- Stale-compose warnings in `./docker_build.sh create` now include both interactive (`docker compose exec ...`) and non-interactive (`docker compose exec -T ... -lc '<command>'`) command forms.
- After interactive standalone `create`/`enter` sessions end, the script prints a reconnect hint and preserves the shell exit code.
- `./docker_build.sh create` reuses an existing standalone `mako-dev` container instead of failing on name conflicts.
- If legacy standalone name `mako-dev` is already bound to a different checkout path, `docker_build.sh` automatically scopes standalone container naming to `mako-dev-<checkout-hash>` for the current checkout, preventing cross-checkout clobbering.
  For legacy same-checkout containers (for example wrong working directory/command/init), it keeps using `mako-dev` and normalizes that container in place.
- When auto-scoping is active and `mako-dev` belongs to a different checkout, non-interactive `shell` output now explicitly explains that legacy/default container is intentionally ignored and shows the scoped standalone name for the current checkout.
- You can explicitly pin standalone naming with `MAKO_DEV_CONTAINER_NAME=<name> ./docker_build.sh <action>`.
- `./docker_build.sh create` and `./docker_build.sh enter` auto-upgrade legacy standalone `mako-dev` containers created without Docker init support, recreating them with `--init` so child processes are reaped correctly.
- `./docker_build.sh create` and `./docker_build.sh enter` auto-recover standalone containers that are unsuitable for persistent dev usage (transient behavior or non-keepalive command setups), recreating them with a persistent keepalive command before suggesting `docker exec`.
- `./docker_build.sh create` and `./docker_build.sh enter` also auto-recreate standalone containers that point at a different `/workspace` bind mount or working directory, so reused `mako-dev` containers always match the current checkout.
- `./docker_build.sh create` and `./docker_build.sh enter` auto-recreate standalone `mako-dev` containers that use a different image, ensuring the dev container always runs on `mako-build:ubuntu24`.
- `docker_build.sh` resolves its own directory and bind mounts to canonical paths (`pwd -P`), and derives a per-checkout compose project id (`mako-<hash>`). Symlinked paths for the same checkout share one compose project, while different checkouts no longer collide on the same `dev` container.
- `docker-compose.yml` parameterizes project naming via `MAKO_COMPOSE_PROJECT` (default: `mako`); `docker_build.sh` sets this automatically for its compose operations.
- `./docker_build.sh compose-up`/`compose-down`/`enter` and non-interactive `shell` guidance warn for ambiguous/conflicting stale compose states (for example multiple non-current projects, or non-current projects while current compose `dev` is also running), print stale project IDs, and include `MAKO_COMPOSE_PROJECT=... docker compose down` cleanup commands.
- Non-interactive `./docker_build.sh shell` now also prints compose access commands plus project-scoped teardown commands (`... docker compose down`) for matching compose sessions (current project, single stale project, or explicit selection guidance for multiple projects), even when standalone is missing or already running.
- For multiple stale compose projects in non-interactive `shell`, teardown guidance now includes explicit per-project cleanup commands (`MAKO_COMPOSE_PROJECT=... docker compose down`) instead of placeholder `<project>` examples.
- For multiple stale compose projects, `shell`/`create`/`enter`/`compose-up` now print explicit per-project compose access commands (`MAKO_COMPOSE_PROJECT=... docker compose exec ...`) instead of placeholder `<project>` examples.
- `compose-up` warnings for stopped standalone containers now avoid placeholder `<project>` hints and instead point to project-scoped commands shown in the same output.
- If stale compose project IDs cannot be enumerated transiently, multi-project guidance now falls back to concrete inspection/retry commands (`docker ps ... | grep -- '-dev-1'` and rerun of `shell`/`create`/`enter`/`compose-up`) instead of placeholder `<project>` examples.
- For multiple stale compose projects in non-interactive `shell`, guidance now points back to `./docker_build.sh enter` after cleanup, so follow-up commands stay in the shell/enter workflow instead of switching to `compose-up`.
- When standalone is absent and the current compose project is not running, `./docker_build.sh enter` reuses a single matching stale compose `dev` service for the same checkout (if exactly one exists) instead of starting a duplicate compose container under a new project ID.
- If multiple stale compose projects are running for the same checkout, `./docker_build.sh enter` refuses to start another duplicate (including the case where standalone exists but is stopped) and asks you to choose one explicitly (via `MAKO_COMPOSE_PROJECT=...`) with both interactive and non-interactive command forms, or stop stale projects first and rerun `./docker_build.sh enter`.
- `./docker_build.sh compose-up` now follows the same duplicate-avoidance policy: it reuses a single stale compose `dev` for this checkout when possible, and refuses to start if multiple stale compose projects are running.
- When `compose-up` refuses due to multiple stale compose projects, it now prints explicit per-project cleanup commands (`MAKO_COMPOSE_PROJECT=... docker compose down`) in addition to project-selection guidance.
- When `compose-up` reuses a stale compose project, it prints the exact teardown command (`MAKO_COMPOSE_PROJECT=... docker compose down`), and `./docker_build.sh compose-down` now reuses that single stale project automatically when the current project has no running `dev` service.
- Successful `./docker_build.sh compose-up` runs now also print the current project teardown command (`MAKO_COMPOSE_PROJECT=... docker compose down`) for both interactive and non-interactive sessions.
- When `enter` uses compose service `dev` (current or stale project, for example standalone is missing/stopped), it prints project-scoped teardown commands so users can stop the compose session without guessing project IDs.
- `./docker_build.sh compose-down` distinguishes full vs partial shutdown: it stops current-project `dev` when running, auto-stops a single stale same-checkout `dev` when current is absent, and exits non-zero when stale compose services still remain (for example multiple stale projects).
- When `./docker_build.sh compose-down` exits non-zero because stale compose services still remain, it now prints explicit project-scoped cleanup commands (for example `MAKO_COMPOSE_PROJECT=... docker compose down`) even if only one stale project remains.
- `./docker_build.sh enter` now also requires standalone liveness to remain stable before non-interactive `docker exec` guidance, so short-lived containers are redirected to recovery-safe instructions (or compose guidance when compose `dev` is running).
- `./docker_build.sh enter` falls back to compose `dev` when standalone `mako-dev` does not exist, and auto-starts/bootstrap the compose service if needed. In non-interactive mode it prints guidance, including the `MAKO_COMPOSE_PROJECT=... docker compose exec -T --user "$(id -u):$(id -g)" dev /bin/bash -lc '<command>'` pattern.
- When `enter` auto-starts compose `dev` (because standalone is absent), it also prints the matching project-scoped teardown command (`MAKO_COMPOSE_PROJECT=... docker compose down`).
- Interactive compose shells entered via `./docker_build.sh enter` now print the same `BUILD_DIR=build_docker` tip and a reconnect hint, while preserving the shell exit code.
- When both standalone `mako-dev` and compose `dev` exist:
  - if standalone is running, `./docker_build.sh enter` prefers standalone `mako-dev`;
  - if standalone is stopped but compose `dev` is running (current project or a single same-checkout stale project), `./docker_build.sh enter` reuses compose `dev` instead of starting standalone.
- For non-interactive usage against the standalone dev container, run:
  `docker exec --user "$(id -u):$(id -g)" -e BUILD_DIR=build_docker "${MAKO_DEV_CONTAINER_NAME:-mako-dev}" /bin/bash -lc '<command>'`.
  If `docker_build.sh` auto-scoped the container name (for example `mako-dev-<checkout-hash>`), use the printed standalone name from `./docker_build.sh shell|create|enter` instead of bare `mako-dev`.
- `docker compose` services also export `BUILD_DIR=build_docker` for the same reason.
- For compose-based sessions, either use `./docker_build.sh enter` (recommended) or prefix raw compose commands with the checkout-specific project id:
  `MAKO_COMPOSE_PROJECT="mako-$(printf '%s' "$(pwd -P)" | sha256sum | cut -c1-10)" docker compose exec --user "$(id -u):$(id -g)" dev /bin/bash`
- For non-interactive compose usage (for example CI/headless), use:
  `MAKO_COMPOSE_PROJECT="mako-$(printf '%s' "$(pwd -P)" | sha256sum | cut -c1-10)" docker compose exec -T --user "$(id -u):$(id -g)" dev /bin/bash -lc '<command>'`.
- `./docker_build.sh ci <test> <jobs>` accepts an optional jobs argument.
- Example: `./docker_build.sh ci all 8` to run CI with 8 build jobs.
- Jobs-only shorthand is supported: `./docker_build.sh ci 8` is equivalent to `./docker_build.sh ci all 8`.
- `./docker_build.sh ci-quick <test>` skips rebuild and validates suite binaries up front (exists, executable, Docker-compatible RUNPATH); use `./docker_build.sh ci <test>` when `ci-quick` reports a binary issue.
- `rrrTests` is not supported in `ci-quick` because it runs broad CTest suites; use `./docker_build.sh ci rrrTests`.
- `cpuThrottlingScaling` is supported via `./docker_build.sh ci` and `./docker_build.sh ci-quick`; it is longer-running than most tests because each CPU cap run waits for full benchmark completion to emit throughput summaries.
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
