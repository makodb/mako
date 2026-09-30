// Verus model of the leader's commit rule, checked against the Rust Raft core.
//
// The exec functions below keep the control flow of the production code
// line for line, with the production types reduced to what the rule reads:
//   majority_match_index    <- src/deptran/raft/src/server_h.rs:881-903
//   commit_index_candidate  <- src/deptran/raft/src/server_h.rs:313-325
//   commit_advance          <- src/deptran/raft/src/server_cc.rs:633-666
// Reductions: PeerTable's progress_ is a Vec of match indexes (the only field
// the rule reads); the log is a Vec of entry terms, entry k (1-based) at
// terms[k - 1], so last_index() == terms.len(); the panics become
// preconditions.
//
// What is proved (Raft's safety conditions for advancing commitIndex, Raft
// paper Figure 2, "Rules for Servers / Leaders", last bullet):
//   * the commit index never decreases;
//   * a new commit index N is in the log (N <= last index);
//   * log[N].term == currentTerm;
//   * a majority of the cluster has N: the leader (which holds every entry up
//     to its last index) plus at least nservers/2 followers with
//     matchIndex >= N.
//
// Run: verus src/deptran/raft/verus/commit_rule.rs

use vstd::prelude::*;

verus! {

// Number of followers in s[0..k) whose match index is >= v / < v.
pub open spec fn count_ge(s: Seq<u64>, v: u64, k: int) -> nat
    decreases k,
{
    if k <= 0 { 0 } else { count_ge(s, v, k - 1) + if s[k - 1] >= v { 1nat } else { 0nat } }
}

pub open spec fn count_lt(s: Seq<u64>, v: u64, k: int) -> nat
    decreases k,
{
    if k <= 0 { 0 } else { count_lt(s, v, k - 1) + if s[k - 1] < v { 1nat } else { 0nat } }
}

proof fn lemma_ge_lt_partition(s: Seq<u64>, v: u64, k: int)
    requires 0 <= k,
    ensures count_ge(s, v, k) + count_lt(s, v, k) == k,
    decreases k,
{
    if k > 0 { lemma_ge_lt_partition(s, v, k - 1); }
}

// Every follower has match index >= 0.
proof fn lemma_all_ge_zero(s: Seq<u64>, k: int)
    requires 0 <= k,
    ensures count_ge(s, 0, k) == k,
    decreases k,
{
    if k > 0 { lemma_all_ge_zero(s, k - 1); }
}

// server_h.rs:881-903: pick the match index whose rank (ties broken by
// position) is (nservers - 1) / 2.
pub fn majority_match_index(progress: &Vec<u64>, nservers: usize, last_log_index: u64) -> (r: u64)
    requires
        nservers >= 1,
        progress.len() == nservers - 1,
    ensures
        r <= last_log_index,
        nservers > 1 ==> count_ge(progress@, r, progress.len() as int) >= progress.len() - (nservers - 1) / 2,
{
    let target = (nservers - 1) / 2;
    let n = progress.len();
    let mut selected: u64 = 0;
    proof { lemma_all_ge_zero(progress@, n as int); }
    let mut i: usize = 0;
    while i < n
        invariant
            n == progress.len(), target == (nservers - 1) / 2, i <= n,
            count_ge(progress@, selected, n as int) >= n - target,
        decreases n - i,
    {
        let value = progress[i];
        let mut rank: usize = 0;
        let ghost mut lt: nat = 0;
        let mut j: usize = 0;
        while j < n
            invariant
                n == progress.len(), i < n, j <= n, value == progress@[i as int],
                lt == count_lt(progress@, value, j as int),
                lt <= rank, rank <= j,
            decreases n - j,
        {
            let other = progress[j];
            if other < value || (other == value && j < i) {
                rank += 1;
            }
            proof { lt = lt + if other < value { 1nat } else { 0nat }; }
            j += 1;
        }
        if rank == target {
            proof { lemma_ge_lt_partition(progress@, value, n as int); }
            selected = value;
        }
        i += 1;
    }
    let r = commit_index_candidate(selected, nservers, last_log_index);
    proof {
        if nservers > 1 { lemma_count_ge_antitone(progress@, r, selected, n as int); }
    }
    r
}

// server_h.rs:313-325.
pub fn commit_index_candidate(selected_match: u64, nservers: usize, last_log_index: u64) -> (r: u64)
    ensures
        r <= last_log_index,
        nservers > 1 ==> r <= selected_match,
{
    let mut candidate = last_log_index;
    if nservers > 1 {
        candidate = selected_match;
    }
    if candidate > last_log_index { last_log_index } else { candidate }
}

// count_ge is antitone in v: lowering the threshold keeps every follower.
proof fn lemma_count_ge_antitone(s: Seq<u64>, lo: u64, hi: u64, k: int)
    requires 0 <= k, lo <= hi,
    ensures count_ge(s, lo, k) >= count_ge(s, hi, k),
    decreases k,
{
    if k > 0 { lemma_count_ge_antitone(s, lo, hi, k - 1); }
}

pub struct CommitAdvance {
    pub advanced: bool,
    pub from: u64,
    pub to: u64,
}

// server_cc.rs:633-666. `terms` is the leader's log (entry k at terms[k-1]);
// `commit_index` is RaftConsensusState::commit_index_, updated in place.
pub fn commit_advance(
    progress: &Vec<u64>,
    terms: &Vec<u64>,
    commit_index: &mut u64,
    current_term: u64,
    nservers: usize,
) -> (out: CommitAdvance)
    requires
        nservers >= 1,
        progress.len() == nservers - 1,          // the first panic, as a precondition
        *old(commit_index) <= terms.len(),
    ensures
        *final(commit_index) >= *old(commit_index),     // never decreases
        *final(commit_index) <= terms.len(),
        !out.advanced ==> *final(commit_index) == *old(commit_index),
        out.advanced ==> out.from == *old(commit_index) && out.to == *final(commit_index)
            && *final(commit_index) > *old(commit_index)
            && terms@[*final(commit_index) - 1] == current_term                 // current-term entry
            && (nservers > 1 ==> count_ge(progress@, *final(commit_index), progress.len() as int) + 1
                                 >= nservers / 2 + 1),                   // majority incl. leader
{
    let candidate_index = majority_match_index(progress, nservers, terms.len() as u64);
    if !(candidate_index > *commit_index) {
        return CommitAdvance { advanced: false, from: 0, to: 0 };
    }
    // candidate <= last index and > commit_index >= 0, so the entry exists:
    // the second panic ("committable index is absent") is unreachable.
    let entry_term = terms[(candidate_index - 1) as usize];
    if entry_term != current_term {
        return CommitAdvance { advanced: false, from: 0, to: 0 };
    }
    let from = *commit_index;
    *commit_index = candidate_index;
    proof {
        if nservers > 1 {
            // followers with match >= candidate: at least (n-1) - (n-1)/2,
            // and with the leader that is floor(n/2) + 1.
            assert(count_ge(progress@, candidate_index, progress.len() as int)
                   >= progress.len() - (nservers - 1) / 2);
        }
    }
    CommitAdvance { advanced: true, from, to: candidate_index }
}

fn main() {}

} // verus!
