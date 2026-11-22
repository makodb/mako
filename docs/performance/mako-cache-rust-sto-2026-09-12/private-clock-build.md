# Settled private artifacts

All three release binaries use Rust 1.95.0, non-LTO, target-cpu=znver1,
one codegen unit, frame pointers, and debuginfo=1. The private baseline is
byte-identical to the frozen production candidate.

| Variant | SHA-256 |
| --- | --- |
| `target-base-rust195/release/engine_compare` | `e487077485d4de795266e5224265e468418e6513fb507b70a8445cb4d49a59b6` |
| `target-control-rust195/release/engine_compare` | `5bbb40aa5151fe6a3130922a5957265e6e9e7c629a6aa8a53168c5c146a9255b` |
| `target-prefetch-rust195/release/engine_compare` | `508acc0213df27f2fe990548189d45d2dbfa0d71e82f18500497ce3063c939eb` |

Control and prefetch both have text 1,121,011 bytes, data 84,192 bytes and
BSS 364,867 bytes. Their HLC is at 0x129800 and their isolated capability
flag is at 0x129780. All three reserve_upper_metadata specializations have
identical addresses and 267-byte sizes between these two variants. The
prefetch binary emits PREFETCHW at 0x6c78f, 0x6c89f and 0x8ab3f; the control
emits none. Omitting the three-byte instruction shifts the following local
branch offsets and padding, without changing the function or global layout.

`prefetch.patch` is the complete five-file source diff from the baseline.
`no-hint-control.patch` is the control's sole source difference from prefetch.

Both Rust 1.95 state-preservation runs passed one test with eight filtered
out. See `test-control-rust195.log` and `test-prefetch-rust195.log`. The test
covers zero stamp, an allocated nonzero stamp, held/free gate state, and
next dense sequence progression from 1 through 3. The hint changes none.
These are native CPU tests, not Miri proof of the hardware hint. Under Miri
the helper deliberately does nothing, and no Miri run is claimed here.

Source checks establish that ordinary `commit_report` calls `inner.commit`
without a Driver hook. The Driver hint guard accepts only RestrictedPair
and RestrictedStampOnly, excluding RustSequence and NativeLocked. Core calls
reserve_upper_metadata only for writing commits after successful acquisition
and before final validation. The hint neither allocates a timestamp nor
changes a gate, sequence, holder, record, or failure report. Cold attachment
initializes the CPU feature check before transactions begin.

The production native archives remained unchanged after these builds:

- libmako.a: `6ae5e5cce81031cbd9daefe9a3bb907fae5fa96e2bac42ed91af314bc2ee0209`
- libmasstree.a: `d0470b6005b62b741198b1143ba82807bdcd6c9e2cadb274a6dad5d11b5ed613`

The production worktree's 225 recorded source hashes still matched after
the private work. No production source was edited. No performance result
has been collected for these private variants; the performance agent owns
paired runs after its existing remote measurement lock is released.
