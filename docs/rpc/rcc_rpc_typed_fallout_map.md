# rcc_rpc Typed Fallout Map

Date: 2026-04-12

## Purpose

This document closes TODO leaf `3a.3` by converting typed-prep compile fallout into
an explicit migration map that feeds leaf `3b` and leaf `3c`.

## Evidence Inputs

1. Prep harness and baseline expectations:
   - `test/rpcgen_in_tree_rcc_rpc_typed_prep_test.py`
   - `docs/rpc/rcc_rpc_typed_prep_inventory.md`
2. Reproduced service compile fallout against temp typed `rcc_rpc.h`:
   - Probe: `#include "src/deptran/service.h"`
   - Compiler: `g++ -std=c++23 -fsyntax-only`
   - Historical first-error signatures (now addressed by leaf 3b.1):
     - `SimpleCommand does not name a type`
     - `parent_set_t does not name a type`
     - `Rpc*Request/Rpc*Response has no member {cmd, txn_cmds, parents, p, x}`

## Current Subsystem Status

| Subsystem | Current prep status | Typed migration meaning |
| --- | --- | --- |
| `service` | PASS | Type-visibility gate for typed `Classic` request/response structs is now unblocked; remaining work is high-traffic callsite migration. |
| `communicator` | PASS | Header-only probe stable; keep as regression guard while callsites are migrated. |
| `config/control` | PASS | Header-only probe stable; preserve while `rcc_rpc` service/proxy migration proceeds. |

## Fallout Taxonomy (Service Subsystem)

| Bucket | Symptom signature | Likely root cause | Representative RPCs |
| --- | --- | --- | --- |
| A. Type visibility | `SimpleCommand` / `parent_set_t` unknown in generated typed structs | Generated request/response structs relied on types not visible at inclusion points of `rcc_rpc.h` in service compilation units; fixed in leaf 3b.1 via typed-header preamble includes | `SimpleCmd`, `TapirFastAccept`, `CarouselFastAccept`, `RccDispatch`, `RccCommit`, `RccPreAccept`, `RccAccept`, `RccInquire` |
| B. Typed field bridge mismatch | `Rpc*Request`/`Rpc*Response` missing expected fields (`cmd`, `txn_cmds`, `parents`, `p`, `x`) in bridge paths | Legacy-to-typed wrapper assumptions drift in generated/consuming code paths for `ClassicService` methods | Same as bucket A, especially RCC/Janus/Tapir methods |
| C. Downstream override/callsite drift | override/callsite compile fallout after typed bridge enters service surface | Existing `ClassicServiceImpl` and proxied method usage still shaped around pointer-era signatures | `src/deptran/service.h`, `src/deptran/service.cc`, communicator/protocol callsites |

## File-Level Migration Map

### Leaf 3b (high-traffic `ClassicService` / `ClassicProxy` path)

Goal: make the service subsystem compile and keep high-traffic classic request
paths working through typed request/response APIs plus compatibility wrappers.

| Priority | Area | Primary files | Exit criteria |
| --- | --- | --- | --- |
| P0 | Type visibility bridge for typed `Classic` structs | `src/deptran/service.h`, `src/deptran/procedure.h`, `src/deptran/rcc/tx.h` (or narrower headers that own `SimpleCommand` / `parent_set_t`) | Service probe no longer fails on unknown `SimpleCommand` / `parent_set_t`. |
| P0 | Typed bridge field alignment for `ClassicService` RPC methods | generated `rcc_rpc.h` + `src/deptran/service.cc` callsites relying on request/response fields | No `Rpc*Request/Rpc*Response has no member ...` errors for classic RCC/Tapir/Janus methods. |
| P1 | High-traffic proxy/service callsite migration to typed overloads | `src/deptran/service.cc`, `src/deptran/communicator.h`, `src/deptran/communicator.cc`, `src/deptran/rcc/commo.h`, `src/deptran/rcc/commo.cc`, `src/deptran/rcc/coord.cc` | Representative high-traffic paths compile using typed request/response calls while legacy wrappers still build. |

Leaf 3b validation gates:
- `test_rpc_rpcgen_in_tree_rcc_rpc_typed_prep` keeps service/communicator/
  config-control probes in all-pass state while high-traffic typed callsite
  migration lands.
- Existing `rcc_rpc` typed sync guard keeps passing:
  `test_rpc_rpcgen_in_tree_rcc_rpc_typed_sync`.
- RPC-focused ctest regex suite remains green.

### Leaf 3c (remaining services/proxies and cleanup)

Goal: finish non-classic surfaces and remove transitional assumptions once 3b
stabilizes.

| Priority | Area | Primary files | Exit criteria |
| --- | --- | --- | --- |
| P1 | Remaining protocol service/proxy typed alignment | `src/deptran/*/service.h`, `src/deptran/*/commo.h`, protocol frame/coordinator callsites touched by `rcc_rpc.h` (`janus`, `tapir`, `troad`, `raft`, `paxos`, `mencius`, `copilot`) | Representative non-classic callsites compile with typed APIs + wrappers. |
| P1 | Config/control regression containment | `src/deptran/config_service.h`, `src/deptran/config_client.h`, `src/deptran/benchmark_control_rpc.h` | Prep pass status preserved throughout service migration. |
| P2 | Transitional cleanup | generated bridge paths/callers no longer needed after typed callsite completion | No dead wrapper-only callsites blocking deprecation path. |

Leaf 3c validation gates:
- Add/expand `rcc_rpc` typed header compile guard with representative non-classic
  callsite coverage (TODO Tests leaf 3).
- RPC-focused ctest regex suite green in typed default mode.
- Follow-up compatibility mode run (from TODO test matrix) still green.

## Sequencing

1. Leaf 3b.1 (service compile unlock/type visibility) is completed.
2. Execute remaining leaf 3b P1 items (high-traffic classic callsites).
3. Only after service probes and high-traffic paths stabilize, execute leaf 3c
   to migrate remaining protocol surfaces and cleanup.
4. Keep prep + sync guards as continuous drift checks while 3b/3c land.

## Risks and Mitigations

- Risk: fixing service type visibility by broad header includes increases compile
  coupling.
  - Mitigation: prefer narrow owner headers and forward declarations where valid.
- Risk: typed bridge field-name assumptions drift again as generator evolves.
  - Mitigation: keep explicit compile guards for representative `Classic` RPCs and
    run sync guard in CI.
- Risk: partial migration breaks compat wrappers unexpectedly.
  - Mitigation: keep typed+legacy compile usage in guards; do not remove wrapper
    paths until 3c completion criteria are met.
