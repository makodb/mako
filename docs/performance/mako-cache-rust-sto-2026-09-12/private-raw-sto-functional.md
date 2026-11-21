# Frozen functional handoff

The private same-binary raw/facade diagnostic is ready for paired measurement. No timing result is claimed here.

Binary `target-rust195/release/engine_compare` has SHA256 `05894af04c9eeb785c6e9d556f7e5472daa627d3ccc398b2840c4439d7d4f96c`. Its text/data/BSS sizes are 1162027/86576/366323 bytes. Build policy is explicit Rust 1.95, non-LTO, znver1, matching both modes. Exact refreshed native archive hashes passed before and after the final build and tests. `raw-attribution.patch` has SHA256 `ca746ba9d1fa1fb76f139343d9bfdf1fb2f327aa8c4872cb01b1c62717065d33` and passes `git apply --check` against the production source at handoff.

- Both new tests passed. The config fixture checks every explicit facade limit/policy, and the native fixture checks repeated PUT presence, abort invisibility, owned payload reuse, committed local metadata without a global ID, and later update/read success.
- `smoke.py` passed 24 PMU-disabled local cases across engine/raw-sto, W1/W4, two warmup lengths and three measured lengths. It verified every expected result field and all final values. Eleven malformed/overflow argument cases rejected without a success record.
- Rustfmt and shell syntax checks passed. Focused `cargo clippy --no-deps ... -- -D warnings` passed for the private benchmark. The first broader strict Clippy run failed on 37 pre-existing mako-cache lints under the engine-sto-only feature selection. That failed log is retained; no dependency code or lint suppression was changed.
- Recursive comparison found sto-core, sto-masstree, masstree, mako-engine-sto and mako-clock identical to the frozen production copies. The source manifest excludes copied build artifacts.

`test.log`, `build-final.log`, `clippy-focused.log` and `smoke-report.json` contain the executed evidence. `build.log` is the initial build output with tool truncation and is not the complete build transcript. The initial test build also caught a misspelled private native library directory; `build.sh` was corrected before the successful test/build runs.

The binary and source are frozen after this handoff. Only docs and provenance exports may be corrected without creating a new variant. Frame boxing remains a design in `FRAME-DESIGN.md`; it is not part of the executable.
