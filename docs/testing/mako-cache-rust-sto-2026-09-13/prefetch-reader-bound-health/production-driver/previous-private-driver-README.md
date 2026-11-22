# Prefetch, read-only fence, and native-health checkpoint

The health-fixed source passes 278 focused checks in each sanitizer profile.
This checkpoint includes the promoted packed-clock prefetch, independent
read-only cuts, post-cut registered-slot bound, and the cold native-runtime
health diagnostic. Earlier reports remain unchanged and describe their own
source checkpoints.

| Profile | Instrumented positive checks | Separate uninstrumented models |
| --- | ---: | ---: |
| ASan | 270 | 8 |
| TSan | 270 | 8 |
| Native-C++ UBSan | 278 | 0 |

ASan's 270 comprise 259 tests with leak checking and 11 individually named
intentional-quarantine cases with only leak detection disabled. The mandatory
lost-transaction-buffer negative control also failed LeakSanitizer as required
under the same two exact process-lifetime worker allocation exceptions. It
does not count as a positive test.

TSan retains Rust, rebuilt Rust standard library, and focused C++ boundary
instrumentation for every production adapter/cache/native test. It uses no
data-race suppression. UBSan instruments native C++, not Rust. None of these
profiles instruments the system RocksDB library.

## Commands and identities

[The source/native manifest](source-native-manifest.json) records the shared
234-file source snapshot, report digests, and native compile/object/archive
identities. Each execution independently checked unchanged sources, both
focused archives, eight audited native objects, and generated Masstree
configuration. Reusing those exact native artifacts avoids implying that
every source file was recompiled during this execution.

The detailed [ASan](execution-asan.json), [TSan](execution-tsan.json), and
[UBSan](execution-ubsan.json) execution records retain their commands,
environment, original log paths, and before/after hashes. Their corresponding
`sanitizer-*.json` files record every test command and result.

ASan and UBSan ran the existing configured CMake target. TSan ran the
[reviewed private driver](tsan-model-runtime/run_rust_sto_cache_tests.py) while
other source-checked gates required the production driver to remain frozen.
The [driver proposal](tsan-model-runtime/driver-proposal.patch) passes
`git apply --check`; its 15 regression tests pass, including required-model
execution and fatal child-failure checks. These reports do not claim that
this CI-only proposal was already promoted at execution time.

The diagnostic's [real native-poison proof](../native-runtime-health/README.md)
is separate functional evidence. Its cfg-only Rust test and macro-guarded
native helper are absent from these sanitizer executables. The new ordinary
healthy-query test is included, with leak checks enabled under ASan.

## Why Loom is separate under TSan

The [runtime diagnosis](tsan-model-runtime/report.json) preserves an actual
TSan failure, not a passing retry. GDB stopped in `__tsan_func_entry` at
`tsan_rtl.h:799`. Its shadow call-stack push targeted `0x7ffff6700000`,
exactly beyond the writable mapping. The native coroutine stack pointer,
`0x7ffff6727b88`, remained inside its separate 32 KiB stack mapping.

A standalone crate containing the byte-identical model and only Loom 0.7.2
as a direct dependency also died with SIGSEGV. It links no Mako or native
boundary code. The failure emitted no TSan data-race warning and did not fail
a model assertion.

Generator 0.8.9 switches CPU stacks through `swap_registers` without TSan
fiber hooks. The installed TSan API requires `__tsan_switch_to_fiber`
immediately before changing stacks. The evidence isolates exhaustion of
TSan's shadow call stack in this unsupported instrumented coroutine runtime.
No patched generator or fiber-hook repair was tested.

Only the two exact model binaries, `commit_fence_loom` and
`occupancy_credit_loom`, run separately. All eight models must execute and
pass, with no schedule bound. Every other cache integration binary stays
instrumented. No error category is suppressed. ASan already uses the same
exact-binary separation for its independently documented fiber-stack failure.

## Intermediate results

[The before-health checkpoint](intermediate-before-health/) retains the
277-check ASan and UBSan passes and the original failed TSan report. That
TSan run completed 252 positive checks before its model executable crashed.
It is not an acceptance pass and is not relabeled as health-fixed evidence.
The diagnostic also labels its manually interrupted first debugger attempt;
that attempt is neither a pass nor the captured SIGSEGV.
