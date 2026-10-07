# Phase 3 plan: remaining adapter-scope Redis commands

Goal: close the 46 commands that Dragonfly supports and this adapter does not,
all of which can be built without changes below `makoCon`. Packages run
sequentially on branch `redis-compat-phase3`, one commit each, each gated by
`cargo test`, `test_phase2_commands.py`, the compat pytest suite, and the
Redis TCL semantic guard before the next package starts. Read
`PHASE3_CONTRIBUTING.md` first.

Baseline at branch start: 181 of 249 top-level Redis 7.4 commands recognized.
After package 2: 185 of 249 recognized.
After package 3: 194 of 249 recognized.
After package 4: 203 of 249 recognized.
After package 5: 206 of 249 recognized.
After package 7: 207 of 249 recognized.
After package 6: 208 of 249 recognized.
After package 8: 211 of 249 recognized. The three new names are EVALSHA,
EVAL_RO and EVALSHA_RO; EVAL and SCRIPT were already counted, because both were
recognized as stubs before this package replaced them with a real interpreter.
FUNCTION and FCALL are not part of this package and are not counted.
After package 9: 226 of 249 recognized.
After package 10: 228 of 249 recognized.

| # | Package | Commands | Where the work is | Acceptance |
|---|---|---|---|---|
| 1 | HyperLogLog | PFADD, PFCOUNT, PFMERGE (3) | New C++ ops `TXN_OP_HLL_ADD/COUNT/MERGE` (read-modify-write must be one op); Rust parse/build/reply. Private register format stored as a string value with a magic header so TYPE, DUMP, DEL, EXPIRE work unchanged. Ertl estimator as in Redis `hllCount`. | Redis doc example `PFADD hll a b c d e f g` → `PFCOUNT 7`; 10k uniques within 1.5%; multi-key PFCOUNT is a union; PFMERGE; `WRONGTYPE Key is not a valid HyperLogLog string value.` on a plain string; standard WRONGTYPE on a list. |
| 2 | BITFIELD | BITFIELD (1) | New C++ op `TXN_OP_BITFIELD` with GET/SET/INCRBY and OVERFLOW WRAP/SAT/FAIL, reusing the Rust encoding/offset parsers from BITFIELD_RO. | Redis doc examples (`BITFIELD mykey INCRBY i5 100 1 GET u4 0` → `[1, 0]`), SAT and FAIL overflow cases, WRONGTYPE on a list. |
| 3 | Geo | GEOADD, GEOPOS, GEODIST, GEOHASH, GEOSEARCH, GEORADIUS, GEORADIUSBYMEMBER, GEORADIUS_RO, GEORADIUSBYMEMBER_RO (9) | Rust only, on top of the existing sorted-set ops: 52-bit geohash as the score (Redis `geohash.c` interleave), GEOADD → ZADD, GEOPOS/GEODIST/GEOHASH → ZMSCORE + decode, searches → the 9 neighbor-box score ranges as ZRANGEBYSCORE ops in one request, then exact distance filter, sort, COUNT/ANY/WITHCOORD/WITHDIST/WITHHASH in Rust. GEOSEARCHSTORE and GEORADIUS[BYMEMBER] STORE/STOREDIST deferred to package 10 (an earlier note here said Dragonfly lacks them; that was wrong — Dragonfly supports GEOSEARCHSTORE in full, so they were always in scope). | Redis doc examples for GEOADD/GEODIST/GEORADIUS (Palermo/Catania), GEOHASH strings match Redis (`sqc8b49rny0`), GEOPOS precision within 1e-4 degrees, BYBOX and BYRADIUS, units m/km/mi/ft. |
| 4 | Hash field expiry | HEXPIRE, HPEXPIRE, HEXPIREAT, HPEXPIREAT, HTTL, HPTTL, HPERSIST, HEXPIRETIME, HPEXPIRETIME (9) | C++: per-field TTL side keys next to the hash field keys (same scheme as key TTL meta), lazy expiry checks in every hash read path (HGET, HMGET, HGETALL, HEXISTS, HLEN, HKEYS, HVALS, HSTRLEN, HSCAN, HRANDFIELD), field deletion of expired fields on access, NX/XX/GT/LT conditions and the per-field reply codes (-2, 0, 1, 2). Rust parse/build/reply with `FIELDS numfields` syntax. | Redis 7.4 doc examples; a field expires and disappears from HGETALL/HLEN; HPERSIST; conditions; HEXPIRE on a missing field → -2; on a non-hash → WRONGTYPE. |
| 5 | CLUSTER shims | CLUSTER INFO/SLOTS/SHARDS/NODES/MYID/KEYSLOT, READONLY, READWRITE (3 top-level) | Rust only: single-node emulation. INFO reports `cluster_enabled:0` style fields Dragonfly uses in emulated mode, SLOTS/SHARDS/NODES describe one node owning slots 0-16383 at the advertised host:port, KEYSLOT is CRC16 mod 16384 with hash-tag support, READONLY/READWRITE return OK. | redis-cli `--cluster` style probes succeed; KEYSLOT matches Redis for `{tag}` keys. Record in known_divergences that this is emulation. |
| 6 | Logical databases | SELECT n (n>0), MOVE (1 + behavior) | Rust: per-connection selected db; for db>0 prefix every user key with a hidden `\x02<db>:` marker before it reaches storage and strip it in every reply that returns key names (KEYS, SCAN, RANDOMKEY, blocking pops, SORT with STORE names, DUMP payload keys are values not names). KEYS/SCAN/DBSIZE/FLUSHDB scope to the selected db via the scan prefix; FLUSHALL clears all. MOVE = read+write+delete in one request. CONFIG GET databases → 16. WATCH keys must include the prefix. | SELECT 1; SET; SELECT 0; GET → nil; KEYS per db; DBSIZE per db; FLUSHDB only clears current db; MOVE semantics incl. returning 0 when dest exists; pytest/TCL still pass with `--singledb`. |
| 7 | MONITOR | MONITOR (1) | Rust: a global monitor registry (like Pub/Sub), every worker formats `+<ts> [0 addr] "CMD" "arg"...` for each executed command and enqueues to monitor clients; while any monitor is attached the raw GET/SET fast frame path must fall through to the general parser so it is observed. QUIT/RESET leave monitor mode. | Two connections: MONITOR then commands appear in Redis format; disabling restores the fast path (INFO commandstats still counts). Document as single-process. |
| 8 | Scripting | EVAL, EVALSHA, EVAL_RO, EVALSHA_RO, SCRIPT LOAD/EXISTS/FLUSH (4 top-level) | Largest change: add an interactive transaction FFI (`cpp_txn_begin/op/commit/abort`) beside the batch call so a script's `redis.call` results can feed later calls inside one Mako transaction, with the whole script re-run on OCC abort (bounded retries, same as batch). Embed Lua 5.1-compatible interpreter (`mlua` with vendored `lua51`), implement `redis.call/pcall`, `redis.error_reply/status_reply`, `redis.sha1hex`, `redis.log`, `cjson` minimal, KEYS/ARGV, error and type conversion per Redis rules, script cache by SHA1, SCRIPT FLUSH, `NOSCRIPT`. Replace the current stub. Keep MULTI/EXEC on the batch path. | Redis doc examples; a read-then-write script is atomic under concurrent writers (torn-read stress like `run_fast_set_atomic_stress.py`); EVALSHA after SCRIPT LOAD; NOSCRIPT error; scripts calling unsupported commands get a clean error; TCL `unit/scripting` file added to the scope and passing or explicitly skipped per test with reasons. |
| 9 | Streams | XADD, XRANGE, XREVRANGE, XLEN, XDEL, XTRIM, XREAD, XGROUP, XREADGROUP, XACK, XPENDING, XCLAIM, XAUTOCLAIM, XINFO, XSETID (15) | C++: new composite-key family (stream meta key with last-id/length/first-id, one entry key per `ms-seq` ID with a packed field list, per-group meta and PEL keys). Rust: ID parsing (`*`, `ms-*`, `-`/`+`, exclusive `(`), MAXLEN/MINID trimming with `~`, COUNT/BLOCK, `$`/`>` semantics, consumer creation, PEL bookkeeping, XINFO shapes. Blocking XREAD/XREADGROUP reuse the blocked-client retry machinery used by BLPOP. | Redis doc examples for each command; consumer-group flow (XGROUP CREATE, XREADGROUP >, XACK, XPENDING, XCLAIM); blocking XREAD wakes on XADD from another connection; TCL `unit/type/stream` and `stream-cgroups` files added to scope with per-test skips documented. |
| 10 | Geo stores and MODULE | GEOSEARCHSTORE, MODULE (2), plus `STORE`/`STOREDIST` on GEORADIUS and GEORADIUSBYMEMBER | Rust only. The storing searches run on the package-8 interactive session, because their write is decided by their own reads: one open transaction holds the FROMMEMBER lookup, the nine neighbor-box range reads, the destination DEL and the ZADD of the matches, retried as a whole on an OCC abort. A MULTI that queues one runs its whole queue on that same path. MODULE is a local Rust arm with no modules to list. | The Redis doc example (`GEOSEARCHSTORE key2 Sicily FROMLONLAT 15 37 BYBOX 400 400 km ASC COUNT 3 STOREDIST` → 2 with distances 56.4413 and 190.4424 km); geohash scores that GEOPOS decodes back within 1e-4 degrees; a destination of another type replaced; zero matches deleting the destination and answering 0; WRONGTYPE on a string source; the STORE/WITH* and _RO error texts; GEOSEARCHSTORE inside MULTI/EXEC and in database 1; MODULE LIST/LOAD/LOADEX/UNLOAD/HELP. Redis's own tests/unit/geo.tcl run as a non-gating check. |

Ordering rationale: 1-3 are self-contained and unblock the most common client
libraries; 4 completes the hash family for Redis 7.4; 5-7 are compatibility
plumbing that some tools require to connect at all; 8 changes the FFI shape
and is the riskiest; 9 is the largest and benefits from 8's interactive FFI
only marginally, so it can also go before 8 if scheduling requires.

Tracking: each package's commit message must list commands added, deviations
recorded, and the four gate results. This file is updated with the resulting
recognized-command count after every package.
