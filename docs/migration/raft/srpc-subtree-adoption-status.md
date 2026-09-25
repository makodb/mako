# Adopting the new srpc: what is happening and why

Working notes for the `adopt-srpc-subtree` branch, written because the work
has spent a long time in build plumbing that is hard to follow from the
outside.

## Why this work exists

The Rust Raft server is finished and passing. The reason for touching srpc at
all is the *next* step: pushing the Rust Raft to `mako-dev`. If this branch
keeps diverging from mako-dev while mako-dev moves, that push becomes a large
conflict. Merging now, and keeping merged, is what makes the eventual push
land cleanly.

Two conditions shape the target:

1. **The merge must be smooth.** Mako can call Raft, and Paxos must not be
   disturbed much.
2. **This branch carries a Rust runtime**, so `rcc_rpc.h`, commo and service
   can become Rust.

## Where the Raft work itself stands

Verified, not asserted: at commit `bbbd51d89` the 25-case Raft lab suite
passes 25/25 (`ALL TESTS PASSED`, exit code 0). rustc builds the whole Raft
server in 13 seconds. Nothing found during this merge has been a Raft defect.

## What the merge already achieved

This is the part that protects against conflicts on mako-dev, and it is done:

- `src/rrr` became `src/srpc`, tracked as a git subtree, then pulled forward
  141 commits.
- Every `#include "rrr/..."` converted; zero `rrr::` references remain outside
  the subtree.
- `src/deptran/rcc_rpc.h` regenerated, with all twelve RPC ids byte-identical
  to before — the wire contract is unchanged.
- `rusty-rustc` and `rusty-cpp-markers` moved out of the subtree, where
  upstream's own `check_rust_independence.py` requires them to be.
- `cpp_value_init` retired; the zero-initialisation guarantee it provided now
  lives in `scripts/raft_field_census.py` as a rule this repository owns.

## What has been costing the time

`srpc_goal0_dual_compile` (`src/srpc-cmake/CMakeLists.txt:640`) is a build
gate. srpc's single set of Rust sources is compiled two ways — rustc for the
Rust library, and rusty-cpp plus clang for `libsrpc.a`, which is what Mako
actually links. Nothing in either compiler forces the two to agree, so the
gate compiles the whole crate independently, links a generated C++ oracle
against it, runs it, and compares against recorded expectations.

Upstream legitimately changed several fields from `rusty::Cell<T>` to a new
`srpc::SharedCell<T>` (a mutex). That one change invalidated recorded values
in **five separate layers** of the gate, and each layer only became visible
after the previous one was fixed:

| layer | what went stale | how it was fixed |
|---|---|---|
| module surfaces | 105 pinned declarations | reconciled against upstream's gate and the generated output; 247 added, coverage 1205 → 1347 |
| module import graph | 12 hand-written per-module lists, 25 modules unchecked | replaced with upstream's complete 37-module `EXPECTED_IMPORTS` |
| exported symbols | 22 modules' symbol sets | adopted upstream's, plus Mako's four admission-gate symbols, verified against `libsrpc.a` |
| retired carrier | `reactor/epoll_platform_linux.cc` no longer exists | its four entry points are ordinary crate symbols now; the platform allowlist is empty |
| struct layout and traits | 22 numbers, 8 Send/Sync assertions, 2 signatures | numbers measured, traits checked against upstream |

## How the layout numbers are now maintained

`scripts/rebaseline_srpc_layout.py` measures them rather than having anyone
type them. It builds a probe from the oracle's own translation unit,
instantiates an undefined template with each `sizeof`/`alignof`/`offsetof`,
and reads the values out of clang's diagnostics — no linking, because the
oracle defines C-kernel stubs that collide with `libsrpc.a`. `--check`
reports drift without rewriting.

The numbers stay *recorded* in the gate rather than recomputed at gate time.
Recomputing them would assert nothing; Mako links `libsrpc.a`, so a silent
layout move is an ABI break and the assertion has to be a ratchet.

One thing worth recording, because it was tried and was wrong: deriving these
from rustc. The lanes do not share a layout. `srpc::CircuitBreaker` is 344
bytes under clang and 96 under rustc, because `rusty::Mutex<T>` in C++ and
`std::sync::Mutex<T>` in Rust are different types of different sizes. Each
lane pins its own numbers — Rust's in srpc's cargo tests, C++'s in this
oracle — and they are not supposed to agree.

## Remaining

- Gate run 3 in flight; the oracle compile is where the verdict lands.
- Then a full ninja build, then the lab suite on the merged tree.
- Then the conversion itself: the **Raft slice only** of `rcc_rpc.h` to Rust
  (leaving MultiPaxos, ServerControl and ConfigKvService generating as C++,
  per condition 1), then commo and service onto the Rust lane, moving
  `rpc_server_` with them.

`server_worker.cc` already helps here: `hb_rpc_server_` (line 14) hosts only
the control service, and `rpc_server_` (line 72) hosts only the replication
protocol's services. So Raft can move lanes without touching either Paxos or
Mako's own services.
