# Private native-global-poison proof

The corrected private cache rejects a native singleton that is already poisoned,
even before any Rust table or core runtime has mapped the failure. The restored
old cache-hit call commits in that same state.

`proof-report.json` records eight fresh-process cases and the source, executable,
native archive, and command identities. The corrected route rejects cached hits
and ordinary misses for both already-active and newly begun transactions. The
old route's two cached-hit cases return `Committed` with successful cleanup;
its two ordinary-miss controls reject. The two expected failures print
`NATIVE_HEALTH_BYPASS_COMMITTED`, not merely an unrelated panic or link failure.

Each case opens and warms two databases. A second OS thread attaches to the
first database, performs the real native poison transition, and waits without
mapping a Rust access error. The target worker observes native `Poisoned` while
both core runtimes and the target table remain `Healthy`. The checked target
operation then poisons its own table and core through normal access-error
handling. The first core remains healthy, demonstrating why core-only health
cannot substitute for the process-wide native check.

The test-only C++ translation unit includes the actual production
`mtree_abi.cc`. An explicit macro enables one helper that calls its existing
`poison` function. The helper does not write an assumed opaque layout, create a
second health flag, or expose a reset. The proof executable has exactly one
native health getter and one helper. The test archive precedes the original
immutable archives only in `run_proof.sh`; normal release and timing builds do
not use it. Both original native archive hashes remain unchanged and neither
contains the helper.

This injects the actual atomic native state transition, not an unexpected C++
exception. It proves rejection after that transition and before Rust mapping;
it does not claim to reproduce every native fault site. Each exact child has a
bounded timeout and exits with its permanently poisoned singleton. No test
resets global health or performs healthy-fixture teardown after poison. This
is functional private-prototype evidence, not sanitizer or release qualification.

The negative control is a separate copied workspace whose only source change
from the corrected copy restores the old unchecked cache-hit call. It is not a
modification of any preserved timing binary. The report verifies all 108 scoped
source/input hashes before and after the eight final runs.

Commands are retained in `build_native.py`, `run_proof.sh`, and
`verify_proof.py`. `fixed-final.log` contains the ordinary parent test's four
successful subprocesses. The per-case logs are named `fixed-*.log` and
`old-hit-*.log`. Rust and Cargo are explicitly 1.95.0.
