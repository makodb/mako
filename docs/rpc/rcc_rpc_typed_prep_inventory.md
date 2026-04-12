# rcc_rpc Typed Prep Inventory

Date: 2026-04-12

## Scope

Prep inventory for TODO leaf `3a.1` in `docs/TODO-srpc.md`:

- Generate typed `rcc_rpc` artifacts from `src/deptran/rcc_rpc.rpc`.
- Capture compile fallout by subsystem before callsite migration:
  - `service`
  - `communicator`
  - `config/control`

This inventory intentionally runs against typed generated output in a temp
directory and does not require in-tree `rcc_rpc.h` check-in yet.

## Method

Harness: `test/rpcgen_in_tree_rcc_rpc_typed_prep_test.py`

1. Copy `src/deptran/rcc_rpc.rpc` to a temp directory.
2. Run:

```bash
./bin/rpcgen --cpp --python --cpp-mode typed <tmp>/rcc_rpc.rpc
```

3. Compile subsystem probes with:

```bash
g++ -std=c++23 -fsyntax-only \
  -I <tmp-generated-dir> \
  -I <repo> -I <repo>/src -I <repo>/src/rrr -I <repo>/src/deptran \
  -I <repo>/third-party/rusty-cpp/include \
  <probe>.cc
```

## Initial Inventory (2026-04-12)

| Subsystem | Probe Include Surface | Result | Notes |
| --- | --- | --- | --- |
| `service` | `src/deptran/service.h` | FAIL | Typed `ClientControl` bridge fallout observed in generated header (`TxReply`/`DispatchTxn` related type mismatch) plus downstream service override/type mismatches. |
| `communicator` | `src/deptran/communicator.h` | PASS | Header-level typed generation compiles in prep probe. |
| `config/control` | `src/deptran/config_service.h`, `src/deptran/config_client.h`, `src/deptran/benchmark_control_rpc.h` | PASS | Config/control header surfaces compile in prep probe. |

## Updated Baseline (2026-04-12, after leaf 3b.1)

`rcc_rpc.rpc` now injects typed-header preamble includes for high-traffic
`Classic` value types:

- `#include "procedure.h"` (`SimpleCommand`)
- `#include "rcc/tx.h"` (`parent_set_t`)

Current prep harness baseline is now all-pass:

| Subsystem | Probe Include Surface | Result | Notes |
| --- | --- | --- | --- |
| `service` | `src/deptran/service.h` | PASS | Type-visibility fallout for `SimpleCommand` / `parent_set_t` resolved in typed generated header preamble. |
| `communicator` | `src/deptran/communicator.h` | PASS | Unchanged pass status. |
| `config/control` | `src/deptran/config_service.h`, `src/deptran/config_client.h`, `src/deptran/benchmark_control_rpc.h` | PASS | Unchanged pass status. |

## Follow-up Mapping

- `Leaf 3b` should prioritize high-traffic `ClassicService`/`ClassicProxy`
  callsite migration to typed request/response APIs now that service header
  type visibility is unblocked.
- `Leaf 3c` should handle remaining subsystem migrations and transitional shim
  cleanup after `Classic` high-traffic paths stabilize.
