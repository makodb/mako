# Redis Compatibility Fixtures

These fixtures make the Phase 12 harnesses runnable without pretending that a
single local `makoCon` process proves distributed claims.

## G2 Cross-Shard Atomicity

`third-party/redis/compat/run_bank_transfer.py` is the workload. A true G2 run needs a
multi-shard Mako Redis endpoint. The current default `third-party/redis/cpp/makoCon.cc`
validation path is one shard, so the acceptance runner reports `N/A` unless a
multi-shard fixture is selected.

Local smoke only:

```bash
MAKO_G2_ALLOW_SINGLE_SHARD=1 python3 third-party/redis/compat/run_bank_transfer.py
```

Local multi-shard Redis-facing fixture:

```bash
MAKO_G2_USE_LOCAL_FIXTURE=1 python3 third-party/redis/compat/run_bank_transfer.py
```

This starts one `makoCon` process with `MAKO_NUM_SHARDS=3` and
`MAKO_LOCAL_SHARDS=0,1,2`, so the Redis endpoint routes keys across multiple
local Mako shard tables in one process. By default the workload follows the
plan-level G2 shape: 3 independent runs, 100 accounts, 16 clients, 30 seconds
per run, and exact total-balance preservation after each run. It validates the
Redis transaction surface over Mako's sharded table routing. It is not a
multi-process backing-service or failover fixture.

Redis Cluster comparison fixture:

```bash
bash third-party/redis/compat/fixtures/redis_cluster.sh start
bash third-party/redis/compat/fixtures/redis_cluster.sh stop
```

Side-by-side Phase 12 capture:

```bash
G2_DEMO_START_REDIS_CLUSTER=1 python3 third-party/redis/compat/run_cross_shard_demo.py
```

This records Redis Cluster's cross-slot transaction rejection and the Mako bank
invariant in `docs/cross_shard_atomicity_demo.md`.

The separate `dbtest`/`bash/shard.sh` replicated scripts are benchmark/live
deployment runners, not Redis-serving `makoCon` backing services. They do not
expose a RESP endpoint that `run_bank_transfer.py` can target, so they are not
used as the G2 Redis compatibility fixture in this PR.

## G3 Failover Durability

`third-party/redis/compat/run_failover_durability.py` needs command hooks for the
replicated Mako deployment:

```bash
MAKO_G3_START_CMD='...' \
MAKO_G3_KILL_CMD='...' \
MAKO_G3_RECOVER_CMD='...' \
python3 third-party/redis/compat/run_failover_durability.py
```

The default Redis binary in this PR does not enable replication, so the harness
is `N/A` until a real replicated fixture is supplied.

Local restart smoke hook:

```bash
MAKO_G3_USE_LOCAL_RESTART_FIXTURE=1 \
MAKO_G3_ALLOW_RESTART_SMOKE=1 \
python3 third-party/redis/compat/run_failover_durability.py
```

This starts one local `makoCon`, kills it with `SIGKILL`, restarts it, and runs
the acknowledged-write oracle. It is useful for exercising the hook contract,
but it is not the replicated failover durability claim. A real G3 PASS still
requires a replicated Redis-facing Mako deployment and hooks that kill/recover
the current leader while clients reconnect to a live service.

## G4 Serializable Isolation

Default G4 runs a self-contained read-modify-write workload:

```bash
python3 third-party/redis/compat/run_elle_isolation.py
```

The workload uses 16 clients and 10 shared keys by default. Each transaction
queues `GET key` and `INCRBY key 1` inside one `MULTI`/`EXEC`; the checker
verifies each key's committed increments are contiguous and the final values
match the committed write count. This catches lost updates and non-serializable
read-modify-write behavior for the G4 workload.

Optional external Elle analysis remains available for externally generated
histories:

```bash
ELLE_JAR=/path/to/elle.jar MAKO_G4_HISTORY=/tmp/history.edn \
python3 third-party/redis/compat/run_elle_isolation.py
```

## TCL Semantic Guard

Use an existing Redis checkout:

```bash
REDIS_TESTS_SOURCE=/path/to/redis/tests REDIS_COMPAT_BOOTSTRAP_TCL=1 \
bash third-party/redis/compat/run_tcl_suite.sh
```

Or fetch a tagged Redis tests directory:

```bash
REDIS_COMPAT_FETCH_REDIS_TESTS=1 REDIS_COMPAT_BOOTSTRAP_TCL=1 \
bash third-party/redis/compat/run_tcl_suite.sh
```

## YCSB Benchmark

`third-party/redis/compat/run_ycsb_benchmark.sh` runs the standard YCSB Redis
binding against Mako and, by default, a local Redis reference server. It is not
part of the default acceptance runner because it requires a separate YCSB
distribution and workload sizing should be selected for the benchmark host.

```bash
YCSB_HOME=/tmp/ycsb-0.17.0 \
MAKO_PORT=6380 REDIS_PORT=6379 \
YCSB_RECORDCOUNT=10000 YCSB_OPERATIONCOUNT=10000 YCSB_THREADS=16 \
bash third-party/redis/compat/run_ycsb_benchmark.sh
```

Older YCSB release launchers are Python 2 scripts; the runner uses `python2`
automatically when it is present, or accepts an explicit `YCSB_PYTHON=/path`.

The default workload set is `workloada workloadb workloadc workloadf`. Override
`YCSB_WORKLOADS` only when a narrower mix is being investigated. Results are
written as timestamped CSV and per-phase logs under
`third-party/redis/compat/benchmark_logs/`.

YCSB load phases exercise Redis hash inserts and are currently a diagnostic for
the Mako Redis hash-write path, not a Redis parity claim. Report load and run
phases separately when using this runner for PR evidence.

## Restart Durability

The restart guard needs stop/start hooks:

```bash
MAKO_RESTART_STOP_CMD='...' MAKO_RESTART_START_CMD='...' \
python3 third-party/redis/compat/run_restart_durability.py
```

Local process fixture:

```bash
bash third-party/redis/compat/fixtures/makocon_local.sh start
MAKO_RESTART_USE_LOCAL_FIXTURE=1 python3 third-party/redis/compat/run_restart_durability.py
bash third-party/redis/compat/fixtures/makocon_local.sh stop
```

The current Redis path is backed by in-memory Masstree, so this is expected to
fail until the Redis layer is wired to the persistent Mako path.

## Client Failover

The client failover guard needs one or more targets:

```bash
MAKO_FAILOVER_TARGETS=127.0.0.1:6380,127.0.0.1:6381 \
python3 third-party/redis/compat/run_client_failover.py
```

With one local target, it is only a reconnect smoke test.
