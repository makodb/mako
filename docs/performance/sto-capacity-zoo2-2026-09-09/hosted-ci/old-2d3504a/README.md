# Original implementation CI

Commit `2d3504a277b3ee66c8fea10099accd2290884f31`, PR 91. Final status checked
on 2026-09-09 at 08:47 UTC. Five jobs passed and one failed. No job was rerun
or canceled to obtain these results.

| Job | Result |
| --- | --- |
| Release build and tests | Passed |
| Rust STO Miri ownership | Passed |
| Goal-0 SRPC canonical Rust dual compile | Passed |
| AddressSanitizer/native boundary | Passed |
| ThreadSanitizer/native boundary | Passed |
| Undefined-behavior/native boundary | Failed |

The [CI workflow](https://github.com/makodb/mako/actions/runs/34325825228)
passed. The [sanitizer workflow](https://github.com/makodb/mako/actions/runs/34325825216)
failed solely in `test_sto_tpcc_rust_concurrent_resource_exhausted`.
[Final CI metadata](ci-final.json) and
[final sanitizer metadata](sanitizers-final.json) retain each job's status.

## Failure and follow-up

The failed test returned the expected exit code 3 and emitted no successful
TPC-C result. Its validator then rejected the output because concurrent SRPC
shutdown messages split the required `TPCC_RESOURCE_EXHAUSTED phase=run`
diagnostic. There was no UBSan finding.

The uncompressed [failed-job log](undefined-job.log.gz) records:

- Line 4606 starts `TPCC_RESOURCE_EXHAUSTED phase=` and continues with an
  unrelated SRPC shutdown message.
- Line 4609 contains the separated `run error=Rust STO full TPC-C NewOrder
  failed (status 6): new-order order-line batch: Capacity(BufferLimit)`.
- Line 4841 reports the missing exact diagnostic text.

The reporter used separate iostream insertions protected by a benchmark-only
mutex. SRPC logging does not share that mutex. The follow-up commit
`a3ad9a110727f5ecc937cbeee23fe40152df719f` emits one bounded atomic diagnostic
and adds a concurrent-output regression test. Its fresh CI runs are separate
validation, not reruns of this failed workflow.

## Evidence checks

The JSON files and compressed log are byte-identical copies of the original
cache evidence. The retained log was checked for credential-shaped tokens,
private keys, signed credential URLs, and authorization values. No exposed
credentials were found; all three authorization entries contain `***` masks.

Verify the portable file hashes from this directory:

```sh
sha256sum -c SHA256SUMS
gzip -dc undefined-job.log.gz | sha256sum
```

The second command must print
`38e4f29c7575f9c6ce7e0ae11e15f975ad68bf0af5a633f162bbd7a72bfcc641`.
The original uncompressed log is 552019 bytes; its lossless compressed copy is
88906 bytes. No binaries or full build artifacts are included.
