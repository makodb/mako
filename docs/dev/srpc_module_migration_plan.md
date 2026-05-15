# SRPC (rrr) C++23 Module Migration — Incremental Plan

## Background

The rrr library was migrated to C++23 named modules in commit `0cf5b972`
(Apr 21, 2026) and de-modularized one week later in `382bb74d` (Apr 27, 2026)
plus the sweep in `9829fa77`. The revert was driven by:

- 60 PCM files totaling ~1.6 GB on-disk per parallel build.
- ~30 minute clean build at `-j32`.
- clang21+ ODR clash on `<emmintrin.h>` reaching both the rrr GMF and
  consumer TUs ("definition with same mangled name").
- rusty-cpp borrow checker unable to resolve `import rrr;` without BMI paths.

## Hypothesis for this retry

The prior attempt was a big-bang conversion of ~80 files in a single
commit. There is no per-file signal in the history showing which
conversions inflated PCMs or wallclock. Going one file at a time with
measurement at each step will either validate that modules are inherently
expensive in this codebase (abort) or expose specific files that bloat
(fix or skip them).

## Approach

### Principles

1. **Top-level module per file, file extensions preserved**: each
   `.hpp`/`.cpp` pair becomes a single module interface unit named
   `rrr.foo` (top-level module, not a `rrr:foo` partition of a single
   umbrella module). The `.cpp` is **rewritten in place** to contain
   `export module rrr.foo;` plus the definitions — extension stays
   `.cpp` (CMake's `FILE_SET CXX_MODULES` doesn't require `.cppm`).
   For header-only files like `cpuinfo.hpp`, a new sibling `.cpp` is
   created to host the module declaration; the original `.hpp` stays
   as the shim. Top-level naming keeps each BMI's transitive footprint
   independent and lets consumers `import rrr.foo;` directly later.
2. **Header retained as forward-decl shim**: the original
   `base/foo.hpp` stays intact (declarations + macros). Consumers
   continue to `#include "foo.hpp"` and the linker resolves to the
   module-emitted `.o`. No consumer churn until a later optional pass
   flips them to `import rrr.foo;`.
3. **CMakeLists wiring**: `RRR_MODULE_SRC` is an explicit list of
   converted files (not a glob); the regular source list is
   `REMOVE_ITEM`-stripped of those entries to avoid double-compile.
   `target_sources(rrr PUBLIC FILE_SET rrr_modules TYPE CXX_MODULES
   FILES ${RRR_MODULE_SRC})` adds them.
3. **Measure every commit**: wallclock clean build, total `.pcm`
   bytes, and `ninja -d stats` per-TU compile times. Any single-file
   conversion that adds >5% wallclock or >50 MB PCM is a red flag —
   stop and investigate before continuing.
4. **Bottom-up**: leaves with no rrr dependencies first; climb up the
   dependency graph.

### Measurement protocol

Before and after each conversion commit:

```
rm -rf build && \
  time cmake -G Ninja -B build -S . -DCMAKE_BUILD_TYPE=Release 2>&1 | tee cfg.log && \
  time ninja -C build -j32 2>&1 | tee build.log

du -cb build/**/*.pcm 2>/dev/null | tail -1
find build -name '*.pcm' | wc -l
ninja -C build -d stats > ninja_stats.txt
```

Record the numbers in the table at the bottom of this doc (CSVs are
gitignored project-wide; the table lives inline so it commits cleanly).

Decision rule per commit:
- ≤ +5% wallclock and ≤ +50 MB PCM → proceed.
- > +5% wallclock or > +50 MB PCM → stop, diagnose, fix or revert.
- Super-linear growth across 3 consecutive conversions → abort migration.

### Conversion order (planned, leaves first)

1. `base/strop.hpp` + `.cpp` — pure string ops, no rrr deps.
2. `misc/cpuinfo.hpp` — header-only.
3. `rpc/errors.hpp` — leaf in rpc/.
4. `rpc/request_options.hpp` — leaf.
5. `base/basetypes.hpp` + `.cpp` — depends on logging; starts climbing.
6. (Continue up the dependency graph; re-plan after step 5.)

After ~5–10 conversions we should have a strong signal on whether to
continue.

### Failure-mode watchlist

- **Heavyweight GMF**: anything pulling `<emmintrin.h>` / `<immintrin.h>`
  / rusty-cpp headers into a module GMF risks reproducing the clang21
  SIMD clash. `RUSTY_PORTABLE_INTRINSICS=1` is project-wide. Verify
  before any rusty-using partition.
- **Transitive `export import` chains**: if every partition re-exports
  its dependencies, BMIs balloon. Prefer plain `import` (not exported)
  where the symbols aren't part of the partition's public surface.
- **`import std;` cost**: importing `std` from every partition is
  cheap once `std.pcm` exists — but verify the per-partition BMI size
  stays reasonable.

## Out of scope / deferred

- **Borrow checking**: stays disabled for rrr during the migration
  (CMakeLists.txt:104–107). Track as follow-up.
- **External consumer migration**: 39 files do
  `#include "rrr/rrr.hpp"`. With the shim approach they don't need to
  change. Flipping to `import rrr;` is an optional later cleanup.
- **`RPC_TEST_HOOKS`** (CMakeLists.txt:191–194): per-TU define no
  longer works once consumers go modular. Decide when the first file
  touching it is converted.

## References

- Prior modularization commit: `0cf5b972` (`git show 0cf5b972`).
- Revert commit: `382bb74d`.
- Sweep: `9829fa77`.
- CMakeLists.txt:104–107 (borrow-check disable note).
- CMakeLists.txt:191–194 (RPC_TEST_HOOKS).

## Metrics

Targets built: `rrr` + `rpcbench`. `-j32`. Clean build each row (`rm -rf
build && cmake -G Ninja -B build ... && ninja rrr rpcbench`).

| Step | Wallclock (s) | PCM count | PCM total (MB) | librrr.a (MB) | Notes |
|------|--------------:|----------:|---------------:|---------------:|-------|
| baseline | 16.86 | 2 | 28.5 | 10.07 | clang 19, cmake 3.31 |
| base/strop | 15.63 | 3 | 53.0 | 10.02 | +24.5 MB PCM (rusty.hpp + import std). 0 importers via shim. Wallclock within noise. File ext kept as .cpp. |

