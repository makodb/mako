# Rust STO sustained-load and memory-pressure qualification, 2026-09-11

The production audit of [PR #91](https://github.com/makodb/mako/pull/91) accepted
the retained 180-second growth run but asked for sustained concurrent churn and
memory-pressure evidence before a release decision, because a three-minute run
does not establish long-running stability. This directory records that evidence.

Every run here used the release `sto_tpcc_bench` built from source
`ba9905533` (SHA-256
`996e79d0edc5ff1a59ad226e977e7cb0403944167e23852ce55a93dfad97d868`), the closed
Rust TPC-C bridge, a local non-replicated shard (`config/mako_sto_tpcc_local.yml`,
`local_s0`), four warehouses, and four worker threads. The host is `zoo-005`
(62 GiB RAM). The build is not PGO-optimized, so the throughput figures below
characterize behaviour on this host and are not a performance claim.

Each run was driven through the same validator the CTest gates use:

```sh
MAKO_TPCC_ALLOCATOR_MEMORY=<bytes> \
MAKO_STO_TPCC_REGISTRY_MEMORY=<bytes> \
MAKO_TPCC_WORKLOAD_MIX=<NewOrder,Payment,Delivery,OrderStatus,StockLevel> \
python3 scripts/ci/run_sto_tpcc_ctest.py <expectations> -- \
  sto_tpcc_bench --num-threads 4 \
    --shard-config config/mako_sto_tpcc_local.yml --site-name local_s0 \
    --runtime 600 --storage-engine rust --slow-exit
```

## Results

| Run | Mix | Registry budget | Outcome |
| --- | --- | --- | --- |
| `run-4w-600s-stocklevel-only.log` | StockLevel only | 8 GiB | Completed: 69,064,964 commits, 0 aborts, 606.1 s, 113,943 txn/s |
| `run-4w-600s-standard-mix.log` | TPC-C default | 8 GiB | Budget exhausted at ~81 s, reported, exit 3 |
| `run-4w-600s-churn10.log` | 10 NewOrder / 90 Payment | 32 GiB | Budget exhausted at ~447 s, reported, exit 3 |
| `run-4w-registry-budget-1g-after-fix.log` | NewOrder only | 1 GiB | Budget exhausted, reported, exit 3 (validator passed) |
| `run-4w-600s.log` | NewOrder only | 1 GiB | **Pre-fix**: uncaught exception, SIGABRT (exit -6) |

The sustained-load run is the positive result the audit asked for. 69 million
transactions over ten minutes across four workers with zero aborts and a clean
validated shutdown shows the engine is stable under sustained concurrent load.

Its registry accounting confirms the run was genuinely growth-free. Both the
load-complete and run-complete database records report
`allocated_registry_bytes=258443464` against an 8 GiB budget, so ten minutes of
concurrent StockLevel traffic added no structural registry bytes at all.

## What memory pressure establishes

The registry budget is a hard run-time limit, not a steady-state memory bound.
Published records and consumed IDs are never reclaimed (decision D7 in
[the design contract](../../architecture/rust-sto.md)), so every inserted row
permanently consumes structural bytes. At four workers on this host the
registry grows at roughly 70-100 MB/s under insert-carrying mixes, which the two
exhaustion runs bracket precisely:

- the 8 GiB default is spent after about 81 seconds of default-mix traffic;
- 32 GiB is spent after about 447 seconds of 10/90 NewOrder/Payment traffic,
  ending at `allocated_registry_bytes=34359734408` with 3,960 bytes of headroom.

No fixed budget sustains an unbounded insert-carrying run. A deployment must
size `MAKO_STO_TPCC_REGISTRY_MEMORY` from both the working set and the intended
run duration, and must treat budget exhaustion as the expected end of a bounded
run rather than as an anomaly. This is the concrete form of the documented
limits: the Rust budget excludes native Masstree memory, inline payloads are
charged but separately allocated payloads are not, and reclamation is deferred
until a grace-period design exists.

## The defect this found

The sustained run immediately failed against the pre-fix build with:

```text
libc++abi: terminating due to an uncaught exception of type std::runtime_error:
Rust STO full TPC-C NewOrder failed (status 5): new-order header: insert returned unexpected status 6
TPC-C CTest validation failed: benchmark exited with status -6
```

The fused NewOrder header helper mapped every status other than `Ok`,
`Duplicate`, and `Retry` to a fatal status, so ordinary capacity exhaustion
inside the header insert reached the C++ wrapper as an uncaught exception and
the process aborted with SIGABRT. The closed TPC-C contract requires the
reported resource outcome instead. Commit `ba9905533` propagates status 6, and
the same configuration now produces:

```text
TPCC_RESOURCE_EXHAUSTED phase=run error=Rust STO full TPC-C NewOrder failed (status 6): ...
```

with exit status 3 and no successful `TPCC_BENCH_RESULT`.

The exhaustion point within a transaction varies with which interning operation
first needs a registry segment, which is why this defect is intermittent at the
integration level. `fused_new_order_header_reports_registry_exhaustion_as_a_resource_outcome`
in `crates/sto-tpcc-ffi` pins it deterministically as an ordinary CI unit test.

This record deliberately stops at evidence rather than adding a CTest for the
structural-budget trigger. The run-phase exhaustion contract is already gated
end to end by `test_sto_tpcc_rust_run_resource_exhausted`, which asserts exit 3,
the `TPCC_RESOURCE_EXHAUSTED phase=run` marker, and the absence of a successful
result. Structural-budget admission is gated by
`rejected_registry_budget_never_allocates_a_process_lifetime_native_directory`.
Adding a third end-to-end gate for the same contract would require extending the
ASan process-lifetime-leak allow-list, whose rule set is deliberately limited to
the four gates that must create native roots before exiting, and would reintroduce
a timing dependency that the deterministic unit test avoids. The logs below
retain the end-to-end evidence for the structural-budget trigger.

## Scope of these claims

These runs establish sustained stability under concurrent load, and correct
reported behaviour as the structural budget is spent. They are not a latency
study: no tail-latency percentiles were collected. They do not exercise restart
or recovery, because ABI v1 keeps native Masstree allocations and registrations
for process lifetime and has no graceful shutdown. Shutdown after a long run is
slow (the four-worker 600-second run emitted its result at 19:27 and the process
exited shortly afterwards, while the 32 GiB exhaustion run needed several minutes
to tear down), which the existing `*_slow_exit` gates already acknowledge.
Native allocator-region exhaustion still asserts rather than reporting, and is
recorded as a deferred limit in the design contract.
