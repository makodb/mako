
# Profiling

Mako uses the CMake-built `dbtest`, `simplePaxos`, and `simpleRaft` binaries.
The former Waf/`run.py`/`deptran_server` benchmark workflow is retired.

Build optimized binaries with debug symbols:

```shell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target dbtest simplePaxos simpleRaft
```

For a local workload, wrap the normal `dbtest` invocation with Linux `perf`:

```shell
perf record --call-graph dwarf -- ./build/dbtest <dbtest arguments>
perf report
```

For a distributed run started by a current script, attach `perf` to the
specific `dbtest` process instead. Keep the workload's usual shutdown and
cleanup behavior intact.

# Heap profiling

Configure a separate tcmalloc build, then run the normal workload with a heap
profile prefix:

```shell
cmake -S . -B build-tcmalloc -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DUSE_MALLOC_MODE=2
cmake --build build-tcmalloc --target dbtest
HEAPPROFILE=/tmp/mako-heap ./build-tcmalloc/dbtest <dbtest arguments>
```

Inspect a generated heap snapshot with the system `pprof` or the vendored
`scripts/pprof` helper. Heap profiling can substantially slow a workload.

# Plotting benchmark results

Aggregate run output:
```
python3 scripts/aggregate_run_output.py --prefix multi_dc_tpcc_6 *yml
```

Replace `multi_dc_tpcc_6` with the correct prefix, then generate graphs:
```
scripts/make_graphs '*csv' . .
```
