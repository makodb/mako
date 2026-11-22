# Private post-cut writer-scan bound proof

This is a private ordering proof and source review, not production sanitizer,
native, or performance qualification. It does not advance the earlier
acceptance checkpoints in the parent directory.

The proposed scan reads the process-lifetime registration high-water mark
after the existing packed Acquire cut. A prefix writer's Relaxed registration
happens before that cut through its packed Release publication. Atomic
write-read coherence then requires the later bound load to include its slot.
The allocator is capped and never decremented or reset. The bound is not an
active-worker count and can still reach 460 after worker churn.

[PROOF.md](PROOF.md) records the argument, actual call-path audit, reset and
reopen analysis, fixture obligations, and model limits.

- [report.json](report.json) records two passing Loom models and an expected
  failure when the bound load moves before the cut. Both positive models use
  the actual capped Relaxed allocator. They passed in 180.71 seconds with no
  preemption or permutation bound. The separate exact single-writer pass is
  repeated evidence, not an additional test.
- [integration-report.json](integration-report.json) records the two ordinary
  positive tests prepared for integration. They passed in 47.03 seconds.
  The two-writer model replaces retrying allocation with successful Relaxed
  RMWs under an explicit two-allocation, no-exhaustion invariant. The other
  model retains capped fetch_update. Exploration remains unbounded.
- [integration.patch](integration.patch) appends those two tests to the private
  candidate's existing three-model file. Its paths are relative to a crates
  workspace. It was checked against `.mako-read-bound-crates` but not applied
  when this bundle was recorded. No ignored negative case enters integration.
- `model-overlarge.rs` and `positive-overlarge-interrupted.log` preserve the
  first, interrupted model attempt. It is not counted as a pass.

All 16 source, dependency-lock, report, patch, and log files copied from
`/var/tmp/mako-fence-bound-model.deVBaQ` are byte-identical to their execution
artifacts. Report commands retain their original absolute paths. Source
hashes identify the production files inspected, not production files that
were executed by the standalone models. The models link Loom only.

The root agent separately tested the real private cache helper's callback
ordering and its before-cut source mutant. Those native checks are distinct
from this standalone bundle and must retain their own source provenance.
