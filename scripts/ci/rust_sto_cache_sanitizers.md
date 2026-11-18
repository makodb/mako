# Rust STO adapter and cache sanitizer scope

Run `MAKO_SANITIZER_FOCUS=cache scripts/ci/run_rust_sto_sanitizer.sh address`
with a fresh `MAKO_SANITIZER_BUILD_DIR`. `thread` and `undefined` select the
other profiles. The driver records commands, test names, ignored tests,
qualifications, and negative controls in `rust-sto-cache-tests.json`.

The record-local version gate runs the `sto-masstree` local-version unit
module, all four `local_versions_native` tests, and the exact fixed-u64
admission-rejection unit test. The native suite uses the real Masstree
directory. The unit module uses the memory directory with the real STO core
to force version exhaustion and indeterminate release. Native and fixed-u64
inventories must match their reviewed names exactly; missing, duplicate,
extra, or ignored tests fail the gate.

Ordinary adapter/cache suites also reject unreviewed ignored tests. The only
permitted ignored name is the adapter's LSan negative control, which the
ASan profile executes separately and requires to fail with its expected leak.

Those cases cover mixed ordered/local writes, same-key worker handoff,
phantoms, fixed batches, terminal reads, scans, read-your-writes, and rejected
configurations. Exhaustion must stop before any commit hook or value install.
Only `local_version_tests::indeterminate_local_release_ignores_mixed_ordered_id`
uses the exact intentional-quarantine leak qualification. The remaining
record-local tests keep ordinary leak checking enabled.

The focused native archive compiles these production sources with the same
generated Masstree configuration and compiler flags as `mako`:

- `src/mako/storage/mtree_abi.cc`
- `src/mako/allocator.cc`
- `src/mako/core.cc`
- `src/mako/counter.cc`
- `src/mako/rcu.cc`
- `src/mako/ticker.cc`
- `src/mako/silo_runtime.cc`

It links the separately compiled, instrumented Masstree archive. The focused
gate does not compile the C++ transaction engine or cluster modules. It does
not claim instrumentation of the system RocksDB library. ASan and TSan also
instrument Rust and rebuild its standard library. UBSan instruments native
C++, with stable Rust linked to the matching UBSan runtime; it is not Rust
UBSan coverage.

An explicitly rebuilt RocksDB can be selected through the existing
`ROCKSDB_LIB_DIR` and matching runtime library search path. For TSan, build it
with the same Clang sanitizer runtime and preserve its native C++ ABI. An
uninstrumented dependency's function-local-static guard fast path can hide
its publication edge from TSan; do not suppress the resulting report without
attribution. The STO-shape campaign retains same-source control/instrumented
RocksDB 8.9.1-2 builds and a Rust/C-ABI guard diagnostic: three control runs
report the mutex race, three instrumented runs pass, and the original 14-test
MRX executable has the same paired outcome. Its TSan launcher hashes all
selected DSO aliases before and after the gate. This diagnostic does not
qualify unrelated system libraries or the complete sanitizer gate.

## Leak qualifications

Ordinary ASan tests keep leak detection enabled. The only suppression entries
are the two named, non-inlined worker allocation helpers in
`rust_sto_cache_lsan.supp`. They implement the existing process-lifetime worker
contract. Transaction log and value allocation happens outside those helpers.
Before any positive ASan gate, the driver requires a deliberately lost
transaction log to fail LeakSanitizer with a `TouchLog` allocation stack under
the same suppression file. The initial probe reported 1,318 leaked bytes in
three allocations.

Each intentional poison/quarantine test is listed exactly in the driver,
excluded from the ordinary leak-checked run, and rerun alone with only leak
detection disabled. Memory-error instrumentation remains enabled. TSan and
UBSan inherit no suppression files.

The exact adapter/cache inventory also includes
`txn::borrow_tests::real_cleanup_panic_pins_original_for_abort_drop_operation_and_commit`
and
`txn::borrow_tests::uncertain_holder_keeps_original_backing_alive_after_pool_destruction`.
Both intentionally retain original borrowed storage after uncertain cleanup;
their ASan checks read that storage after facade or holder destruction. These
two additions were explicitly approved after isolated baseline runs attributed
all 438 reported allocations (134,272 bytes) to them and the remaining ordinary
library tests passed with leak detection enabled. No allocation-root suppression
or production cleanup behavior was added.

## Loom model separation under ASan and TSan

Only `commit_fence_loom` and `occupancy_credit_loom` run separately without
ASan or TSan. Their `generator` dependency, version 0.8.9, switches stacks
through `swap_registers` assembly and has neither sanitizer's fiber-switch
hooks. The reproduced ASan failure emitted:

```text
ASan is ignoring requested __asan_handle_no_return
False positive error reports may follow
ERROR: AddressSanitizer: stack-buffer-overflow
std::panicking::catch_unwind::cleanup
generator::detail::gen::gen_init_impl
generator::detail::asm::gen_init
```

The TSan failure was a SIGSEGV in `__tsan_func_entry`, at the shadow
call-stack push in `tsan_rtl.h:799`. GDB showed its destination exactly beyond
the writable mapping, while the native coroutine stack pointer remained
inside its separate stack. A standalone crate with the byte-identical model
and only Loom as a direct dependency reproduced the crash without Mako or
native boundary code. No TSan data-race warning or model assertion preceded
it. The [diagnostic artifacts](../../docs/testing/mako-cache-rust-sto-2026-09-13/prefetch-reader-bound-health/tsan-model-runtime/report.json)
preserve the failing commands, stack trace, memory mappings, disassembly,
dependency source, and installed TSan fiber API. No patched generator or
fiber-hook repair was tested.

The production driver requires both model suites to pass in a separate Cargo
target directory with sanitizer flags and native-integration selection
removed. All eight models must execute, with no schedule bound. Missing or
ignored models and child failures fail the gate. The driver then discovers
and runs every other cache integration test under the selected sanitizer.
No error category or production data race is suppressed. The native-C++
UBSan profile still executes both model suites with the other tests.

The separate model environment also removes explicit RocksDB link selection
and its corresponding loader-path entries, so an instrumented native DSO cannot
be carried into an executable that intentionally has no sanitizer runtime.
