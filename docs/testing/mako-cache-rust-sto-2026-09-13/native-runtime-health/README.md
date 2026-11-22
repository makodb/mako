# Native runtime health diagnostic

`LocalDb::runtime_is_healthy()` now requires both the core runtime and the
shared native Masstree runtime to be healthy. A failed native health query
also returns false. The query neither attaches nor quarantines its caller.
`check_abort_cleanup()` remains core-only. Native poison from another
database does not make a completed core abort uncertain.

No begin, point-operation, commit, native ABI or CMake admission path changed.
The existing native `Runtime::health()` getter supplies the additional
diagnostic observation. A transaction can still begin on an already-attached
worker before a point operation maps native poison into its core runtime.

## Executed checks

- Ordinary native engine library, Rust 1.95.0: 20 passed, one existing
  intentional LSan negative control ignored. The new healthy diagnostic test
  covers an attached caller and an unattached observer. It uses normal native
  archives and never poisons them.
- Dedicated native proof: one exact parent test passed. Its two fresh child
  processes cover explicit abort and Drop. A second worker invokes the real
  production singleton's Release poison transition while both database core
  runtimes and the target table are still healthy. Both diagnostics return
  false, an unattached observer stays unattached, and the caller's health and
  quarantine count do not change. Core abort cleanup remains successful.
  A later ordinary point operation maps poison into the target core and
  quarantines its caller. The origin core remains healthy, and the parent
  process's native runtime stays healthy.
- Two isolated source mutants, each built in a fresh Cargo target, are
  rejected in both child scenarios. Restoring the old core-only diagnostic
  fails the unhealthy assertion. Coupling abort cleanup to the combined
  diagnostic fails the known-clean abort assertion with `WorkerPoisoned`.
  These are four assertion failures, not timeout-based kills.
- Four runner parser tests pass. They cover retained native compile flags,
  quoted paths, malformed options and rejection of unreviewed response files.

The native proof invokes the actual state transition, not a reproduced native
exception. It is a functional proof, not sanitizer evidence. The test-only TU
includes the production `mtree_abi.cc` and is guarded by
`MAKO_NATIVE_RUNTIME_HEALTH_PROOF=1`. The Rust test also requires `test`,
`test-hooks`, `mako_native_runtime_health_proof`, and `not(miri)`. Ordinary
native, sanitizer, release and Miri builds do not include the proof module.
There is no reset and no opaque-state offset write. Poisoned child processes
retain their worker/runtime state until exit, consistent with the existing
process-lifetime ownership policy.

The runner links its dedicated boundary archive ahead of the unchanged
production archives. Its symbol audit finds exactly one native health getter
and one test helper in the proof executable. The helper is absent from both
production archives. All 121 scoped input hashes remain unchanged during the
positive proof and after the negative controls.

## Reproduction and provenance

The tested runner is `run_native_health_proof.py`. Its explicit `--repo-root`
option also permits replay from this evidence directory. Use a fresh output
directory; the runner refuses to reuse an existing one.

```sh
python3 run_native_health_proof.py \
  --repo-root /path/to/mako \
  --build-dir /path/to/mako/build_c22 \
  --libcxx-dir /path/to/llvm22/lib \
  --output-dir /var/tmp/new-native-health-proof
```

`report.json` contains exact commands, Rust compiler identity, source/native
hashes before and after, and proof executable/archive hashes.
`negative-controls.json` records both complete copied-source manifests and
the four failing child invocations. `qualification.json` records the normal
test executable, retained log hashes, prepared driver hashes and source
archive hash. `source-inputs.tar.gz` preserves the scoped source files and
compile-command metadata, plus the cache diagnostic comment. It excludes
the large native archives, whose identities are recorded separately.

Production archive identities for this proof are:

```text
libmako.a     53e9a34a1c1e963aaa809adcb26ac5af70d27d072c351122f5911b8f7a273402
libmasstree.a d0470b6005b62b741198b1143ba82807bdcd6c9e2cadb274a6dad5d11b5ed613
```

`driver-path-fix.patch` and `driver-tests.patch` are the prepared CI-only
follow-up. At the time these checks ran, the new in-worktree runner still had
its initial toolchain-path error and was frozen alongside other gates. The
corrected runner was tested in scratch. No result here claims that the
uncorrected runner passed or that the driver-only follow-up was already
promoted.
