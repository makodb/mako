# Changes to the group's Raft spec

Every change we make to the ghost-log-refinement Raft spec
(local branch `mako-spec` of `/home/users/zyang2/ghost-log-refinement`, checked
out in the worktree `/home/users/zyang2/glr-mako-spec`; from upstream `1bb2f142`
since v2, `d7e04ed7` before), under the two rules of
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
| v2 (export) | `raft-spec-v2-export` (= `8cc23b23`) | upstream `1bb2f142` (A10 snapshots, A11 crash and restart, A14's library with its persistence marks) + S1 + `pub mod protocol;` + patch 0003, which leaves the application ports out of the crate (see below); 17 hashed files, all equal to the group's Mako case study's at this base | 0.2026.08.02.b677dd5 | 1916 verified, 0 errors (78 s) | `$RESULTS/spec/verify_spec.raft-spec-v2-export.20261019T004108.log` |
| v3 (export) | `raft-spec-v3-export` (= `d87fbfa6`) | v2 + patch 0004: Raft's certificate instance (`CertProtocol for RaftP`) at the protocol level, `src/protocol/Raft/ghost_log_certs_raft.rs`, over copies of the raft-rs port's `HardState` (`LHardState`) and `rank` (`id_rank`); additive, no existing statement changes; the new file is hashed (18 files) | 0.2026.08.02.b677dd5 | 1917 verified, 0 errors (79 s) | `$RESULTS/spec/verify_spec.raft-spec-v3-export.20261019T015645.log` |

## v2: moving the base (2026-10-19)

**Why.** `d7e04ed7` predates the group's snapshot actions (A10, `dcaf5b16`,
`09e69009`) and its crash-and-restart and persistence work (A11 `e1f4df62`,
A14 H2 `321d083a`), which proving Mako's snapshot and persistence paths
needs. `1bb2f142` (2026-10-15) has all four, and the group's Mako case study
is proved against it with the same two patches (glr
`src/ports/mako/spec/SPEC_VERSION.toml`).

**Patch 0003 (build scope, no statement change).** At `1bb2f142` the raft-rs
port needs a vstd specification of `HashSet::with_capacity_and_hasher` that
the pinned Verus 0.2026.08.02 lacks (the group gated that commit on the
rolling Verus), so the whole crate stopped in `src/ports/raftrs/`. No
protocol module uses the ports, so 0003 compiles them out of the crate root;
the 17 hashed files are untouched. The verified count falls from 2129 to
1916 because the ports' items are gone, not because the spec lost proofs.

**The coupling (core) needed two ghost edits.** `LState` has an `outgoing`
field (A9-2's joint configuration): the state view sets it empty, and the
restore premise `restore_prev_ok` requires the restored state's to be empty
(true under the gate's static configuration). The commit quorum is now
`quorum_ok(replicator_set(..), config, outgoing)`: `lemma_commit_quorum`
shows the set is our filter, inside the configuration, with the outgoing half
stable. `scripts/verus/core_check.sh`: Verus 408 verified, 0 errors, the 366
listed functions all verified, ledger lint 0 unregistered.

## v3: the certificate instance (2026-10-19)

The persistence coupling (coupling-design-persistence-snapshots.md, Part A)
judges the core's ghost log with the library's certificates (`certs_ok`),
which need `CertProtocol` for `RaftP`. The only instance at `1bb2f142` is in
the raft-rs port, which patch 0003 compiles out, and Rust's orphan rule keeps
it out of Mako (glr is a separate crate). Patch 0004 states the same instance
in the protocol. It is abstract (the hard state and the id rank of Raft in
general, nothing of Mako) and correct (the crate verifies, 1917 items, 0
errors: the instance's one lemma is the new item). A crate that keeps the
ports would have two instances for `RaftP`; upstream would move the port onto
this one. The core proof is unchanged (408 verified).

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
