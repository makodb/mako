# Changes to the group's Raft spec

Every change we make to the ghost-log-refinement Raft spec
(`/home/users/zyang2/ghost-log-refinement`, local branch `mako-spec` from
upstream `d7e04ed7`), under the two rules of
[modification-plan.md](modification-plan.md) 0.1:

1. **Abstract**: the action describes Raft behaviour in general, true of any
   correct implementation; no Mako code path, constant or quirk.
2. **Correct**: the group's safety proof and composition theorem still verify
   with the full-crate command (plan 0.5), with at least as many verified
   items and 0 errors.

The patch series and the frozen manifest live in
`src/deptran/raft/verus/spec/`; `scripts/verus/verify_spec.sh` re-checks both.

## Versions

| Version | Tag on `mako-spec` | Content | Verus | Verified | Log |
|---|---|---|---|---|---|
| v0 | `raft-spec-v0` (= `d7e04ed7`) | upstream, unmodified | 0.2026.08.02.b677dd5 | 2129 verified, 0 errors (5 min 26 s, 6.7 GB) | `$RESULTS/spec/v0-pin.log` |
| v1 | `raft-spec-v1` (= `a19122cf`) | v0 + S1 | 0.2026.08.02.b677dd5 | 2129 verified, 0 errors (5 min 24 s, 6.7 GB) | `$RESULTS/spec/v1-pin.log` |
| v1 (export) | `raft-spec-v1-export` (= `c0c242ab`) | v1 + `pub mod protocol;` in the crate root (patch 0002): visibility only, so the core can import the spec (plan §4.5, spike (d) choice (i)). No statement-bearing file changes, so the version stays v1: the manifest's ten sha256 are unchanged | 0.2026.08.02.b677dd5 | 2129 verified, 0 errors (5 min 9 s) | `$RESULTS/spec/verify_spec.v1-export.log` |

## Changes

| # | Change | Why it is still Raft | Abstractness self-check | Verified | Log |
|---|---|---|---|---|---|
| S1 | `LRejectAppendEntries` loses its guard: any server may refuse any AppendEntries; state unchanged; one `AppendResponse{term: own, success: false, match_index: 0, read_ctx: 0}` | A refusal is observationally a loss plus an unsuccessful reply, which the network can already produce; the leader can only move `next_index` on it, which `LHandleAppendReject` treats as unconstrained bookkeeping. The group made the same argument for `LRejectVote` and `LStepAside`. raft.tla's three reasons remain admitted. | Names no Mako identifier, constant or code path; phrased over spec fields only ("any server, in any state, may refuse"); covers raft-rs and etcd equally. | 2129 verified, 0 errors (= v0) | `$RESULTS/spec/v1-pin.log` |

## Notes

- **S1's proof maintenance is a hint, not an rlimit bump.** The plan proposed
  raising `lemma_follower_commit_prefix_advanced_common`'s rlimit 150 → 400
  (from a run under the rolling Verus). Under the pinned release that is not
  enough: full crate 2128 verified, 1 error at 400 and again at 1000; the
  module alone at 1000 also fails (26 verified, 1 error, 818 s). The lemma's
  context holds `LNextAtomic`, whose reject disjunct is now just
  `s_ == s ∧ sent == [refusal]`; it is refutable because the lemma requires
  the commit index to change, but Z3 did not find that. One line,
  `assert(s_ != s)`, states it; with it the original rlimit 150 holds (full
  crate at 150: 2129 verified, 0 errors; the module alone at 400: 27
  verified in 50 s). Function-only
  runs (`--verify-function`) passed even at 400 without the hint and were
  misleading: they check in a different context.
- Rule 3 (no new `assume`/`admit`/`external_body`/`verifier::external` in
  `src/protocol/Raft`): the grep over `d7e04ed7..mako-spec` prints nothing.
- Hashing is by whole file (plan §4.5 left the choice to Phase 0): simpler,
  and no edits to those files are expected outside numbered S-changes.
