# Final implementation hosted CI

All six hosted jobs passed for commit
`a3ad9a110727f5ecc937cbeee23fe40152df719f`, PR 91. These were fresh runs after
the atomic diagnostic fix, not reruns of the original UBSan failure.

| Job | Result |
| --- | --- |
| Release build and tests | Passed |
| Rust STO Miri ownership | Passed |
| Goal-0 SRPC canonical Rust dual compile | Passed |
| AddressSanitizer/native boundary | Passed |
| ThreadSanitizer/native boundary | Passed |
| Undefined-behavior/native boundary | Passed |

The [CI workflow](https://github.com/makodb/mako/actions/runs/34329193601)
and [sanitizer workflow](https://github.com/makodb/mako/actions/runs/34329193602)
both completed successfully. The release job finished at 09:11:21 UTC on
2026-09-09. [Final CI metadata](ci-final.json) and
[final sanitizer metadata](sanitizers-final.json) retain the exact head SHA,
job URLs, step results, and timestamps.

The earlier five-pass/one-failure result remains in
[old-2d3504a](../old-2d3504a/README.md), including its original failure log.
That diagnostic-line failure was not suppressed or retried to obtain green
results. The fix also added a concurrent-output test to the release and
sanitizer test inventories.

## PR readiness snapshot

At the 09:12 UTC check on 2026-09-09, [PR 91](https://github.com/makodb/mako/pull/91)
was open with head `a3ad9a110727f5ecc937cbeee23fe40152df719f`,
`mergeable=MERGEABLE`, and `mergeStateStatus=CLEAN`. All six entries in its
check rollup reported `SUCCESS`. See [the PR metadata](pr-final.json).

`gh pr checks 91 --required` returned exit code 1 with
["no required checks reported"](required-checks.txt). This snapshot therefore
records six successful hosted checks, not six branch-protection-required
checks. It records no approval or merge action. A later documentation-only
commit has its own head SHA and checks; these results apply to `a3ad9a1`.

The metadata came from read-only `gh run view` and `gh pr view` calls. No job
was rerun or canceled during monitoring. Verify the retained files from this
directory with:

```sh
sha256sum -c SHA256SUMS
```
