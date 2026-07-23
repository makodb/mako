# SOUL

This file records how and why the Redis compatibility work should proceed.

## Principle

Compatibility is not a claim we get by writing our own happy-path tests. It has to be demonstrated against the Redis ecosystem.

The testing strategy is therefore layered:

1. Use Redis/Valkey-style semantic tests for command behavior.
2. Use real client libraries unmodified for ecosystem behavior.
3. Use project-native tests for Mako-specific claims.
4. Use fuzzing and sanitizers for protocol-parser safety.
5. Keep explicit divergence lists so skipped tests cannot silently hide bugs.

## Why Correctness Runs Are Serialized

Several compatibility harnesses intentionally clear the Redis keyspace during
setup. Running a command probe or Tcl file concurrently with an isolation,
soak, or benchmark workload can delete that workload's state and manufacture a
failure that is not a product bug. Run destructive suites serially, record the
exact target and build, and rerun any result collected during accidental
overlap. Concurrency belongs inside the workload being tested, not between
uncoordinated harnesses sharing one keyspace.

## How To Interpret Worker CPU

Measure Redis request workers separately from Mako transport/helper threads and
from the client load generator. A persistent client is assigned to one request
worker, so N clients should activate at most N request workers until all workers
are in use. Active-worker count is not the same as CPU saturation: a worker can
be active while spending part of the interval waiting on sockets or Mako.

Always run a matching-thread client mode in addition to a one-thread generator.
If throughput plateaus while server CPU remains available, the single client
generator is the likely bottleneck. Report the command, hit/miss state,
pipelining level, backend, client threads, request-worker CPU, helper CPU, and
throughput together so a client-side ceiling is not presented as a Mako
regression.

## Redis Layer Ownership

Redis compatibility is an adapter layer. Keep Redis-owned Rust, C++, FFI, fixtures, probes, and compatibility tests under `third-party/redis/`.

Do not push Redis implementation changes through the old `third-party/makocon` submodule. The PR should be self-contained in the main repo branch.

Do not add Redis-only APIs to core Mako storage internals. If a command needs a core feature, the feature must make sense as a general Mako capability; otherwise the Redis adapter should compose existing generic DB/table operations.

## Why Step 0.1 Comes First

The harness must exist before implementation changes. Otherwise every later command implementation is unverifiable.

Step 0.1 is intentionally small:

- prove the harness can connect to Redis and makoCon,
- prove the same pytest fixture can target both systems,
- produce a CSV runner output that later phases can grow into a real scoreboard.

That is why `test_ping.py` has only two cases today: one for Redis and one for makoCon. It is a harness smoke test, not the compatibility suite.

## How To Treat Client Tests

Client tests are not all equivalent.

- `redis-py`, `node-redis`, `ioredis`, and `Jedis` are high-priority because they represent common production client behavior.
- `redis-cli` is required because it gives a simple reproducible protocol sanity check.
- `fakeredis-py` is useful because it is designed around comparing Redis-like behavior.
- `redis-rs` is useful as a Rust client check, but it must be filtered to Mako's claimed command surface.

Do not run full client suites unfiltered when they include Redis subsystems Mako does not claim, such as Cluster, Sentinel, Streams, Lua, ACL, modules, TLS, or RESP3 client-side caching.

Failures inside the filtered, in-scope subset are real Mako bugs. Failures outside the claimed scope must be documented as intentional divergences.

## How To Treat Redis Tcl Tests

The Redis Tcl suite is the semantic baseline for command behavior. Run the in-scope files against a clean reference Redis first, then against makoCon.

Use `REDIS_COMPAT_START_REDIS=1 bash third-party/redis/compat/run_tcl_suite.sh` when possible. That starts a disposable Redis configured for upstream tests, including `enable-debug-command yes`; a random existing Redis on port 6379 may fail tests for server-configuration reasons instead of semantic reasons.

The runner uses `--singledb --ignore-encoding --ignore-digest` by default because Mako is claiming Redis wire semantics, not Redis's internal logical-DB, encoding, or digest behavior.

Setup-command failures are still compatibility gaps when the command is in the plan. For example, if Tcl files cannot start because makoCon rejects `FLUSHALL`, do not skip the suite and call it done. Implement the planned gated cleanup command or add a reviewed divergence if the plan changes.

## How To Add New Compatibility Work

Use TDD:

1. Add the smallest failing test for the command or behavior.
2. Run it against Redis and makoCon.
3. Confirm Redis passes and makoCon fails for the right reason.
4. Implement the minimal Mako change.
5. Re-run the test and keep it in the suite.
6. Record any intentional divergence by name and reason.

Do not add implementation first and test afterward.

## Phase Review Guard

At the end of every phase, before calling it complete:

1. Cross-check the implementation against the source plan and project docs.
2. Re-check PR #60 comments and confirm the phase does not repeat the same mistakes.
3. Run the correctness tests for the changed command surface.
4. Run Redis benchmarks after the implementation, including `redis-benchmark` and `memtier_benchmark` when makoCon can start.
5. Call out any exception directly in the phase notes or PR text.

For Phase 12 specifically, do not call the work release-clean unless all twelve
acceptance lines pass. `N/A` is useful evidence about missing harnesses, but it
is not a pass.

The Phase 12 harness files were missing because the implementation phases moved
command semantics forward without bringing the Phase 0 acceptance scripts into
this PR checkout. Fix this by adding the harness entrypoint, not by marking the
line PASS. A harness that exists but needs external setup should exit `78`, and
`run_acceptance.sh` should report that as `N/A`.

The PR #60 checks are:

- no atomics in hot storage paths,
- no transaction lifecycle changes unless the docs require them,
- no unnecessary pre-read before `put` / `insert`,
- no partial scan/list-table interfaces,
- no deletion shortcuts without understanding flags and `current_e`,
- comments for new interfaces.

## Git Push Rule

All PR updates must be pushed from `/home/users/ssoumojit/mako-fork-pr` using
SoumojitDalui auth.

Do not use the default `/home/users/wshen24/.gitconfig` for PR pushes. It has a
GitHub URL rewrite that can make GitHub record the push under the wrong token
owner. Use:

```bash
GIT_CONFIG_GLOBAL=/dev/null \
HOME=/home/users/ssoumojit \
GH_CONFIG_DIR=/home/users/ssoumojit/.config/gh \
git push ...
```

## How To Read Phase 0 Acceptance

`run_acceptance.sh` is a starting-line orchestrator, not a release gate that should pass today.

Interpret statuses literally:

- `PASS`: the harness ran and the current system satisfied that claim or guard.
- `FAIL`: the harness ran and exposed a current product gap.
- `N/A`: the harness is present, but required external setup is missing, such as a replicated Mako topology, `memtier_benchmark`, or a restart command.

Do not turn `N/A` into `PASS`. Do not hide `FAIL` behind `known_divergences.txt` unless a test has been reviewed and the divergence is intentional.

The useful Phase 0 outcome is that every later implementation step has a place to show progress: command probes, client tests, Tcl semantics, claim tests, and operational guards all now produce machine-readable artifacts plus a twelve-line human summary.

## How To Treat External Fixture Harnesses

True G3, TCL, restart durability, and client failover are environment
tests, not just files in `third-party/redis/compat`.

- G2 now has a checked-in local multi-shard Mako Redis fixture. Single-shard
  bank transfer remains smoke only.
- G3 needs a replicated Mako topology plus start/kill/recover hooks. A local
  process restart does not prove acknowledged-write survival after failover.
- G4 now has a checked-in Redis-facing RMW serializability oracle. External
  Elle analysis remains optional for externally generated histories.
- TCL needs the Redis tests directory from a pinned Redis checkout or explicit
  bootstrap. Do not vendor/fetch it silently.
- Restart durability needs a persistent Redis storage path. The current
  in-memory `makoCon` path is expected to fail that guard.
- Client failover needs multiple targets to prove failover. One target is only
  a reconnect smoke test.
- RESP fuzz needs deterministic input and delayed liveness checks. A short
  post-fuzz `PING` is not enough; longer fuzz-plus-soak remains a separate
  stability follow-up if the server exits later.

If the fixture is missing, return exit code `78` and record `N/A`. Do not turn
fixture absence into `PASS`.

## How To Treat The Phase 1 FFI Contract

Do not use `data_len == 0` to mean "missing." Redis has valid empty bulk strings, so the FFI result must carry presence separately from byte length.

The intended interpretation is:

- `success=false`: operation/backend failure.
- `success=true, value_present=false`: key did not exist.
- `success=true, value_present=true, data_len=0`: key existed with an empty value, or an existence-returning operation found the key.
- `success=true, value_present=true, data_len>0`: key existed and `data_ptr` contains the value bytes.

Any future command that aggregates per-key operations, such as variadic `EXISTS`, `DEL`, `MGET`, or set/list/zset operations, should preserve Redis's one-command response shape even when it expands to multiple internal FFI operations.

When multiple operations in one `EXEC` can touch the same composite Redis keys,
stage the logical type in memory and flush once before commit. Lists and zsets
already use this pattern; sets need it too for cases like `SADD` followed by
`SMOVE` of the same new member. Do not issue a real storage `Put` and later a
real storage `Delete` for the same internal member key in one Mako transaction.

## How To Implement Existence Checks

Keep Redis existence-style commands inside the Redis adapter layer unless Mako core already exposes a general presence API.

The current implementation uses existing generic table reads to determine presence, then applies Redis response shaping in `third-party/redis/cpp/makoCon.cc`. This may materialize a value for `EXISTS`, but it avoids adding Redis-specific hooks to `MassTrans.hh`, `abstract_ordered_index.h`, or the MBTA wrappers.

Do not reintroduce `transExists()` or a Redis-only `abstract_ordered_index::exists()` path just to optimize Redis `EXISTS`. A no-copy presence check can be revisited only as a general Mako storage API with non-Redis callers and tests.

## How To Treat Phase 2 Connection Commands

Phase 2.1 commands are connection-level compatibility, not storage semantics. Keep them in Rust unless they need database state.

The goal is to let real Redis clients finish their handshake:

- `HELLO` should return a parseable capability map.
- `CLIENT SETNAME` / `GETNAME` should preserve per-connection metadata.
- `CLIENT SETINFO` should accept library metadata without touching storage.
- `COMMAND` should return a parseable response, even if the command table is minimal at this stage.
- `RESET` should clear connection and transaction state.
- `QUIT` should flush its reply before closing.
- `SELECT 0`, `AUTH`, and `ECHO` are compatibility shims for clients that expect them.

Do not route these commands through C++ FFI. If a command only manages connection state, test it as a pure Rust dispatch path.

`PING` is pure Rust, but it still obeys `MULTI`: queue it inside a transaction and return its reply from `EXEC`.

`INFO` is the exception in Phase 2: the formatting stays in Rust, but metrics come from a small C++ FFI getter so the values describe the real transaction executor.

`CLIENT ID` returns a stable per-connection integer. Do not re-add the old
`CLIENT ID returns 0` divergence.

Parse errors should be typed before formatting. Returning one generic `unsupported command` hides whether the parser saw an unknown verb, wrong arity, or malformed protocol input. Keep those categories separate so later Redis/Valkey protocol-error tests can compare exact strings.

## Why Filter Instead Of Convert

The goal is not to rewrite upstream tests until they say Mako passes. The goal is to identify the part of each suite that tests the behavior Mako actually claims.

Filtering is acceptable when it removes out-of-scope subsystems. It is not acceptable when it hides an in-scope failure.

Examples:

- Excluding Redis Cluster tests is acceptable because Mako's sharding model is different.
- Excluding Sentinel tests is acceptable because Mako does not claim Sentinel compatibility.
- Excluding `MGET` from the filtered subset would not be acceptable once Mako claims `MGET`.

## How To Treat makoConMultiTrd

The Redis semantic target is `third-party/redis/cpp/makoCon.cc`.

`third-party/redis/cpp/makoConMultiTrd.cc` may stay ABI-aligned with the shared Rust FFI, but do not duplicate every Redis command implementation there. Duplicating logic creates two semantic paths to maintain and makes it easy for TTL, transaction, and response-shape behavior to drift.

If `makoConMultiTrd` must become a correctness target, first extract the transaction executor into a shared implementation and make both binaries call it. Do not add one-off TTL/hash/set/list logic to `makoConMultiTrd` as a shortcut.

Phase 11 shares only ABI helpers, response allocation/freeing, metrics, retry
accounting, and layout assertions. That is not full executor deduplication.
Keep the remaining MultiTrd extended-command semantic gap visible in docs and
PR notes.

Project decision for PR #72: keep `makoConMultiTrd` ABI-compatible only. Do
not make it a Redis correctness target in this PR unless the executor is first
deduplicated with `makoCon.cc`.

## Redis Scope Decisions

Mako Redis is a single-keyspace adapter. `SELECT 0` exists for client
compatibility, but Redis logical DBs such as `SELECT 1` and DB-scoped
namespaces are out of scope. `FLUSHDB` and `FLUSHALL` clear the single Mako
Redis keyspace.

Do not expand this PR to Redis Streams or keyspace notification Pub/Sub. Normal
Pub/Sub command delivery is in scope; Streams, stream consumer groups, blocking
stream timeout behavior, and mutation-event notification fanout remain explicit
Tcl skips.

## How To Treat Phase 5 Scan

`SCAN`, `KEYS`, and `DBSIZE` are string-key enumeration only.

Do not claim cross-shard Redis SCAN from the current `makoCon` path. PR #60 made clear that local-only scan is wrong if presented as a full distributed scan. The current Redis server runs with one shard, so local scan is correct for this binary. If multi-shard `makoCon` is added later, scan needs a documented remote-scan strategy or must reject the request.

Return Redis-compatible numeric cursors. Internally, Rust can map those cursor ids to last-seen key bytes, but clients such as `redis-py` expect cursor tokens to parse as integers.

`HSCAN` waits for hash storage. Do not invent a hash field scan format before `HSET`/`HGET` define the encoding.

Large keyspace commands must not run as one unbounded Mako transaction. The
Redis adapter should chunk standalone `FLUSHDB`/`FLUSHALL` and `DBSIZE`
workloads, commit or roll back each bounded batch, and keep liveness checks in
the benchmark flow. Memtier exposed both issues at roughly a 50k-key keyspace:
`FLUSHALL` could terminate the server after benchmark population, and then
`DBSIZE` could terminate it before the next benchmark pass.

`LPOS ... COUNT` is an array of Redis integers. Do not encode positions as bulk
strings just because the C++ FFI uses a packed byte-list internally.

## How To Treat Phase 6 Sets

Sets use composite internal keys because the current Redis layer has no native
Redis type table. Keep the composite keys hidden from clients.

`SRANDMEMBER` and `SPOP` should use adapter-local PRNG sampling over the
transactionally visible member set. Do not use deterministic scan order as a
stand-in for Redis random sampling.

Use:

- `\x01S:<u64 set-len><set><member>` for member presence.
- `\x01S#:<u64 set-len><set>` for cardinality.

Do not expose those keys through `KEYS`, `SCAN`, or `DBSIZE`.
Do not store set internals under the visible `table_key_` namespace.

`SINTERSTORE`, `SUNIONSTORE`, and `SDIFFSTORE` must write the destination in the
same OCC transaction as the source reads. Reconcile the destination in place:
delete stale members, keep overlapping members, and insert new members. Avoid
delete-then-reinsert of the same composite key in one transaction.

TTL is keyed by the visible Redis key. For set-aware logical commands, expiry
must delete the bare string key, set members, set cardinality, and TTL metadata
together. Do not let the string path consume TTL metadata before the set path
can clean up the composite keys.

Current keyspace scan remains string-key enumeration. This is a documented
limitation, not a hidden compatibility claim. PR #60's scan comments still
apply: do not present local/internal scan as distributed or full typed Redis
keyspace scan.

## How To Treat Phase 7 Lists

Lists are non-blocking only. Do not add `BLPOP`, `BRPOP`, `BLMOVE`, or related
blocking commands without a new wait-queue design.

Use a transaction-local list overlay for list commands. Redis requires reads
inside one `EXEC` to observe earlier queued list writes. Writing every list
operation directly to composite keys can create put/delete conflicts on the same
internal key, so dirty lists are flushed once before commit.

Immediate expiry must also update the list overlay. If `EXPIRE key 0` or an
expired TTL path sees a staged list, clear the staged list and mark it dirty so
commit does not flush stale elements back into storage.

`LINSERT` return codes follow Redis: missing list returns `0`; existing list
without the pivot returns `-1`.

Keep list internals hidden from `KEYS`, `SCAN`, and `DBSIZE` until a shared
logical key index exists. Do not expose `\x01L:` or `\x01L#:` as Redis-visible
keys.

## How To Treat Phase 8 Sorted Sets

Sorted sets use two internal indexes: member -> score and encoded-score ->
member. Keep both indexes inside one C++ transaction.

Use a transaction-local zset overlay for zset commands. Reads inside one `EXEC`
must observe earlier queued zset writes.

When a member score changes, do not delete and reinsert the same member key in
one transaction. Delete only the stale score-index key, then overwrite the
member key and write the new score-index key. This avoids the put/delete hazard
called out in PR #60.

Keep zset internals hidden from `KEYS`, `SCAN`, and `DBSIZE` until a shared
logical key index exists. Do not expose `\x01Z:`, `\x01ZS:`, or `\x01Z#:` as
Redis-visible keys.

Current Phase 8 gaps are explicit: no lex range, no range-removal commands, and
no sorted-set store variants yet. Cursor pagination for `ZSCAN` was added in
Phase 10.

## How To Treat Phase 9 Pub/Sub

Pub/Sub is Rust handler state, not a Mako storage feature.

Keep `SUBSCRIBE`, `PSUBSCRIBE`, `UNSUBSCRIBE`, `PUNSUBSCRIBE`, `PUBLISH`, and
`PUBSUB` in the Rust connection layer unless a future phase explicitly adds
distributed Pub/Sub. Do not add C++ FFI or storage records for messages.

Subscriber mode is asymmetric: after a connection subscribes, it may only use
subscribe/unsubscribe commands, `PING`, `QUIT`, and `RESET`. Storage commands
must return an error until the subscription count returns to zero.

Use weak queue handles in the global registry. A disconnected client must not be
kept alive by a channel entry.

`PUBLISH` inside `MULTI` must queue and deliver on `EXEC`. Delivering
immediately is a shortcut because it leaks the side effect before the
transaction reply.

Pub/Sub metrics are scoped: expose `pubsub_channels` and `pubsub_patterns`.
Do not invent Redis replication, blocked-client, or sharded Pub/Sub counters.

Current Phase 9 scope is process-local only. Messages are not persisted, not
replayed after disconnect, and not distributed across Mako nodes.

## How To Treat Phase 10 Scan Family

Member scans are not the same as full keyspace scans.

`SSCAN` and `ZSCAN` can be implemented safely in Rust by reusing existing
transactional set/zset member reads and adding cursor pagination in the handler.
This does not add new storage interfaces and does not expose internal composite
keys.

Keep `MATCH` filtering in Rust. For `ZSCAN`, match against member names, not
score strings.

Do not implement full collection-key visibility for `KEYS`, `SCAN`, or
`DBSIZE` by scanning local composite prefixes. PR #60 made clear that local-only
or dummy-table scan results are misleading for distributed Mako. Full logical
keyspace visibility needs a shared logical-key index or an explicit remote-scan
design.

`HSCAN` remains blocked until hash storage exists. Do not invent a hash scan
encoding before `HSET`/`HGET` define the storage format.

## Release Standard

The end state should not be "all tests pass" in the abstract. The end state should be:

- all in-scope Redis semantic tests pass,
- all in-scope real-client tests pass,
- all intentional divergences are named,
- Mako-specific claims are tested separately:
  - cross-shard atomicity,
  - failover durability,
  - serializable isolation,
  - Redis wire compatibility.

## How To Treat Latest G2/G3 Fixture Work

G2 now has a concrete local multi-shard Redis-facing fixture. It starts one
`makoCon` process with three local Mako shard tables (`MAKO_NUM_SHARDS=3`,
`MAKO_LOCAL_SHARDS=0,1,2`) and runs the bank-transfer oracle through Redis
`MULTI`/`EXEC`.

This is stronger than single-shard smoke because keys are routed across Mako's
sharded table abstraction. It is the G2 Redis-facing fixture for this PR. Keep
the distinction from separate live-replication services visible in reports:

- single-shard smoke proves the Redis transaction command path works;
- local multi-shard G2 proves the Redis path works over Mako sharded-table
  routing;
- `run_cross_shard_demo.py` now captures the side-by-side Redis Cluster
  rejection artifact against the local multi-shard Mako fixture;
- `bash/shard.sh` / `dbtest` are benchmark/live-replication runners, not
  Redis-serving backing services for G2.

The G2 oracle is intentionally borrowed from established distributed transaction
testing practice: concurrent bank transfers must preserve total balance exactly.
The implementation cites CockroachDB's bank roachtest family as provenance, but
the reusable part is the invariant, not Cockroach's SQL implementation.

G3 now has an executable local restart hook, but it is deliberately smoke-only.
Do not turn it into a PASS for failover durability. The current Redis path is
in-memory; when the hook is explicitly enabled, it should expose missing
acknowledged writes after restart. That is useful evidence, not a success.

A real G3 PASS still requires:

- a replicated Redis-facing Mako deployment;
- start/kill/recover hooks that target the current leader;
- client reconnection to a live service;
- the acknowledged-write survival oracle passing after fault/recovery.

The G3 oracle follows Jepsen Redis-Raft style methodology: every write
acknowledged to the client before/during the fault must be readable after
recovery. The local restart hook only validates the harness plumbing.

## How To Treat Latest Phase 12 Acceptance

The newest full G2 local multi-shard validation passed with
`bank transfer invariant preserved runs=3 transfers=190615 retries=0`.

It is valid evidence for two local-fixture claims:

- `G2 bank transfer` passes with `MAKO_G2_USE_LOCAL_FIXTURE=1`.
- `G2 cross-shard demo` passes by showing Redis Cluster rejects a cross-slot
  transaction while the Mako local multi-shard fixture preserves the bank
  invariant.

It is not a full Phase 12 release gate because the run intentionally did not
keep one shared `makoCon` endpoint alive for G1/throughput/INFO/soak/fuzz, and
because G3/TCL still require external real fixtures. Keep G3 marked `N/A`
until the replicated Redis-facing deployment exists and the acknowledged-write
survival oracle passes after leader kill/recover.

## How To Treat Mainline-Comparable Redis Performance

The fair comparison to the current mainline Redis implementation is the default
Mako backend path. The memory backend is valuable because it models cache-like
Redis behavior, but it changes the storage contract and should not be used as
the headline comparison against mainline's Mako-backed Redis frontend.

The latest optimization intentionally targets only the shared benchmark surface:
plain `GET key` and unconditional plain `SET key value`. It removes fixed costs
that were unrelated to the old implementation's simple command path:

- no duplicated `args` vector for plain GET/SET command parsing;
- no general transaction-op vector construction for one-operation GET/SET;
- no general RESP frame decoding for exact raw GET/SET frames;
- no WATCH version-map lock/insert on every write before any client has used
  WATCH.

The implementation is deliberately narrow. Extended Redis behavior still goes
through the general path so the broader compatibility work is not weakened just
to win a microbenchmark.

The result is not "better everywhere." It is the more precise story:

- at 16 clients, the new implementation is much faster than the mainline
  baseline for both GET and PUT;
- at 4 clients, it is effectively at parity;
- at 1 client, it remains slower by a small but visible margin.

That pattern means the remaining low-concurrency gap is probably not the Rust
parser alone. After the raw parser and direct transaction fast path, the
dominant fixed cost appears to be the Mako transaction boundary and surrounding
server dispatch, while the newer SO_REUSEPORT/thread-per-core path wins once
there is enough client concurrency to keep those workers busy.

Do not claim the current Mako-backed Redis path beats mainline in every
direction. The defensible claim is: comparable at low/mid concurrency, far
better at high concurrency, with serial preload required for stable measured
runs because parallel preload can still trigger backend `-ERR backend` aborts.

Latency matters for this comparison. The earlier `bench_resp` CSV format had
p50/p95/p99 columns but wrote zeros, so those rows were throughput-only. The
benchmark runner now measures latency around each synchronous command and fills
the percentile columns.

Treat latency-enabled rows as the better artifact when discussing end-to-end
Redis request behavior. They include measurement overhead, but they make the
comparison honest: throughput and latency come from the same run instead of
mixing Mako throughput-only rows with Redis latency rows.

The first latency-enabled Mako run shows healthy GET latency even at 16 clients
and a visible PUT tail increase at 16 clients. That supports the current
diagnosis: high-concurrency GET scales well; write contention/transaction retry
cost still shows up in PUT tail latency and remains a better next target than
more parser-only work.

When updating PR #72, keep the language deliberately careful. The pushed
fast-path commit is good evidence for high-concurrency scaling, not a blanket
victory claim. The PR body should preserve three truths together:

- the adapter is much more functional than the old/plain Redis-like path;
- the latest Mako-backed Redis path is effectively comparable around 4 clients
  and substantially faster at 16 clients;
- low-concurrency rows and PUT tail latency still point at fixed transaction
  and write-side costs that need more work.

For the Redis-vs-Mako comparison table, keep three rows groups separate instead
of blending them:

- `redis-published` is the external/their Redis result and should be treated as
  the reported reference.
- `redis-local-latency` is our rerun of the Redis benchmark shape using the
  patched latency-recording runner. It controls for the local benchmark harness,
  but it is not identical to the published environment.
- `mako-rawfast2-latency` is the optimized Mako result under the same local
  latency-recording harness.

The honest reading is that Mako wins strongly at 16-client GET/PUT throughput
against both Redis row groups, while Redis remains ahead or comparable in the
lower-concurrency rows depending on which Redis reference is used. Latency also
matters: Mako GET/16 has much lower p95/p99 than both Redis references, while
Mako PUT/16 has higher tail latency than Mako GET/16 because write-side
transaction cost is still visible.

The 32-worker/32-client run changes the scaling story in a useful way. With
`MAKO_REDIS_THREADS=32`, Mako reaches roughly 487k GET ops/s and 463k PUT ops/s
at 32 clients, with p99 still under 140 us. The local Redis 32-client rerun is
roughly 101k GET ops/s and 96k PUT ops/s, with p99 around 600 us.

The clean CPU rerun gives the more reliable saturation reading because preload
was excluded and the GET/PUT phase boundaries were selected from the stable
one-second samples. At c=32, GET uses 22.52 cores on average and PUT uses 16.84
cores on a 32-physical-core host. Neither the machine nor the full worker pool
is CPU-saturated, and I/O wait is effectively zero.

There is still a local saturation point: 24 workers received the 32 persistent
GET connections and 18 received the separately opened PUT connections. Those
workers were mostly near full CPU while the other listeners stayed idle. This
is consistent with the current architecture: every benchmark client keeps one
connection, every server worker owns a `SO_REUSEPORT` listener, and Linux hashes
connections across listeners rather than assigning one to each worker. Say
"the busy worker subset is saturated, but the server is not"; do not describe
the 32-core machine as saturated. More client connections or explicit
connection redistribution would use the idle workers better.

The initial replicated G3 run must remain recorded as a historical failure. A
real four-process Paxos fixture acknowledged 100 writes, then lost all 51 writes
that had completed before the Redis-serving `localhost` process was killed and
restarted. The learner noticed the failure and entered recovery, but the
restarted Redis endpoint opened without the acknowledged pre-fault keyspace.

At that stage, replication-enabled startup and learner election logs were not a
G3 PASS: the acknowledged-write oracle failed, and the fixture reconnected to a
restarted `localhost` endpoint rather than the elected learner. The follow-up
had to wire Redis acknowledgment to replication and route recovery to a live
promoted endpoint before rerunning the same oracle.

The follow-up G3 work completed that next step, so the historical failure must
now be reported together with the corrected PASS. The failure was not evidence
that Paxos had discarded committed data: Redis had replied before its
400-transaction serializer batch was submitted, and the test restarted an
empty fixed leader rather than using the promoted learner.

For replicated Redis, an `OK` reply is a durability promise. Keep the
replication batch at one transaction and wait for submission completion before
replying to a successful write. This cost belongs only to the replicated path;
do not slow the non-replicated performance path to simulate durability it does
not claim.

Role correctness is part of the same promise. A follower may serve replicated
reads, but it must reject local writes until the replication layer reports that
it is leader. A failover test must prove promotion with a write/read round trip,
not merely with `PING`, because a live follower socket is not a writable leader.

Do not make Redis failover depend on process uptime. Preserve the old 35-second
learner exit for non-Redis programs, but have `makoCon` identify itself so its
learner continues into the real promotion path. Preserve the delayed G3 check
so a quick test cannot hide that class of failure again.

The current defensible correctness result is: 100 acknowledged writes survived
leader death with zero missing and zero wrong values, both immediately and
after 37 seconds of cluster uptime, and clients continued on the promoted
learner. Keep the original FAIL artifact as diagnosis history and use
`mako_g3_replicated_failover_pass_result.txt` as the corrected result.

Performance checks after correctness work must exercise the non-replicated
path separately, because replicated durability intentionally adds submission
waiting that the fast local path does not claim. Keep key count, value size,
preload order, client count, duration, server worker count, and benchmark
binary fixed when comparing before and after results.

The post-G3 runs support the intended boundary: non-replicated throughput and
latency were maintained. Treat a single tail-latency spike as a signal to
repeat the exact case on a fresh server, not immediately as a regression. The
fresh 32-client repeat improved throughput and every latency percentile over
the pre-G3 baseline, showing that the first GET p95/p99 increase was run
variance rather than a repeatable effect of the correctness changes.

For Raft request routing, prefer a callback-fed atomic local-role cache over a
mutex or repeated leader-view lookup on every transaction. The cache is only a
fast rejection hint: leadership can change immediately after any pre-check, so
the Raft append must validate leadership and term under its own lock and return
an acceptance/commit result to the Redis acknowledgement path. Two-phase commit
coordinates transaction participants; it does not replace this consensus-role
validation.

For PR #72, keep the G3 follow-up deliberately narrow. The checked-in G3 oracle
uses the Paxos fixture and already passes acknowledged-write survival; the only
remaining lifetime bug was the learner's legacy 35-second `quick_exit()`.
Identify `makoCon` at startup and skip that cutoff only for Redis, while
preserving old behavior for every other program. Raft per-entry completion is
a separate future change and must not be bundled into this Redis PR.

When reporting server-worker CPU, use per-thread samples and express process
usage as cores, not one process percentage. Also report how many request
workers carried substantial load: 32 persistent clients are hashed among 32
`SO_REUSEPORT` listeners, so idle listeners can coexist with busy workers.

The current-commit rerun used 16.47 average cores for GET and 19.74 for PUT,
with p95 usage of 19.02 and 20.99 cores. Only 20 GET workers and 22 PUT workers
averaged at least 70% CPU. The defensible conclusion is therefore that neither
the 32-core machine nor the complete worker pool was saturated; connection
placement and the occupied worker subset still shape the result.

Use a scaling sweep, not one 32-client sample, to explain worker use. With the
server fixed at 32 workers, the 1/2/4/8/16/32-client sweep occupied
1/2/4/7/12/22 workers for GET and 1/1/3/7/13/21 for PUT. Throughput remained
close to linear through 16 clients, while per-client throughput fell at 32 as
the occupied listener workers approached saturation and other listeners stayed
idle. This supports connection placement as the limiting factor without
claiming that the whole server or machine is CPU-saturated.

## How To Treat Shared-Listener Distribution And Worker CPU

The old `SO_REUSEPORT` measurements above are historical evidence for why the
listener design changed. Linux hashes persistent connections across listener
sockets; it does not guarantee one connection per worker. That made a busy
subset look saturated while request workers elsewhere in the same process were
idle.

Use one shared nonblocking listener and deterministic round-robin accept turns.
The required invariants are:

- reject a zero-worker configuration before creating a barrier;
- assign the first N persistent connections to N different workers;
- keep each accepted connection owned by one worker for its lifetime;
- let every worker service all of its owned nonblocking clients; and
- measure helper-thread CPU separately from request-worker CPU.

Do not restore per-worker `SO_REUSEPORT` listeners merely because they remove
accept coordination. The deterministic assignment fixes the observed workload
imbalance and makes worker-count experiments reproducible. If connection
migration is needed later, design and test it explicitly instead of returning
to kernel hashing.

Shared connection ownership creates a wakeup obligation. Queueing a Pub/Sub
message is not socket readability, so the subscriber worker can otherwise
sleep forever in `poll()`. Each worker therefore owns a private nonblocking wake
socket. A Pub/Sub registry target keeps a weak wake handle, queues the reply,
and notifies the owning worker. Ignoring a full wake socket is safe because an
unread byte already guarantees a pending wake; periodic short poll timeouts are
not an acceptable substitute because they add idle CPU and delivery latency.

Protocol negotiation controls reply types. Under RESP3, `HGETALL` must be a map
and sorted-set scores must be doubles; under RESP2 the same commands keep their
legacy array/bulk-string shapes. Validate protocol-shape changes against a
reference Redis with the same client version before changing the adapter.

Performance counters must include both dispatch paths. The raw Mako `GET`/`SET`
fast path bypasses general command dispatch, so it must increment
`total_commands_processed` explicitly. `CONFIG RESETSTAT` must reset the same
counter exposed by `INFO stats`; otherwise timed throughput samples silently
undercount the optimized path.

For CPU claims, distinguish a client-generator ceiling from a server ceiling.
One `redis-benchmark` thread plateaued around 52k GET ops/s after four
connections even though all requested server workers were active. Repeating
with `--threads` equal to the persistent connection count scaled from about
16k GET ops/s at one client to 496k at 32 clients. The 32-client server used
about 19.09 cores, and every sweep point activated exactly as many request
workers as persistent clients. The defensible conclusion is that deterministic
distribution works and the 32-worker server was not CPU-saturated; a
single-thread benchmark plateau is not evidence of Mako worker saturation.

## Compatibility Before Feature Expansion

Redis compatibility is the current deliverable. Do not widen the command
surface with vector-specific behavior while known in-scope compatibility gaps
remain. Adding another feature family would make failures harder to attribute
and would weaken confidence in the connection, transaction, and response
foundations that every later command depends on.

Treat the compatibility layer as solid only when:

- RESP2 and RESP3 reply shapes are correct for the claimed commands;
- command semantics pass the focused suite and the scoped Redis/client tests;
- persistent connections, pipelining, transactions, blocking commands, and
  cross-worker Pub/Sub behave correctly under concurrency;
- Mako-backed reads, writes, retries, expiry, and collection operations preserve
  their documented transaction invariants;
- fuzz, soak, liveness, and resource-cleanup guards have run, with every `N/A`
  or skip tied to a concrete external-fixture requirement or reviewed scope
  decision; and
- benchmark results distinguish client limits, request-worker CPU, helper CPU,
  backend contention, and whole-machine saturation.

Only after that gate is met should vector work begin as a separate phase with
its own Redis command contract and correctness evidence. Compatibility failures
found during that later phase still take priority over feature expansion.
