# Phase 1F mutation gate

This directory contains an external mutation runner for `mako-cache`. It does
not add runtime mutation flags, `cfg` branches, or observer calls to the
production transaction path. Every baseline and mutant is built from a fresh
copy in a temporary Cargo workspace with its own initially empty
`CARGO_TARGET_DIR`.

The complete workspace `Cargo.lock` is copied first. Cargo's lock model stores
dev-dependency edges for all workspace members, so an offline `cargo metadata`
pass then prunes only edges/packages absent from the one-member copied
workspace. Every compilation and test uses `--locked` after that normalization,
and the report records both lockfile hashes and proves metadata did not create
the target directory.

The gate is deliberately stricter than “the suite exited nonzero.” A mutant
counts as killed only when all of the following are true:

1. its exact source anchor occurs once and has the reviewed SHA256 digest;
2. the replacement occurs once in the isolated copy and the source hash
   changes;
3. the mutant compiles successfully before any test is run;
4. one designated test (selected with libtest `--exact`) executes;
5. that test fails with its expected `test ... FAILED` signature; and
6. the original crate's complete manifest/build/source/test hash tree is
   unchanged at the end.

A compile failure, timeout, zero-test command, loader error, unrelated test
failure, changed/missing anchor, or source change is a harness error. A passing
designated test means the mutant survived and fails the gate. The JSON report
contains command arrays, output hashes/tails, anchor and file hashes, kill
signatures, and the original-source integrity proof.

## Commands

From the repository root, check every anchor and exercise the isolated-copy
and path-rewrite setup without compiling:

```bash
python3 crates/mako-cache/mutations/run.py --check
```

List contracts or check/run a subset by name:

```bash
python3 crates/mako-cache/mutations/run.py --list
python3 crates/mako-cache/mutations/run.py --check missing-ready-publication partial-replay
python3 crates/mako-cache/mutations/run.py early-detached-capacity-discharge
```

The full matrix includes native recovery and the exact timestamp observer, so
it requires a hook-enabled archive. Pass the CMake *build directory* that owns
`libmako.a`, `CMakeCache.txt`, and its companion libraries:

```bash
python3 crates/mako-cache/mutations/run.py \
  --hook-build-dir "$(pwd)/build_item4_hooks" \
  --report /tmp/mako-cache-phase1f-mutations.json
```

`MAKO_CACHE_HOOK_BUILD_DIR` is the environment equivalent. It is kept
separate from `MAKO_BUILD_DIR` so the timestamp kill cannot silently run
against a production archive lacking `MAKO_LOCAL_TEST_HOOKS=ON`. For a subset
that needs native Mako but not hooks, use `--mako-build-dir` or the existing
`MAKO_BUILD_DIR` environment variable. The runner sets
`MAKO_LOCAL_REQUIRE_NATIVE=1`; missing or fingerprint-mismatched native code is
therefore a hard error rather than a skipped test. `ROCKSDB_LIB_DIR` and
`LIBCXX_DIR`, when needed by the local environment, are inherited unchanged.
For a pure-Rust subset with no native build argument, it explicitly selects
`MAKO_LOCAL_FAKE_ABI=1` so `mako-local` cannot accidentally auto-discover and
link an unrelated or stale default archive from the source worktree.

Progress goes to stderr and the authoritative JSON report goes to stdout.
`--report` writes the same JSON to a file. Use `--keep-workdirs` only when
debugging; otherwise every copied source/target directory is removed eagerly.
Workspaces default to
`${XDG_CACHE_HOME:-~/.cache}/mako-cache-mutations`, not `/tmp`; override that
with `--temp-root` or `MAKO_CACHE_MUTATION_TEMP_ROOT` when a larger filesystem
is preferable.

## Mutants and scope

The table in `run.py` covers corrupted native-record put replay, early detached
capacity discharge, hook-time allocation, a conflict cancellation CacheSeq
gap, missing and premature Ready publication, an unpinned unknown outcome,
partial/reordered/duplicate recovery replay, a wrong Mako timestamp, and a
missing recovered-clock floor. Tests and anchors are intentionally adjacent in
one reviewed table so a renamed or strengthened Phase 1F test is easy to
re-anchor; `--check` refuses to skip an obsolete row.

“Applied” in this milestone means the asynchronous worker's atomic RocksDB
`WriteBatch` returned successfully and the in-memory applied watermark then
advanced. It does **not** mean RocksDB's WAL was synchronously forced to disk.
This gate makes no claim about an unflushed log tail, torn/internal Rocks WAL,
forced sync, or SIGKILL inside RocksDB. Those are later durability milestones;
the present kill contracts cover cache sequencing, publication, quarantine,
timestamp carriage, replay, and application correctness.
