# Redis Compatibility Fixtures

These fixtures make the Phase 12 harnesses runnable without pretending that a
single local `makoCon` process proves distributed claims.

## G2 Cross-Shard Atomicity

`tools/redis_compat/run_bank_transfer.py` is the workload. A true G2 run needs a
multi-shard Mako Redis endpoint. The current default `examples/makoCon.cc`
validation path is one shard, so the acceptance runner reports `N/A`.

Local smoke only:

```bash
MAKO_G2_ALLOW_SINGLE_SHARD=1 python3 tools/redis_compat/run_bank_transfer.py
```

Redis Cluster comparison fixture:

```bash
bash tools/redis_compat/fixtures/redis_cluster.sh start
bash tools/redis_compat/fixtures/redis_cluster.sh stop
```

## G3 Failover Durability

`tools/redis_compat/run_failover_durability.py` needs command hooks for the
replicated Mako deployment:

```bash
MAKO_G3_START_CMD='...' \
MAKO_G3_KILL_CMD='...' \
MAKO_G3_RECOVER_CMD='...' \
python3 tools/redis_compat/run_failover_durability.py
```

The default Redis binary in this PR does not enable replication, so the harness
is `N/A` until a real replicated fixture is supplied.

## G4 Serializable Isolation

True G4 needs Elle:

```bash
ELLE_JAR=/path/to/elle.jar MAKO_G4_HISTORY=/tmp/history.edn \
python3 tools/redis_compat/run_elle_isolation.py
```

For a local read-modify-write smoke test only:

```bash
MAKO_G4_ALLOW_BUILTIN=1 python3 tools/redis_compat/run_elle_isolation.py
```

The built-in smoke checks final counters. It is not a substitute for Elle's
cycle/anomaly analysis.

## TCL Semantic Guard

Use an existing Redis checkout:

```bash
REDIS_TESTS_SOURCE=/path/to/redis/tests REDIS_COMPAT_BOOTSTRAP_TCL=1 \
bash tools/redis_compat/run_tcl_suite.sh
```

Or fetch a tagged Redis tests directory:

```bash
REDIS_COMPAT_FETCH_REDIS_TESTS=1 REDIS_COMPAT_BOOTSTRAP_TCL=1 \
bash tools/redis_compat/run_tcl_suite.sh
```

## Restart Durability

The restart guard needs stop/start hooks:

```bash
MAKO_RESTART_STOP_CMD='...' MAKO_RESTART_START_CMD='...' \
python3 tools/redis_compat/run_restart_durability.py
```

Local process fixture:

```bash
bash tools/redis_compat/fixtures/makocon_local.sh start
MAKO_RESTART_USE_LOCAL_FIXTURE=1 python3 tools/redis_compat/run_restart_durability.py
bash tools/redis_compat/fixtures/makocon_local.sh stop
```

The current Redis path is backed by in-memory Masstree, so this is expected to
fail until the Redis layer is wired to the persistent Mako path.

## Client Failover

The client failover guard needs one or more targets:

```bash
MAKO_FAILOVER_TARGETS=127.0.0.1:6380,127.0.0.1:6381 \
python3 tools/redis_compat/run_client_failover.py
```

With one local target, it is only a reconnect smoke test.
