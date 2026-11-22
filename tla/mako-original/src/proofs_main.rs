//! The top-level theorems over whole behaviors: the invariant holds in every
//! reachable state, hence every reachable state's acknowledged history is
//! strictly serializable, and the paper's durability, atomicity (Theorem 1)
//! and rollback-safety (Theorem 2) statements hold.
use super::types::*;
use super::normal::*;
use super::recovery::*;
use super::behavior::*;
use super::history::*;
use super::invariants::*;
use super::proofs_basic::*;
use super::proofs_normal1::*;
use super::proofs_prepare::*;
use super::proofs_install::*;
use super::proofs_certify::*;
use super::proofs_crash::*;
use super::proofs_advance::*;
use super::proofs_closeroll::*;
use super::proofs_history::*;
use vstd::prelude::*;

verus! {

/// Every enabled action preserves the invariant.
pub proof fn lemma_step_inv(s: State, c: Constants, a: Action)
    requires inv(s, c), enabled(s, c, a)
    ensures inv(apply(s, c, a), c)
{
    match a {
        Action::Submit { id, body, coord, thread } => lemma_submit_inv(s, c, id, body, coord, thread),
        Action::Read { id, key } => lemma_read_inv(s, c, id, key),
        Action::Prepare { id, vc } => lemma_prepare_inv(s, c, id, vc),
        Action::Install { id, shard } => lemma_install_inv(s, c, id, shard),
        Action::Certify { id } => lemma_certify_inv(s, c, id),
        Action::Commit { id } => lemma_commit_inv(s, c, id),
        Action::Abort { id } => lemma_abort_inv(s, c, id),
        Action::Replicate { sid } => lemma_replicate_inv(s, c, sid),
        Action::Crash { shard, survive } => lemma_crash_inv(s, c, shard, survive),
        Action::AdvanceEpoch { shard } => lemma_advance_inv(s, c, shard),
        Action::CloseEpoch { shard, epoch, wm } => lemma_close_inv(s, c, shard, epoch, wm),
        Action::Rollback { shard, epoch } => lemma_rollback_inv(s, c, shard, epoch),
        Action::Stutter => lemma_stutter_inv(s, c),
    }
}

pub proof fn lemma_next_inv(s: State, s_: State, c: Constants)
    requires inv(s, c), next(s, s_, c)
    ensures inv(s_, c)
{
    let a = choose|a: Action| #[trigger] enabled(s, c, a) && s_ == apply(s, c, a);
    lemma_step_inv(s, c, a);
}

/// The invariant holds in every state of every behavior.
pub proof fn theorem_invariant(states: Seq<State>, c: Constants, i: int)
    requires behavior(states, c), 0 <= i < states.len()
    ensures inv(states[i], c)
    decreases i
{
    if i == 0 {
        lemma_init_inv(states[0], c);
    } else {
        theorem_invariant(states, c, i - 1);
        let p = i - 1;
        assert(next(states[p], states[p + 1], c));
        assert(states[p + 1] == states[i]);
        lemma_next_inv(states[i - 1], states[i], c);
    }
}

/// Main theorem: in every reachable state, the history of acknowledged
/// transactions is strictly serializable.
pub proof fn theorem_mako_strictly_serializable(states: Seq<State>, c: Constants, i: int)
    requires behavior(states, c), 0 <= i < states.len()
    ensures strictly_serializable(states[i])
{
    theorem_invariant(states, c, i);
    theorem_strictly_serializable(states[i], c);
}

} // verus!
