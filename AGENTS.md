# AGENTS

This file records what exists in the Redis compatibility work and where each piece lives.

## Plan

- Source plan: `/home/users/ssoumojit/.claude/plans/read-the-plan-for-magical-muffin.md`
- Rendered plan: `/home/users/ssoumojit/.claude/plans/read-the-plan-for-magical-muffin.html`
- Current working step: make the PR #72 Redis compatibility layer release-clean.
  Defer all vector-specific implementation until this compatibility work is
  complete.
- Plan correction made during Step 0.1: `redis-rs` is a filtered client subset, not the full upstream `redis-rs` suite.

## Phase Completion Checklist

- After each implementation phase, run the relevant correctness tests.
- After correctness passes, run Redis benchmarks for the changed command surface.
- Include at least one `redis-benchmark` smoke run and one `memtier_benchmark` run when the Redis server can be started.
- If a benchmark cannot run, record the concrete blocker instead of leaving it implicit.

## Repository State For This Work

- Active working and PR push repo: `/home/users/ssoumojit/mako-fork-pr`.
- `/home/users/ssoumojit/mako` is a sibling checkout containing historical
  guidance and artifacts. Do not implement this work there unless the user
  explicitly changes the active checkout.
- GitHub PR pushes must be done from `/home/users/ssoumojit/mako-fork-pr` with SoumojitDalui auth only.
- Always disable the wshen24 global Git config when pushing:
  `GIT_CONFIG_GLOBAL=/dev/null HOME=/home/users/ssoumojit GH_CONFIG_DIR=/home/users/ssoumojit/.config/gh git push ...`
- Never push this PR with the default `/home/users/wshen24/.gitconfig`; it rewrites GitHub URLs through the wrong token owner.
- Active branch: `redis-compat-phase3`.
- Base commit before the current compatibility hardening:
  `c28e39f2affee7c74ecb9747342c0811dd560053`.
- `origin`: `https://github.com/SoumojitDalui/makodb.git`.
- `upstream`: `https://github.com/makodb/mako.git`.
- Safe-directory entries were added for the repo and submodules because Git initially rejected the worktree as dubious ownership.

## Redis Layer Ownership

- Redis-owned code now lives under `third-party/redis/` in the PR working tree.
- Rust RESP and command parsing: `third-party/redis/rust-lib/`.
- C++ Redis executors and FFI helpers: `third-party/redis/cpp/` and `third-party/redis/include/`.
- Redis compatibility harnesses, fixtures, client runners, acceptance artifacts, and probes: `third-party/redis/compat/`.
- Vendored Redis TCL tests: `third-party/redis/redis-tests/`.
- The old `third-party/makocon` submodule is removed from the PR tree. Do not push Redis changes through `Jiyang-Wu/MakoCon`.
- Do not add Redis-specific APIs or shortcuts to core Mako storage files such as `MassTrans.hh`. Redis commands should call generic Mako DB/table APIs from the adapter layer unless a core feature is independently useful outside Redis.

## Compatibility Evidence

- Human-readable test inventory, latest dated findings, limitations, CPU tables,
  and reproduction commands:
  `third-party/redis/compat/README.md`.
- Generated ecosystem-client scoreboard:
  `third-party/redis/compat/client_test_results.csv`.
- Reproducible request-worker CPU sampler:
  `third-party/redis/compat/run_worker_cpu_benchmark.py`.
- The worker sampler writes `third-party/redis/compat/worker_cpu_results.csv`,
  which is a generated, Git-ignored artifact.
- Cross-worker blocking wakeups, blocked-client fairness queues, and multi-key
  retry eligibility are implemented and unit-tested in
  `third-party/redis/rust-lib/src/lib.rs`.
- Destructive correctness runners must be serialized. In particular,
  `probe_commands.py`, the Tcl suite, and other cleanup paths can issue
  `FLUSHDB` or `FLUSHALL` and must not overlap G2/G4 histories or benchmarks.

## Step 0.1 Harness

- `requirements-test.txt`
  - Python test dependencies for the compatibility harness.
  - Current baseline: `redis`, `pytest`, `pytest-xdist`.

- `third-party/redis/compat/conftest.py`
  - Shared pytest fixtures.
  - Provides:
    - `redis_client`: reference Redis on `127.0.0.1:6379`.
    - `mako_client`: makoCon on `127.0.0.1:6380`.
    - `both_clients`: parametrized fixture over Redis and makoCon.
  - Ports and hosts can be overridden with `REDIS_HOST`, `REDIS_PORT`, `MAKO_HOST`, `MAKO_PORT`.

- `third-party/redis/compat/test_ping.py`
  - Minimal smoke test.
  - Verifies both Redis and makoCon respond to `PING`.

- `third-party/redis/compat/run_client_tests.sh`
  - G1 wire-compatibility runner scaffold.
  - Default mode is fast and emits `third-party/redis/compat/client_test_results.csv`.
  - Full client execution is opt-in:
    ```bash
    REDIS_COMPAT_FULL=1 bash third-party/redis/compat/run_client_tests.sh
    ```
  - Tool rows:
    - `pytest-ping`
    - `redis-cli`
    - `fakeredis-py`
    - filtered `redis-rs`
    - `jedis`
    - `node-redis`
    - `ioredis`

- `third-party/redis/compat/fakeredis_filter.txt`
  - Seed filter for in-scope fakeredis-py tests.
  - Later phases should replace broad command-family filters with exact test names.

- `third-party/redis/compat/.gitignore`
  - Keeps generated logs, CSVs, upstream checkouts, and Python caches out of Git.

## Step 0.2 Tcl Semantic Suite

- `third-party/redis/redis-tests/tests/`
  - Vendored Redis `7.4.0` Tcl test suite support and test files.
  - Only the Redis `tests/` tree is present; this is not a full Redis source checkout.

- `third-party/redis/compat/run_tcl_suite.sh`
  - Runs the scoped Redis Tcl semantic files against reference Redis and makoCon.
  - Default in-scope files:
    - `unit/type/string`
    - `unit/type/hash`
    - `unit/type/list`
    - `unit/type/set`
    - `unit/type/zset`
    - `unit/expire`
    - `unit/scan`
    - `unit/multi`
    - `unit/keyspace`
    - `unit/networking`
    - `unit/pubsub`
  - Emits:
    - `third-party/redis/compat/tcl_results_redis.csv`
    - `third-party/redis/compat/tcl_results_mako.csv`
  - Logs per target/file under `third-party/redis/compat/tcl_logs/`.
  - Default Tcl flags are `--singledb --ignore-encoding --ignore-digest`, so the run targets external command semantics instead of Redis logical-DB, object-encoding, or digest internals.
  - `REDIS_COMPAT_START_REDIS=1` starts a temporary reference Redis on port `7399` with `enable-debug-command yes`; use this for a clean Redis baseline instead of relying on whatever is already running on `6379`.
  - `TCL_COMPAT_FILES="unit/type/string"` can restrict the run to selected files.

- `third-party/redis/compat/tcl_scope.txt`
  - Lists the 11 in-scope Tcl files.
  - Lists whole-file exclusions and one-line reasons for Redis-internal or deliberately out-of-scope subsystems.

- `third-party/redis/compat/tcl_known_skips.txt`
  - Reviewed per-test skips inside otherwise in-scope files.
  - Current skip categories:
    - Redis Streams and blocking stream timeout behavior, which are outside the current Mako Redis surface,
    - Redis keyspace notification Pub/Sub, which is separate from normal Pub/Sub delivery and is not claimed,
    - Redis AOF/server-process internals that are not external command semantics.
  - `run_tcl_suite.sh` converts this file into per-file `--skipfile` inputs for Redis's Tcl helper.

## Steps 0.3-0.5 Command Probe, Claim Harnesses, And Acceptance

- `third-party/redis/compat/command_tiers.json`
  - Part A command surface classified by P0/P1/P2.
  - P0 currently has 31 commands as required by the plan.

- `third-party/redis/compat/probe_commands.py`
  - Raw RESP command probe against Redis and makoCon.
  - Emits JSON with per-command classifications and tier summaries.
  - Includes Mako-specific checks for `\x01` key-prefix rejection and `INFO mako` fields.

- Claim harnesses:
  - `third-party/redis/compat/run_bank_transfer.py` for G2 cross-shard atomicity.
  - `third-party/redis/compat/run_failover_durability.py` for G3 failover durability.
  - `third-party/redis/compat/run_elle_isolation.py` for G4 serializable isolation.
  - G2 reports `N/A` unless `MAKO_G2_MULTI_SHARD=1` is set by a real
    multi-shard fixture. `MAKO_G2_USE_LOCAL_FIXTURE=1` starts the checked-in
    local multi-shard Redis fixture; `MAKO_G2_ALLOW_SINGLE_SHARD=1` is smoke only.
  - G3 reports `N/A` unless replicated-topology command hooks are provided:
    `MAKO_G3_START_CMD`, `MAKO_G3_KILL_CMD`, `MAKO_G3_RECOVER_CMD`.
  - G4 runs the checked-in Redis-facing RMW serializability oracle by default.
    External Elle remains optional when `ELLE_JAR` and `MAKO_G4_HISTORY` are provided.

- Operational guards:
  - `third-party/redis/compat/run_throughput.sh`
  - `third-party/redis/compat/run_memtier.sh`
  - `third-party/redis/compat/run_fuzz.sh`
  - `third-party/redis/compat/run_soak.sh`
  - `third-party/redis/compat/run_restart_durability.py`
  - `third-party/redis/compat/run_client_failover.py`

- Fixture helpers:
  - `third-party/redis/compat/fixtures/makocon_local.sh`
    - Starts/stops one local `makoCon` for smoke and restart-hook testing.
  - `third-party/redis/compat/fixtures/redis_cluster.sh`
    - Starts/stops a local Redis Cluster comparison fixture.
  - `third-party/redis/compat/bootstrap_redis_tests.sh`
    - Links or fetches Redis TCL tests only when explicitly requested.
  - `third-party/redis/compat/fixtures/README.md`
    - Lists required env vars for G2, G3, G4, TCL, restart, and client failover.

- `third-party/redis/compat/run_acceptance.sh`
  - Runs the four claim checks and eight operational guards.
  - Prints exactly twelve result lines.
  - `ACCEPTANCE_QUICK=1` uses reduced durations and a one-file Tcl run for smoke verification.

- `third-party/redis/compat/known_divergences.txt`
  - Empty initial divergence file.
  - Do not add entries without review.

## Phase 1 FFI And Encoding Work

- `third-party/redis/compat/test_del_exists.py`
  - Focused TDD coverage for `EXISTS`, `DEL`, `UNLINK`, variadic `EXISTS`, and transaction behavior.

- `third-party/redis/compat/test_encoding_roundtrip.py`
  - Focused TDD coverage for byte-for-byte `SET`/`GET` round trips, including empty values, `\x00`, high bytes, and deterministic random payloads.

- `third-party/redis/include/transaction_ffi.h`
  - Adds `TXN_OP_EXISTS`.
  - Keeps `TXN_OP_DEL` as an alias of `TXN_OP_DELETE`.
  - Adds `TxnOpResult::value_present` so an empty Redis bulk string is distinguishable from a missing key.

- Core Mako storage files
  - The Redis-specific `Exists`/`exists()`/`transExists()` storage hook was removed.
  - `EXISTS` and delete-style Redis commands now stay in the Redis adapter layer and use the existing generic table `Get`/`remove` path where presence is needed.
  - This keeps `MassTrans.hh`, `abstract_ordered_index.h`, `mbta_wrapper.hh`, and `mbta_sharded_ordered_index.hh` free of Redis-only API surface.

- `third-party/redis/cpp/makoCon.cc`
  - Implements correct `GET` hit/miss signaling for empty values.
  - Implements `DELETE` and `EXISTS` FFI handling with Redis-style existence result bits.
  - Set mutations now use a transaction-local set overlay for staged write cases such as `SADD` followed by `SMOVE` inside one `EXEC`; dirty sets flush once before commit.

## Phase 5 Scan / Key Enumeration

- `third-party/redis/rust-lib/src/lib.rs`
  - Parses `KEYS`, `SCAN`, `DBSIZE`, `TYPE`, `WAIT`, and `HSCAN`.
  - Formats Redis-compatible numeric scan cursors.
  - Performs glob filtering in Rust.
  - Returns `TYPE` as `string` or `none` for the current string-only surface.
  - Returns `WAIT 0 0` as `0` for the no-replication compatibility shim.
  - Returns a clear `HSCAN` error until hash storage exists.

- `third-party/redis/include/transaction_ffi.h`
  - Adds `TXN_OP_SCAN`.
  - Uses existing FFI fields for cursor, prefix, count, and `DBSIZE` count-only mode.

- `third-party/redis/cpp/makoCon.cc`
  - Executes scan inside the existing transaction path.
  - Scans user-visible `table_key_` keys only.
  - Skips expired keys and internal TTL metadata.
  - Standalone large-keyspace `FLUSHDB`/`FLUSHALL` use chunked scan/delete/commit batches so benchmark-populated keyspaces do not run as one oversized Mako transaction.
  - Standalone `DBSIZE` uses chunked `table_key_` scan/count batches, with lazy expired-key cleanup, so post-benchmark key counts do not run as one oversized Mako transaction.

- `third-party/redis/compat/test_scan_keys_dbsize.py`
  - Covers `KEYS`, cursor `SCAN`, expired-key visibility, `SCAN TYPE`, `DBSIZE`, and the documented `HSCAN` gap.
  - Covers large-keyspace `FLUSHALL` liveness with `test_flushall_large_keyspace_keeps_server_alive`.

- `third-party/redis/compat/test_lists_basic.py`
  - Covers `LPOS` option behavior for `RANK`, `COUNT`, and `MAXLEN`.
  - `LPOS ... COUNT` must return an array of integers, not bulk-string positions.

- `third-party/redis/compat/test_sets_basic.py`
  - Covers `SADD` followed by `SMOVE` of the same member inside one `EXEC`.
  - Covers `SPOP`/`SRANDMEMBER` membership/count semantics; the adapter now uses PRNG sampling rather than deterministic scan-order rotation.

- `third-party/redis/compat/test_type_wait.py`
  - Covers `TYPE` for missing, live, and expired string keys.
  - Covers `WAIT 0 0` standalone and inside `MULTI`.

- `docs/redis_interface.md`
  - Documents local single-shard scan scope and the hash-scan deferral.

## Phase 11 Code Health

- `third-party/redis/cpp/makocon_ffi_impl.hh`
  - Shared C/Rust FFI helper header for both Redis binaries.
  - Pins `TxnOperation`, `TxnRequest`, `TxnOpResult`, and `TxnResponse` layout with compile-time assertions.
  - Provides shared response allocation/freeing, metrics population, and retry accounting helpers.

- `third-party/redis/cpp/makoCon.cc`
  - Remains the Redis semantic target.
  - Uses the shared FFI helpers but keeps the canonical transaction executor.

- `third-party/redis/cpp/makoConMultiTrd.cc`
  - Builds against the shared FFI helpers.
  - Remains ABI-compatible only for extended Redis commands; it does not claim full Redis semantic parity.

## Phase 12 Acceptance

- `third-party/redis/compat/run_acceptance.sh`
  - Prints the twelve planned Phase 12 acceptance lines.
  - Saves artifacts under `third-party/redis/compat/acceptance/`.
  - Current run: 6 PASS, 6 N/A.
  - Exit code `78` from a child harness means the harness exists but required
    environment/fixture setup is absent.

- Harness entrypoints now present:
  - `third-party/redis/compat/run_bank_transfer.py`
  - `third-party/redis/compat/run_failover_durability.py`
  - `third-party/redis/compat/run_elle_isolation.py`
  - `third-party/redis/compat/run_tcl_suite.sh`
  - `third-party/redis/compat/run_fuzz.sh`
  - `third-party/redis/compat/run_soak.sh`
  - `third-party/redis/compat/run_restart_durability.py`
  - `third-party/redis/compat/run_client_failover.py`

- `API_COMPAT_REPORT_REDIS_v2.md`
  - Current supported command surface, scoped metrics, divergences, and acceptance status.

- `docs/cross_shard_atomicity_demo.md`
  - Documents why the G2 demo is not captured yet.
  - Missing pieces: Redis Cluster fixture and multi-shard Mako Redis fixture.

## Phase 6 Sets

- `third-party/redis/rust-lib/src/lib.rs`
  - Parses `SADD`, `SMEMBERS`, `SISMEMBER`, `SREM`, `SCARD`, `SMOVE`, `SPOP`, `SRANDMEMBER`, `SINTER`, `SUNION`, `SDIFF`, and the `*STORE` variants.
  - Packs member/source lists as length-prefixed bytes for FFI.
  - Returns Redis integer, bulk, and array replies for set commands.

## Phase 9 Pub/Sub

- `third-party/redis/rust-lib/src/lib.rs`
  - Adds `SUBSCRIBE`, `UNSUBSCRIBE`, `PSUBSCRIBE`, `PUNSUBSCRIBE`, `PUBLISH`, and `PUBSUB`.
  - Keeps Pub/Sub in Rust handler memory only; no C++ storage or FFI changes.
  - Tracks subscriber-mode state per connection.
  - Uses a global channel/pattern registry with weak queue handles so closed clients do not keep queues alive.
  - Drains per-client outbound queues from the worker loop before normal socket reads.
  - Rejects storage commands while a connection is in subscriber mode.
  - Queues `PUBLISH` inside `MULTI` and delivers on `EXEC`.
  - Adds `INFO clients` and `INFO stats` counters: `pubsub_channels`, `pubsub_patterns`.
  - Treats connection reset/broken pipe as normal close to keep benchmark logs clean.

- `third-party/redis/compat/test_pubsub_basic.py`
  - Covers direct subscribe/publish/unsubscribe.
  - Covers pattern subscribe and matching publish.
  - Covers `PUBSUB` introspection and INFO counters.
  - Covers subscriber-mode storage-command rejection.
  - Covers `PUBLISH` inside `MULTI`.

- `docs/redis_interface.md`
  - Documents Pub/Sub as process-local, in-memory, non-persistent handler state.
  - States that Pub/Sub does not use Mako storage and does not replay messages after disconnect.

## Phase 10 Scan Family

- `third-party/redis/rust-lib/src/lib.rs`
  - Adds `SSCAN`.
  - Allows nonzero `ZSCAN` cursors.
  - Implements Rust-owned cursor pagination for `SSCAN` and `ZSCAN`.
  - Supports `MATCH` and `COUNT` for both member-scan commands.
  - Reuses existing C++ transactional member reads; no new storage FFI or scan interface is added.

- `third-party/redis/compat/test_scan_family.py`
  - Covers paginated `SSCAN` with `MATCH`.
  - Covers paginated `ZSCAN` with `MATCH`.
  - Keeps `HSCAN` documented as blocked until hash storage exists.

- `docs/redis_interface.md`
  - Documents `SSCAN`.
  - Removes the old cursor-0-only `ZSCAN` limitation.
  - Keeps full keyspace visibility for set/list/zset logical keys deferred until there is a shared logical key index.

- `third-party/redis/compat/known_divergences.txt`
  - Removes the stale `ZSCAN cursor` divergence.

- `third-party/redis/cpp/makoCon.cc`
  - Stores set members under internal `\x01S:<set>:<member>` keys.
  - Stores set cardinality under `\x01S#:<set>`.
  - Keeps set writes, cardinality updates, and `*STORE` updates inside one transaction.

## Phase 7 Lists

- `third-party/redis/rust-lib/src/lib.rs`
  - Parses non-blocking list commands:
    `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LLEN`, `LINDEX`, `LRANGE`,
    `LSET`, `LREM`, `LTRIM`, `LINSERT`, `LPUSHX`, `RPUSHX`, `LPOS`,
    `LMOVE`, and `RPOPLPUSH`.
  - Packs list arguments as length-prefixed bytes for FFI.

- `third-party/redis/cpp/makoCon.cc`
  - Stores list elements under internal `\x01L:<u64 list-len><list><ordered-index>` keys.
  - Stores list head/tail metadata under `\x01L#:<u64 list-len><list>`.
  - Uses a transaction-local list overlay and flushes dirty lists once before commit.
  - Clears staged lists on immediate expiry before commit.
  - Keeps Redis `LINSERT` return codes: missing list -> `0`, missing pivot in an existing list -> `-1`.

- `third-party/redis/compat/test_lists_basic.py`
  - Focused Phase 7 pytest coverage for basic list operations, mutation, moves, TTL/type behavior, wrong-type behavior, and list commands inside `MULTI`.
  - Handles set-aware `TYPE`, `EXISTS`, `DEL`, `EXPIRE`, `TTL`, and `PERSIST`.
  - Hides internal set keys from `KEYS`/`SCAN`/`DBSIZE`.

- `third-party/redis/include/transaction_ffi.h` and `third-party/redis/cpp/rust_wrapper.h`
  - Add Phase 6 set opcodes and flags.

- `third-party/redis/compat/test_sets_basic.py`
  - Covers basic set commands, random member count modes, move, algebra/store, type, TTL, and hidden internal keys.

- `third-party/redis/compat/test_tag_index.py`
  - Covers the tag-index workflow from the plan.

- `third-party/redis/compat/kvrocks_set_cases/`
  - Ports in-scope Apache Kvrocks set command cases into Mako's pytest harness.
  - Keeps skipped Kvrocks cases and reasons in `skipped_cases.md`.
  - Can optionally compare the same scenarios against reference Redis with `KVROCKS_CASES_COMPARE_REDIS=1`.

- `third-party/redis/rust-lib/src/lib.rs`
  - Parses `EXISTS` and `UNLINK`.
  - Supports variadic `DEL`/`UNLINK`/`EXISTS` by expanding to per-key FFI ops and aggregating the result count.
  - Uses `value_present` to return empty bulk strings correctly instead of nil.

- `third-party/redis/cpp/makoConMultiTrd.cc` and `third-party/redis/cpp/rust_wrapper.h`
  - Mirrored opcode/result-shape updates so the alternate makoCon binary remains aligned with the shared Rust FFI contract.

## Current Verification

Run:

```bash
python3 -m pytest third-party/redis/compat/test_ping.py -v
bash third-party/redis/compat/run_client_tests.sh
REDIS_COMPAT_START_REDIS=1 bash third-party/redis/compat/run_tcl_suite.sh
python3 third-party/redis/compat/probe_commands.py --json-out third-party/redis/compat/logs/probe_commands.json
ACCEPTANCE_QUICK=1 ACCEPTANCE_G2_DURATION=1 G2_RUNS=1 G2_CLIENTS=2 ACCEPTANCE_G4_DURATION=1 bash third-party/redis/compat/run_acceptance.sh
cargo check  # from third-party/redis/rust-lib
python3 -m pytest third-party/redis/compat/test_del_exists.py third-party/redis/compat/test_encoding_roundtrip.py -v
redis-benchmark -h 127.0.0.1 -p 6380 -t set,get -n 10000 -c 50 -d 128 -q
memtier_benchmark -s 127.0.0.1 -p 6380 --protocol=redis --threads=1 --clients=50 --requests=1000 --data-size=128 --ratio=1:1 --key-pattern=P:P --key-minimum=1 --key-maximum=50000 --hide-histogram
```

Expected now:

- `test_ping.py` reports two tests:
  - `test_ping[redis]`
  - `test_ping[mako]`
- `run_client_tests.sh` writes `third-party/redis/compat/client_test_results.csv`.

Observed result:

- `python3 -m pytest third-party/redis/compat/test_ping.py -v`
  - `2 passed`
- `bash third-party/redis/compat/run_client_tests.sh`
  - wrote `third-party/redis/compat/client_test_results.csv`
  - default rows pass for `pytest-ping` and `redis-cli`
  - heavier client rows are marked `not-run-set-REDIS_COMPAT_FULL=1`
- `TCL_COMPAT_FILES='unit/type/string' TCL_TEST_TIMEOUT_SECONDS=45 bash third-party/redis/compat/run_tcl_suite.sh`
  - Redis passed `unit/type/string` with 83 checks.
  - makoCon failed before string semantics because the Tcl harness calls `FLUSHALL` during cleanup and makoCon currently returns `ERR unsupported command`.
  - This confirms Step 0.2 is exposing a real prerequisite compatibility gap, not just a connection failure.
- `REDIS_COMPAT_START_REDIS=1 TCL_TEST_TIMEOUT_SECONDS=120 bash third-party/redis/compat/run_tcl_suite.sh`
  - Redis passed all 11 scoped Tcl files with reviewed skips applied.
  - makoCon passed only `unit/networking`; the other 10 files fail at setup because makoCon does not support the Tcl harness cleanup command (`FLUSHALL`) yet.
  - The temporary reference Redis on port `7399` was cleaned up after the run.
- `python3 third-party/redis/compat/probe_commands.py --json-out third-party/redis/compat/logs/probe_commands.json`
  - makoCon: P0 `4/31`, total command probes `6/97`.

## Phase 2 Connection Layer

- `third-party/redis/rust-lib/src/lib.rs`
  - Adds per-connection `ClientState`.
  - Parses and handles pure Rust connection commands:
    - `HELLO`
    - `CLIENT SETNAME`
    - `CLIENT GETNAME`
    - `CLIENT ID`
    - `CLIENT LIST`
    - `CLIENT NO-EVICT`
    - `CLIENT REPLY`
    - `CLIENT SETINFO`
    - `COMMAND`
    - `COMMAND DOCS`
    - `COMMAND INFO`
    - `COMMAND COUNT`
    - `RESET`
    - `QUIT`
    - `SELECT 0`
    - `AUTH`
    - `ECHO`
    - `INFO`
  - `QUIT` marks the connection for close after flushing `+OK`.
  - `RESET` clears transaction state and client metadata.
  - `INFO server` returns a Redis-style server section.
  - `INFO mako` returns Mako transaction counters.
  - `PING` queues inside `MULTI` and returns its reply from `EXEC`.
  - Keeps Phase 1 Redis storage commands in the same parser: `DEL`, `UNLINK`, `EXISTS`, variadic spans, and `value_present`.
  - Parser errors now distinguish:
    - unknown command,
    - wrong arity,
    - non-array protocol input.
  - Test-only C++ FFI stubs allow pure Rust unit tests to run without linking the full makoCon binary.

- Phase 2 tests live in the same Rust file under `#[cfg(test)]`.
  - They cover `HELLO 3`, client name round trip, documented `CLIENT` subcommands, `RESET`, `QUIT`, `SELECT 0`, `AUTH`, `ECHO`, parseable `COMMAND`, `INFO server`, `INFO mako`, queued `PING` in `MULTI`, empty bulk string vs nil, duplicate `EXISTS`, `UNLINK`, unknown command errors, wrong arity errors, and non-array protocol errors.

- `third-party/redis/include/transaction_ffi.h`
  - Adds `MakoMetrics`.
  - Declares `cpp_get_metrics(MakoMetrics*)`.

- `third-party/redis/cpp/makoCon.cc`
  - Adds relaxed atomic counters for transaction commits, aborts, and retries.
  - Exposes counters and uptime through `cpp_get_metrics`.
  - Atomics are annotated as a C/Rust FFI metrics boundary.
  - `mako_txn_retries` remains 0 until this Redis path has a retry loop.

- `third-party/redis/cpp/makoConMultiTrd.cc`
  - Mirrors the same metrics FFI for the alternate binary.
  - Atomics are annotated as a C/Rust FFI metrics boundary.

- Verification:
  - `cargo test`
  - `cargo check`
  - `cmake --build build_clang21 --target makoCon makoConMultiTrd` compiled Rust and C++ objects but failed at final link because the environment is missing `-llz4`.
  - `prefix_rejection` fails because `SET \x01evil v` currently returns `OK`.
  - `info_mako_fields` fails because `INFO mako` currently returns `ERR unsupported command`.
- `ACCEPTANCE_QUICK=1 ACCEPTANCE_G2_DURATION=1 G2_RUNS=1 G2_CLIENTS=2 ACCEPTANCE_G4_DURATION=1 bash third-party/redis/compat/run_acceptance.sh`
  - Produced twelve lines.
  - Current expected shape: G1 FAIL, G2 FAIL, G3 N/A, G4 N/A, throughput FAIL, memtier N/A, Tcl quick PASS, INFO mako FAIL, fuzz PASS, soak N/A, restart N/A, client failover N/A.
- Phase 1 verified implementation:
  - Built `build_clang21/makoCon`.
  - Built `build_clang21/makoConMultiTrd`.
  - `cargo check` in `third-party/redis/rust-lib`: passed.
  - Started rebuilt `makoCon` on `MAKO_PORT=6381 MAKO_PAXOS_PROC_NAME=p1` to avoid the old 6380 process.
  - `MAKO_PORT=6381 python3 -m pytest third-party/redis/compat -v`: 7 passed.
  - Manual smoke:
    - `EXISTS present missing` returned `1`.
    - `EXISTS present present present` returned `3`.
    - `DEL present` returned `1`.
    - `EXISTS present` returned `0`.
  - `MAKO_PORT=6381 bash third-party/redis/compat/run_client_tests.sh`: completed and wrote `third-party/redis/compat/client_test_results.csv`.
  - Temporary 6381 test server was stopped after verification.

## Known Local State

- The repository was updated to `origin/mako-dev`.
- The old `dependencies/yaml-cpp/` embedded repository remains untracked after the upstream move to `third-party/yaml-cpp`; it was not deleted.
- Phase 0 and Phase 1 files are currently untracked/modified and ready to review.
- Generated upstream client checkout/build caches under `third-party/redis/compat/_upstream` were removed after verification.

## Phase 8 Sorted Sets

- `third-party/redis/rust-lib/src/lib.rs`
  - Parses core sorted-set commands:
    `ZADD`, `ZSCORE`, `ZINCRBY`, `ZREM`, `ZCARD`, `ZRANGE`,
    `ZREVRANGE`, `ZRANGEBYSCORE`, `ZRANK`, `ZREVRANK`, `ZCOUNT`,
    `ZPOPMIN`, `ZPOPMAX`, and `ZSCAN`.
  - Packs score/member and range arguments as length-prefixed bytes for FFI.

- `third-party/redis/cpp/makoCon.cc`
  - Stores zset member scores under internal `\x01Z:<u64 zset-len><zset><member>` keys.
  - Stores score index records under `\x01ZS:<u64 zset-len><zset><encoded-score><member>` keys.
  - Stores cardinality metadata under `\x01Z#:<u64 zset-len><zset>`.
  - Uses a transaction-local zset overlay and flushes dirty zsets once before commit.
  - On score updates, deletes only the stale score-index key and overwrites the member key; do not delete and reinsert the same member key in one transaction.

- `third-party/redis/compat/test_zsets_basic.py`
  - Focused Phase 8 pytest coverage for add/update, score reads, ranges, ranks, pops, type/TTL behavior, wrong-type behavior, and zset commands inside `MULTI`.

## Latest Phase 12 G2/G3 Fixture Update

- `third-party/redis/cpp/makoCon.cc`
  - Adds environment-driven Redis fixture knobs:
    - `MAKO_NUM_SHARDS`
    - `MAKO_SHARD_INDEX`
    - `MAKO_LOCAL_SHARDS`
    - `MAKO_SHARD_CONFIG`
    - `MAKO_REPLICATION_ENABLED`
    - `MAKO_REPLICATION_CONFIG`
    - `MAKO_OCC_CONFIG`
  - Defaults are unchanged: one shard, shard index 0, no replication.
  - `MAKO_LOCAL_SHARDS=0,1,2` marks all local shard tables as local in one process.

- `third-party/redis/compat/fixtures/makocon_multishard.sh`
  - Starts one Redis-facing `makoCon` with `MAKO_NUM_SHARDS=3` and `MAKO_LOCAL_SHARDS=0,1,2`.
  - This is the current G2 local multi-shard fixture.

- `third-party/redis/compat/run_bank_transfer.py`
  - `MAKO_G2_USE_LOCAL_FIXTURE=1` starts/stops the local multi-shard fixture and sets `MAKO_G2_MULTI_SHARD=1`.
  - Reference provenance is now in the file: CockroachDB-style bank workload; reusable oracle is exact total-balance preservation.

- `third-party/redis/compat/fixtures/makocon_g3_local_restart.sh`
  - Provides `start`, `kill`, `recover`, `stop`, and `status` commands for exercising the G3 hook contract against one local `makoCon`.
  - This is restart smoke only, not replicated failover.

- `third-party/redis/compat/run_failover_durability.py`
  - `MAKO_G3_USE_LOCAL_RESTART_FIXTURE=1` wires the local restart hook.
  - It still returns `N/A` unless `MAKO_G3_ALLOW_RESTART_SMOKE=1` is set, so restart smoke is not accidentally counted as G3 failover durability.
  - Reference provenance is now in the file: Jepsen Redis-Raft style acknowledged-write survival oracle.

- Verification just run:
  - `cmake --build /tmp/mako-fork-pr-build --target makoCon -j2`: passed with the existing `libunwind` warning.
  - `MAKO_PORT=6385 MAKO_G2_USE_LOCAL_FIXTURE=1 python3 third-party/redis/compat/run_bank_transfer.py`: full plan-shaped local multi-shard G2 passed, `bank transfer invariant preserved runs=3 transfers=190615 retries=0`.
  - `MAKO_G3_USE_LOCAL_RESTART_FIXTURE=1 python3 third-party/redis/compat/run_failover_durability.py`: exits `78` with the expected smoke-only guard.
  - `MAKO_PORT=6392 MAKO_G3_USE_LOCAL_RESTART_FIXTURE=1 MAKO_G3_ALLOW_RESTART_SMOKE=1 MAKO_G3_WRITES=10 python3 third-party/redis/compat/run_failover_durability.py`: hook path ran and failed durability as expected for in-memory `makoCon` (`acked=10 missing=6`).
  - `python3 -m py_compile third-party/redis/compat/run_bank_transfer.py third-party/redis/compat/run_failover_durability.py`: passed.
  - `bash -n` on new fixture scripts: passed.
  - `git diff --check`: passed.

- Phase 12 remaining:
  - G2 local multi-shard fixture and side-by-side Redis Cluster vs Mako demo output are now captured.
  - True G3 still needs a replicated Redis-facing Mako deployment with leader kill/recover hooks. Do not treat local restart smoke as G3 PASS.

## Latest Phase 12 G2 Acceptance Update

- `third-party/redis/compat/run_cross_shard_demo.py`
  - Starts or uses a local Redis Cluster comparison fixture.
  - Confirms Redis Cluster rejects a cross-slot transactional transfer with
    `CrossSlotTransactionError`.
  - Runs the Mako local multi-shard bank-transfer fixture and records the
    invariant result in `docs/cross_shard_atomicity_demo.md`.

- `third-party/redis/compat/run_acceptance.sh`
  - Adds a `G2 cross-shard demo` row.
  - Treats soak and RESP fuzz as `N/A` when the shared `makoCon` endpoint is not
    reachable, matching the other endpoint-dependent guards.

- Latest full G2 local multi-shard validation:
  - `MAKO_PORT=6385 MAKO_G2_USE_LOCAL_FIXTURE=1 python3 third-party/redis/compat/run_bank_transfer.py`
  - `G2 bank transfer`: PASS, `bank transfer invariant preserved runs=3 transfers=190615 retries=0`.

- Latest verification just run:
  - `MAKO_PORT=6394 REDIS_CLUSTER_DIR=/tmp/redis-compat-cluster-phase12 REDIS_CLUSTER_PORTS='7200 7201 7202 7203 7204 7205' REDIS_CLUSTER_PORT=7200 MAKO_G2_ACCOUNTS=12 MAKO_G2_CLIENTS=2 MAKO_G2_ITERATIONS=20 G2_DEMO_START_REDIS_CLUSTER=1 python3 third-party/redis/compat/run_cross_shard_demo.py`: passed.
  - `MAKO_PORT=6395 MAKO_G2_USE_LOCAL_FIXTURE=1 MAKO_G2_ACCOUNTS=12 MAKO_G2_CLIENTS=2 MAKO_G2_ITERATIONS=20 G2_DEMO_START_REDIS_CLUSTER=1 REDIS_CLUSTER_DIR=/tmp/redis-compat-cluster-phase12-accept2 REDIS_CLUSTER_PORTS='7310 7311 7312 7313 7314 7315' REDIS_CLUSTER_PORT=7310 bash third-party/redis/compat/run_acceptance.sh`: produced the G2-focused artifact above.

- Phase 12 remaining after this update:
  - True G3 is still blocked on a replicated Redis-facing Mako deployment with leader kill/recover hooks.
  - TCL semantic guard still needs Redis TCL tests provided or fetched; they are not vendored by default.

## Latest Mainline-Comparable Redis Performance Update

- Mainline-comparable path is the default Mako backend, not
  `MAKO_REDIS_BACKEND=memory`.
  - Baseline CSV: `MakoCon-benchmark-ref/benchmark/mako_results.csv`.
  - Latest optimized CSV:
    `MakoCon-benchmark-ref/benchmark/mako_rawfast2_mako_1m_serial_preload.csv`.
  - Earlier Mako fast-path CSV:
    `MakoCon-benchmark-ref/benchmark/mako_fastpath_mako_1m_serial_preload.csv`.
  - Memory backend CSVs are useful for cache-mode research, but are not the
    mainline comparison.

- `mako-fork-pr/third-party/redis/rust-lib/src/lib.rs`
  - Avoids constructing `args` for plain `GET` and unconditional plain `SET`
    during RESP frame parsing.
  - Adds `execute_fast_mako_string_op` for direct one-operation Mako
    transactions for plain `GET` and unconditional plain `SET`.
  - Adds `try_fast_mako_string_command` as the parsed-command fallback fast path.
  - Adds a raw RESP array fast path for exact `GET key` and `SET key value`
    frames before the general RESP decoder.
  - Keeps extended `SET` forms, `MULTI`, pub/sub, blocking-command edge cases,
    OOM state, Lua-busy state, and blocked-client wakeup handling on the general
    path.
  - Makes WATCH bookkeeping lazy: key-version maps are only updated after WATCH
    state exists.
  - Adds unit coverage for the raw parser's exact GET/SET acceptance and
    extended SET fallback.

- `mako-fork-pr/third-party/redis/rust-lib/src/resp3_handler.rs`
  - Exposes buffered RESP bytes and a consume helper so the exact GET/SET fast
    path can consume complete frames without changing normal decoding behavior.

- Latest verification:
  - `cargo test` in `third-party/redis/rust-lib`: passed, `29 passed`.
  - `cmake --build /tmp/mako-fork-pr-poll-build --target makoCon -j 8`:
    passed with the existing `libunwind.so.1` linker warning.
  - Final benchmark command used serial preload because 8-thread preload still
    trips backend `-ERR backend` during concurrent load:
    `bench_resp --keys 1000000 --value-size 8 --threads 1,4,16 --duration 60 --preload-threads 1`.
  - Final Mako backend result versus baseline:
    - GET c=1: 19,918.93 ops/s vs 20,739.55, -4.0%.
    - GET c=4: 82,058.88 ops/s vs 82,153.32, -0.1%.
    - GET c=16: 304,521.61 ops/s vs 86,884.83, +250.5%.
    - PUT c=1: 19,094.34 ops/s vs 20,587.81, -7.3%.
    - PUT c=4: 79,896.29 ops/s vs 80,691.77, -1.0%.
    - PUT c=16: 267,311.27 ops/s vs 85,906.14, +211.2%.

- Latest latency-enabled benchmark update:
  - `MakoCon-benchmark-ref/benchmark/bench_resp.cpp`
    - Now records per-operation latency around each synchronous RESP
      `send`/`read_reply`.
    - Merges worker samples and writes real p50/p95/p99 microsecond values to
      the existing CSV columns.
  - Rebuilt binary:
    `MakoCon-benchmark-ref/benchmark/bench_resp`.
  - Latency artifact:
    `MakoCon-benchmark-ref/benchmark/mako_rawfast2_mako_1m_latency.csv`.
  - Command shape stayed the same as the prior stable run:
    `--keys 1000000 --value-size 8 --threads 1,4,16 --duration 60 --preload-threads 1`.
  - Latency-enabled result:
    - GET c=1: 19,710.88 ops/s, p50/p95/p99 = 46/58/69 us.
    - GET c=4: 82,058.95 ops/s, p50/p95/p99 = 45/54/63 us.
    - GET c=16: 332,113.92 ops/s, p50/p95/p99 = 42/71/92 us.
    - PUT c=1: 18,782.70 ops/s, p50/p95/p99 = 49/60/70 us.
    - PUT c=4: 77,729.10 ops/s, p50/p95/p99 = 47/60/70 us.
    - PUT c=16: 226,416.28 ops/s, p50/p95/p99 = 51/141/183 us.

- PR push/update:
  - Pushed commit `d88d02b007f37818c0acf27aa2fbaa642961b0b5`
    (`Optimize Redis plain GET SET fast path`) from local
    `mako-fork-pr:redis-compat-phase3` to PR head branch
    `SoumojitDalui/makodb:redis-compat-phase1`.
  - Updated PR #72 description:
    `https://github.com/makodb/mako/pull/72`.
  - The PR description now contains the mainline-comparable Mako backend update,
    throughput/latency table, baseline comparison table, and the explicit caveat
    that the new path is not better in every direction.

- Redis-vs-Mako comparison table update:
  - Created:
    `MakoCon-benchmark-ref/benchmark/redis_mako_latency_comparison.csv`.
  - Includes three complete row groups under the requested header:
    - `redis-published`: the published/their Redis rows from
      `redis_results.csv`.
    - `redis-local-latency`: rerun of their Redis benchmark locally with the
      patched latency-enabled `bench_resp`.
    - `mako-rawfast2-latency`: latest optimized Mako backend run.
  - Reran missing local Redis latency test on temporary Redis `127.0.0.1:6381`
    with persistence disabled, `--keys 1000000 --value-size 8 --threads 1,4,16
    --duration 60 --preload-threads 8`.
  - New local Redis latency artifact:
    `MakoCon-benchmark-ref/benchmark/redis_local_results_resp_latency.csv`.
  - Temporary Redis server was stopped after the run.

- 32-thread scaling and server CPU update:
  - Started optimized `makoCon` with `MAKO_REDIS_THREADS=32` on
    `127.0.0.1:6380`.
  - Ran latency benchmark with client threads `1,4,16,32`.
  - Benchmark artifact:
    `MakoCon-benchmark-ref/benchmark/mako_rawfast2_32workers_latency.csv`.
  - Per-thread CPU raw artifact:
    `MakoCon-benchmark-ref/benchmark/mako_32workers_pidstat.txt`.
  - Phase-level CPU summary:
    `MakoCon-benchmark-ref/benchmark/mako_32workers_cpu_summary.csv`.
  - Added local Redis 32-client comparison:
    `MakoCon-benchmark-ref/benchmark/redis_local_results_resp_latency_32.csv`.
  - Extended comparison artifact:
    `MakoCon-benchmark-ref/benchmark/redis_mako_latency_comparison.csv`.
  - 32-worker Mako result:
    - GET c=32: 486,851.70 ops/s, p50/p95/p99 = 60/91/122 us.
    - PUT c=32: 463,345.27 ops/s, p50/p95/p99 = 62/99/137 us.
  - Local Redis c=32 result:
    - GET c=32: 100,827.68 ops/s, p50/p95/p99 = 281/504/600 us.
    - PUT c=32: 96,480.29 ops/s, p50/p95/p99 = 295/519/604 us.
  - CPU summary for 32-worker Mako run:
    - GET c=32: aggregate worker CPU avg/p95/max = 1579.60/1989.00/1992.00%
      with avg/max active worker threads = 44.88/52.
    - PUT c=32: aggregate worker CPU avg/p95/max = 1543.42/1807.00/1812.00%
      with avg/max active worker threads = 44.08/51.
  - Temporary Mako and Redis servers were stopped after the runs.

- Clean 32-client CPU saturation rerun:
  - Restored the cached LLVM 21 runtime required by the existing optimized
    `makoCon` binary; no code rebuild or implementation change was made.
  - Preloaded 1,000,000 keys before starting CPU collection, then ran separate
    60-second GET and PUT phases with 32 clients and 32 server workers.
  - Throughput/latency artifact:
    `MakoCon-benchmark-ref/benchmark/mako_rawfast2_32workers_cpu_rerun.csv`.
  - Raw one-second per-thread CPU samples:
    `MakoCon-benchmark-ref/benchmark/mako_32workers_pidstat_rerun.txt`.
  - Phase summary:
    `MakoCon-benchmark-ref/benchmark/mako_32workers_cpu_rerun_summary.csv`.
  - GET c=32: 486,582.98 ops/s, p50/p95/p99 = 60/91/125 us; server CPU
    avg/p95/max = 22.52/22.66/22.72 cores.
  - PUT c=32: 461,375.04 ops/s, p50/p95/p99 = 61/104/151 us; server CPU
    avg/p95/max = 16.84/16.93/17.05 cores.
  - The host has 32 physical cores (64 logical CPUs), so whole-server CPU use
    was 70.4% of physical capacity for GET and 52.6% for PUT. I/O wait was
    effectively zero in both phases.
  - Per-thread samples show connection-distribution skew: 24 server workers
    were busy during GET and 18 during PUT. All 24 GET workers averaged at
    least 90% CPU; all 18 PUT workers averaged at least 80%, with 8 at least
    90%. The benchmark opens one persistent connection per client, while each
    worker owns a `SO_REUSEPORT` listener, so the kernel hashes 32 connections
    across workers and does not guarantee one connection per worker.
  - Conclusion: the machine and full 32-worker pool were not CPU-saturated,
    but the subset of workers that received connections was close to
    saturated. The clean rerun supersedes the earlier phase averages for CPU
    saturation analysis.
  - The temporary Mako server was stopped after the rerun.

- Replicated G3 leader crash/recovery run:
  - Used current PR commit `d88d02b0` and the optimized `makoCon` binary.
  - Built a temporary one-shard, four-process Paxos fixture with roles
    `localhost`, `p1`, `p2`, and `learner`; each process ran one Redis/Mako
    worker with replication enabled.
  - Ran the existing
    `third-party/redis/compat/run_failover_durability.py` acknowledged-write
    oracle with 100 writes against `localhost` on `127.0.0.1:6390`.
  - The harness killed the Redis-serving `localhost` process after write index
    50, waited 1 second, restarted that role against the surviving processes,
    waited 3 seconds, and reconnected.
  - Result: `FAIL acked=100 missing=51 wrong=0`.
  - The learner log detected the failed localhost role and entered the
    fail-recovery path, but the restarted Redis endpoint did not restore the 51
    acknowledged pre-fault writes.
  - Result artifact:
    `MakoCon-benchmark-ref/benchmark/mako_g3_replicated_failover_result.txt`.
  - Scope caveat: this validates replicated leader crash/recovery through a
    restarted `localhost` Redis endpoint. It does not yet route clients to the
    newly elected learner, so transparent Redis service failover remains
    untested.
  - All four temporary Mako processes were stopped after diagnosis.

- Replicated G3 correction and passing rerun:
  - The initial failure had two concrete causes. Replicated Redis commits used
    Mako's default 400-transaction serialization batch, so the 51 acknowledged
    pre-fault writes had not been submitted to Paxos. The recovery hook also
    restarted an empty `localhost` process instead of reconnecting to the
    promoted learner.
  - Added a runtime replication-leader query shared by Paxos and Raft. The
    Redis adapter rejects writes on followers and permits them after a learner
    is promoted.
  - Replication-enabled `makoCon` now forces one transaction per replication
    log and waits for `wait_for_submit()` after successful write commits before
    returning a Redis success response. Non-replicated benchmark behavior is
    unchanged.
  - Scoped the Paxos learner's legacy 35-second `quick_exit()` cutoff to
    non-Redis programs. `makoCon` marks itself with `MAKO_REDIS_SERVER=1`, so
    Redis learners continue into the existing promotion path while other Mako
    programs preserve their prior behavior.
  - Extended `run_failover_durability.py` with a recovery target and added the
    reproducible four-process fixture:
    `third-party/redis/compat/fixtures/makocon_g3_replicated.sh` with
    `makocon_g3_paxos.yml`.
  - Role checks passed: the learner returned `ERR backend` before promotion,
    all replicas contained a leader-acknowledged value, and the learner became
    writable while preserving that value after the leader was killed.
  - Full G3 passed immediately and after 37 seconds of cluster uptime:
    `G3 durability preserved acked=100 writes`, with zero missing and zero
    wrong values. The packaged repository fixture repeated the immediate PASS.
  - PASS artifact:
    `MakoCon-benchmark-ref/benchmark/mako_g3_replicated_failover_pass_result.txt`.
    The original FAIL artifact remains as the pre-fix record.
  - Verification: `makoCon` built successfully, all 29 Rust tests passed,
    `git diff --check` passed, and the Python/shell fixture syntax checks passed.
  - All temporary and packaged fixture processes were stopped after testing.
  - After scoping the quick-exit exception, rebuilt `makoCon`, kept the
    four-process fixture alive beyond 35 seconds, and reran G3. The learner
    remained alive and promotion preserved all 100 acknowledged writes.

- Post-G3 non-replicated performance regression check:
  - Benchmarked PR commit `f6add509` with the optimized poll-based `makoCon`,
    replication disabled, 32 server workers, 1,000,000 decimal keys, 8-byte
    values, serial preload, and 60-second GET/PUT phases.
  - Full 1/4/16-client artifact:
    `MakoCon-benchmark-ref/benchmark/mako_g3fix_mako_1m_latency.csv`.
  - Versus the pre-G3 `mako_rawfast2_mako_1m_latency.csv`, GET throughput
    changed by -1.4%, -1.7%, and -0.4%; PUT changed by -5.3%, -2.5%, and
    +33.2% at 1, 4, and 16 clients respectively.
  - A fresh one-client repeat reduced the apparent PUT difference to -3.1%
    with p50/p95/p99 = 50/63/71 us. Artifact:
    `MakoCon-benchmark-ref/benchmark/mako_g3fix_mako_1client_latency_repeat.csv`.
  - The first 32-client run preserved throughput but showed a GET tail-latency
    outlier. A fresh exact repeat reached 505,032.88 GET ops/s at 58/84/105 us
    and 484,513.42 PUT ops/s at 60/91/119 us. Artifact:
    `MakoCon-benchmark-ref/benchmark/mako_g3fix_32clients_latency_repeat.csv`.
  - Compared with the pre-G3 32-client baseline, the repeat improved GET
    throughput by 3.8% and PUT by 5.0%, while all p50/p95/p99 values improved.
  - Conclusion: the G3 changes maintain non-replicated performance quality.
    Small low-client differences are within observed run variance, while the
    high-concurrency write path improved materially.
  - Every temporary benchmark server was stopped after the runs.

- Raft leader-check audit for the Redis path:
  - `RaftServer::setIsLeader()` already fires `RegisterLeaderChangeCallback`,
    and `RaftWorker` caches the resulting role in `is_leader` under
    `election_state_lock`.
  - The new Raft `is_replication_leader()` currently calls
    `RaftServer::IsLeader()`, which reads a plain `bool` without synchronization.
    Repeatedly fetching `Communicator::GetLeaderForPartition()` would still be a
    cached, potentially stale view and would not close the role-change race.
  - Preferred follow-up: make the callback-fed `RaftWorker::is_leader` an atomic
    role cache and have the helper perform one atomic load per replicated write.
    Keep `RaftServer::Start()` as the authoritative under-lock leadership check.
  - The audit also found that `wait_for_submit()` only waits for the Raft submit
    queue to drain; it does not prove that a log was accepted and durably
    committed. Raft Redis acknowledgements therefore need a propagated
    append/commit result before they can claim the same G3 guarantee as Paxos.

- Minimal G3 learner-lifetime follow-up:
  - Reverted the experimental Raft per-entry completion, atomic core-state,
    dispatcher, and Redis error-propagation changes. They are not part of PR
    #72.
  - `third-party/redis/cpp/makoCon.cc` now sets `MAKO_REDIS_SERVER=1` at startup.
  - `src/deptran/paxos_main_helper.cc` preserves the legacy 35-second learner
    `quick_exit()` for non-Redis programs but skips it for `makoCon`, allowing
    the existing Redis promotion path to run after longer uptimes.
  - The reduced two-file diff built successfully and all 29 Redis Rust tests
    passed.
  - Delayed G3 passed after all four fixture roles remained alive for 37
    seconds: `G3 durability preserved acked=100 writes`.
  - All temporary fixture processes were stopped.

- Current-commit 32-worker CPU rerun:
  - Reran commit `c28e39f2` with replication disabled, 32 server workers,
    1,000,000 preloaded decimal keys, 8-byte values, and separate 60-second
    GET/PUT phases at 32 persistent clients.
  - GET reached 476,266.18 ops/s at 60/103/142 us p50/p95/p99 while using
    16.47 average server cores (19.02 p95, 19.19 max).
  - PUT reached 471,141.35 ops/s at 61/94/126 us p50/p95/p99 while using
    19.74 average server cores (20.99 p95, 21.07 max).
  - This is 51.5% and 61.7% of the host's 32 physical cores, respectively;
    neither phase saturated the machine or the full 32-worker pool.
  - `SO_REUSEPORT` spread the separately opened connections unevenly. Twenty
    worker threads averaged at least 70% CPU during GET and 22 during PUT;
    the remaining listener workers were idle or lightly used.
  - Artifacts:
    `MakoCon-benchmark-ref/benchmark/mako_g3minimal_32workers_cpu_rerun.csv`,
    `mako_g3minimal_32workers_pidstat_rerun.txt`, and
    `mako_g3minimal_32workers_cpu_rerun_summary.csv`.
  - The temporary benchmark server was stopped after collection.

- Current-commit 1-to-32-client CPU scaling sweep:
  - Held the server at 32 workers and tested 1, 2, 4, 8, 16, and 32 persistent
    clients. Each point used separate 60-second GET and PUT phases over the same
    1,000,000 preloaded decimal keys and 8-byte values.
  - GET throughput scaled from 19,532.50 ops/s at one client to 503,625.64 at
    32; PUT scaled from 18,115.34 to 473,971.37 ops/s.
  - Average server CPU cores for GET were 1.73, 2.46, 3.98, 6.47, 10.92, and
    20.77. PUT used 1.73, 1.91, 3.45, 6.56, 11.68, and 19.81 cores.
  - Substantially busy worker counts for GET were 1, 2, 4, 7, 12, and 22;
    PUT used 1, 1, 3, 7, 13, and 21. This is the expected connection collision
    pattern from hashing persistent connections across `SO_REUSEPORT` listeners.
  - At 32 clients every occupied request worker averaged at least 80% CPU, but
    the process used only 64.9% of physical cores for GET and 61.9% for PUT.
    The busy listener subset was saturated; the machine and full worker pool
    were not.
  - Combined artifacts:
    `MakoCon-benchmark-ref/benchmark/mako_g3minimal_worker_scale_summary.csv`,
    `mako_g3minimal_worker_scale_results.csv`, and
    `mako_g3minimal_worker_scale_cpu.csv`. Per-point raw `pidstat` traces and
    the exact phase timeline are stored beside them.
  - The temporary benchmark server was stopped after collection.

## Latest Shared-Listener Worker Distribution Update

- Active checkout: `/home/users/ssoumojit/mako-fork-pr`.
  - Branch: `redis-compat-phase3`.
  - Base commit for the compatibility update: `c28e39f2affee7c74ecb9747342c0811dd560053`.

- Compatibility stabilization remains the active workstream:
  - Command and protocol tests: `third-party/redis/compat/`.
  - Rust parser, connection state, reply shaping, and worker loop:
    `third-party/redis/rust-lib/src/lib.rs`.
  - Canonical Mako-backed command execution:
    `third-party/redis/cpp/makoCon.cc`.
  - FFI contract: `third-party/redis/include/transaction_ffi.h`.
  - Scoped compatibility claims and known gaps: `docs/redis_interface.md`,
    `third-party/redis/compat/known_divergences.txt`, and
    `third-party/redis/compat/run_acceptance.sh`.

- `third-party/redis/rust-lib/src/lib.rs`
  - Owns the single nonblocking TCP listener created by `rust_init()`.
  - Owns `worker_has_accept_turn()` and `advance_accept_turn()`, which assign
    persistent connections to workers in deterministic round-robin order.
  - Owns `WorkerWake`, the per-worker nonblocking Unix socket pair used to wake
    a subscriber's poll loop after a publisher queues a cross-worker message.
  - Owns protocol-specific reply shaping. `HGETALL` is a RESP3 map, while
    sorted-set scalar scores are RESP3 doubles and remain bulk strings in RESP2.
  - Owns `TOTAL_COMMANDS_PROCESSED`, including explicit accounting in the raw
    Mako `GET`/`SET` fast path, exposure through `INFO stats`, and reset through
    `CONFIG RESETSTAT`.
  - Contains the focused unit tests for accept-turn cycling, command statistics,
    worker wake notification, RESP3 `HGETALL`, and RESP3 `ZSCORE`.

- `third-party/redis/cpp/makoCon.cc`
  - Remains the Mako-backed Redis semantic target.
  - Its startup comments now point to the shared-listener, nonblocking Rust
    connection layer and synchronous Mako transaction execution.

- Architecture and scope documentation:
  - `third-party/redis/rust-lib/README.md` describes the current listener,
    poll-loop, connection ownership, wake channel, and protocol model.
  - `docs/status.md` points to the current Redis command surface and
    compatibility suite instead of the removed `SO_REUSEPORT` design.

- Current release build:
  - Binary: `/tmp/codex-pr72-build-final/makoCon`.
  - CMake: `/tmp/codex-cmake-330/bin/cmake`.
  - LLVM runtime: `/tmp/codex-llvm21/usr/lib/x86_64-linux-gnu` and
    `/tmp/codex-llvm21/usr/lib/llvm-21/lib`.

- Current verification:
  - Rust release unit tests: 34 passed.
  - Python Redis compatibility suite in `third-party/redis/compat/`: 105 passed.
  - Eight independent Pub/Sub subscribers: 8/8 cross-worker deliveries.
  - Concurrent Mako-backed check: 40,000 `SET`, 40,000 `GET`, and 8,000
    shared-key `INCR` operations; final counter was exactly 8,000.
  - `git diff --check` and the release `makoCon` build passed.

- CPU sweep artifacts are temporary and live outside the repository:
  - Single-client-thread samples: `/tmp/codex-pidstat-{1,2,4,8,16,32}.txt`.
  - Matching-client-thread samples:
    `/tmp/codex-pidstat-mt-{1,2,4,8,16,32}.txt`.
  - The sweep held the server at 32 workers. Both variants activated exactly
    1/2/4/8/16/32 request workers for 1/2/4/8/16/32 persistent connections.
  - No temporary `makoCon`, benchmark, or reference Redis process was left
    running after verification.
