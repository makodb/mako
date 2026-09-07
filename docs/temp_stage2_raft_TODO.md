# Raft Stage 2 — preparation TODO

**Status:** working notes, 2026-09-06. Supersedes the deleted
`docs/raft-stage2-shapes.txt`. Companion to `docs/raft-dsl-examples.txt`
(current Stage-1 shapes) and `docs/raft-rpc-flow.txt`.

Everything below marked **[measured]** was verified against this tree with real
`clang -E` runs, probe compiles using `build/compile_commands.json` flags, and
`llvm-nm` on the built objects. Everything marked **[proposed]** is a plan, not
tested code.

---

## 0. Why this document exists

The ordering agreed so far is: convert one small value type, add one `impl`,
stand up the crate, then whole objects. That ordering is sound. But it walks
straight through `src/deptran/constants.h`, and there is a real defect there
that must be fixed **before** any raft type that names those aliases moves.

---

## 1. THE DEFECT — an ODR violation that exists today [measured]

`src/deptran/constants.h` defines 29 type aliases as **preprocessor macros**:

```c
constants.h:6    #define ballot_t int64_t     // SIGNED
constants.h:18   #define siteid_t uint16_t
constants.h:19   #define slotid_t uint64_t
... 26 more ...
```

Two raft headers redeclare two of them as **guarded fallbacks with the opposite
signedness**, and neither header includes `constants.h`:

```cpp
// src/deptran/raft/log_storage.hpp:38-43
#ifndef slotid_t
using slotid_t = uint64_t;
#endif
#ifndef ballot_t
using ballot_t = uint64_t;      // UNSIGNED — constants.h says int64_t
#endif

// src/deptran/raft/snapshot_manager.hpp:32-37   (same pattern)
```

So `ballot_t` means `int64_t` or `uint64_t` **depending on include order**.
Both of these headers contain Rust-DSL blocks
(`raft_log_entry.scalar_decisions`, `raft_snapshot.metadata_decisions`).

### It fires today [measured]

Of the 18 translation units in `build/compile_commands.json` that transitively
include either header, **16 see `constants.h` first** (signed) and **2 do not**:

| TU | constants.h | log_storage.hpp | result |
|---|---|---|---|
| `tests/legacy_raft_log_test.cc` | line 160777 | line **99874** | `ballot_t = uint64_t` |
| `tests/raft_memory_snapshot_manager_test.cc` | **never included** | — | `ballot_t = uint64_t` |

Cause for the first one is a two-line accident: `legacy_raft_log_test.cc:8`
includes `log_storage.hpp`, and `:9` includes `tpc_command.h` which reaches
`constants.h` via `__dep__.h:120`. **Swapping source lines 8 and 9 silently
changes `LogEntry::term` from `uint64_t` to `int64_t`.** The build uses `-w`, so
nothing warns either way.

Two more TUs would also fire but are not currently built (no CMake target):
`src/rrr/tests/rpc_log_storage_test.cc`, `src/rrr/tests/rpc_rocksdb_log_storage_test.cc`.

### The consequence is a live ODR violation [measured]

```
$ llvm-nm build/CMakeFiles/txlog_core_obj.dir/src/deptran/raft/server.cc.o
0000000000000000 W _ZN5janus4raft8LogEntry4loadERN3rrrW3rrrW12serializable17BinaryReadArchiveE

$ llvm-nm build/CMakeFiles/test_legacy_raft_log.dir/tests/legacy_raft_log_test.cc.o
0000000000000000 W _ZN5janus4raft8LogEntry4loadERN3rrrW3rrrW12serializable17BinaryReadArchiveE

$ llvm-nm build/test_legacy_raft_log | grep -c <that symbol>
1
```

The same weak comdat symbol is defined in two objects **compiled against
different field types**, both are linked into `build/test_legacy_raft_log`, and
the linker keeps exactly one. The mangling does not encode member types, so
nothing detects it.

### Why it has not bitten yet [measured]

1. `i64` and `u64` are layout-identical.
2. The rrr serializers for both are raw 8-byte memcpy
   (`src/rrr/misc/serializable.rs:289` and `:325`).
3. The DSL blocks in both headers are written in plain `uint64_t`/`bool`/`int8_t`
   and **never mention the alias**, so the generated code is byte-identical
   under either include order.

One divergence *was* demonstrated: `SnapshotMetadata::to_string`
(`snapshot_manager.hpp:74`) relocates to `std::to_string(unsigned long)` in one
order and `std::to_string(long)` in the other. That symbol is not currently
emitted into any linked archive, so the divergence is unrealised.

**Verdict: latent, not live.** It is a correctness landmine, not a present
miscompile — but it is exactly the kind of thing that becomes live the moment a
type moves to Rust, because Rust will force one signedness and make it real.

---

## 2. THE METHOD — five options, costed

| Option | What it fixes | What it risks |
|---|---|---|
| **A** — do nothing, keep writing `i64`/`u64` in DSL blocks | nothing | ODR stays; every conversion re-litigates signedness |
| **B** — `#define` → `using` in `constants.h` | makes aliases real types, mappable | `key_t` collides with POSIX; 29 aliases × all of deptran |
| **C** — delete the guarded fallbacks; make the two headers include `constants.h` | kills the ODR violation outright | smallest possible change; may expose latent signed/unsigned warnings |
| **D** — strong newtypes (`struct Ballot(i64)`) | real type safety | very large churn; not behaviour-preserving |
| **E** — type-map entry once the crate exists | lets Rust name the alias | macros already expanded; **does not work for `#define`** |

**Option E does not work and should not be attempted.** A type map maps *names*;
by the time the C++ compiler sees the code the macro has expanded, so there is
no name left to map. This is why the aliases must be fixed in C++ **before**
Stage 2, not during it.

### Recommended sequence

**Step 1 — fix the ODR violation (Option C). Do this first, on its own.**

Delete the four guarded blocks in `log_storage.hpp:38-43` and
`snapshot_manager.hpp:32-37`, and add `#include "../constants.h"` to both.
This is the smallest change that makes `ballot_t` mean one thing everywhere.

Verify:
- `llvm-nm` the two objects again; confirm the `LogEntry::load` bodies now agree.
- Re-run the 18 TUs through `clang -E` and confirm zero occurrences of
  `using ballot_t = uint64_t;`.
- `./build/...` full build, then all 12 raft ctest targets, then both RaftLab
  arms (`MAKO_RAFT_PERSISTENCE` unset, then `=1`).

Do **not** proceed until the preprocessed output is uniform across all 18 TUs.

**Step 2 — decide signedness deliberately, and write it down.**

`ballot_t` is signed today in production. The already-DSL-owned `RaftData` at
`read_raft_disk.cc:57` spells the same fields `u64`. One of them is wrong.
Establish which by finding whether a negative ballot can occur (check
initialisation, increment, and any sentinel), then record the answer in
`constants.h` as a comment. Every later conversion inherits this decision.

**Step 3 — `#define` → `using`, one alias at a time (Option B), only for the
aliases raft actually uses.**

Not all 29. Survey which appear in `src/deptran/raft` first. For each:
- convert the single line in `constants.h`
- full build + full raft test matrix
- diff generated `rcc_rpc.h` — **`bool_t` is on the wire** (`rcc_rpc.rpc` uses it),
  so any change to it must leave the generated header byte-identical

Handle **`key_t` (`constants.h:27`) last and separately** — POSIX
`<sys/types.h>` also defines `key_t`, and the macro currently wins silently
wherever both are included. Converting it to a `using` turns a silent override
into a hard conflict.

**Step 4 — only now start the conversion ordering.**

1. `RaftSubmissionProgress` as a struct (Stage-1 legal today)
2. `impl RaftStartResult` (needs the crate)
3. stand up the raft crate — `Cargo.toml`, manifest, `--extern rusty`
4. whole objects: `CRC32`, then storage/snapshot value types, then `RaftServer`

---

## 3. Invariants for the whole exercise

- **One concern per commit.** A signedness fix and a DSL ownership change must
  never ship together — `docs/migration/rustycpp/raft-rust-migration.md`
  invariant 3 already requires this.
- **The preprocessor is the oracle.** For anything alias-related, `clang -E`
  with real `compile_commands.json` flags beats reading the header.
- **Watch the wire.** `bool_t` and the `Rpc*Request` types are wire-visible;
  regenerate and diff `rcc_rpc.h` rather than assuming.
- **`scripts/raft_dsl.sh --check` must stay green at every step**, and after any
  edit to a `#if RUSTYCPP_RUST` block run `scripts/raft_dsl.sh --rewrite` rather
  than hand-editing the generated C++.

## 4. Open questions

- Can a ballot legitimately be negative? (blocks Step 2)
- Are `src/rrr/tests/rpc_log_storage_test.cc` and
  `rpc_rocksdb_log_storage_test.cc` meant to be built? They are referenced by no
  CMakeLists and would fire the unsigned branch if enabled.
- Does `rrr`'s `src/rrr/base/basetypes.rs` provide a pattern worth copying for
  the raft aliases, or is it solving a different problem?
