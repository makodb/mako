//! Conditional administrative liveness over actual infinite placement traces.
//!
//! `service::theorem_service_eventual_completion` composes actual infinite
//! placement progress with terminating raw traversals, checked emissions, and
//! delivery fairness only for actually emitted records. `actual` retains the
//! abstract phase argument; `shape` derives its reachable-state invariants.
//! Transactions may independently commit/abort, and selected admissions remain
//! allowed until actual Freeze. Stable recovery must supply an exact reachable
//! placement projection/history; no production adapter or availability under
//! endless failures/arbitrary loss is claimed.
use vstd::prelude::*;
use crate::sharding_placement as p;
#[path = "sharding_progress_shape.rs"]
pub mod shape;
#[path = "sharding_progress_actual.rs"]
pub mod actual;
#[path = "sharding_progress_io.rs"]
pub mod io;
#[path = "sharding_progress_service.rs"]
pub mod service;
pub use service::theorem_service_eventual_completion;
pub use actual::{theorem_actual_eventual_completion, theorem_actual_commit_without_abort,
    theorem_recovered_terminal_eventual_completion};

verus! {

pub open spec fn selected_holders(s: p::State, g: nat) -> Set<int> {
    s.sessions.dom().filter(|t: int| exists|k: int|
        s.plans[g].keys.contains(k) && s.sessions[t].held.contains_key(k))
}

pub open spec fn completed(s: p::State, g: nat) -> bool {
    s.plans.contains_key(g) && p::terminal(s.phases[g]) && !p::current(s,g)
        && s.received.contains((g,p::Certificate::SourceDone))
        && s.received.contains((g,p::Certificate::DestinationDone))
        && s.replies.contains_key(s.plans[g].nonce)
        && s.replies[s.plans[g].nonce] == s.outcomes[s.plans[g].nonce]
}

/// Decision persistence is a placement property, not a scheduling assumption.
pub proof fn theorem_decision_never_reverts(c: p::Constants, s: p::State,
    a: p::Action, g: nat)
    requires p::inv(c,s), s.plans.contains_key(g), p::terminal(s.phases[g])
    ensures p::dispatch(c,s,a).phases[g] == s.phases[g],
        p::dispatch(c,s,a).plans.contains_key(g),
        p::dispatch(c,s,a).plans[g] == s.plans[g],
        p::dispatch(c,s,a).outcomes[s.plans[g].nonce] == s.outcomes[s.plans[g].nonce]
{
    if p::enabled(c,s,a) {
        match a {
            p::Action::Begin { .. } => {
                assert(p::plan_inv(c,s,g));
                assert(g < s.next_generation);
            },
            p::Action::RequestFreeze | p::Action::RequestFinal | p::Action::RequestRetire
            | p::Action::Commit | p::Action::Abort => {
                assert(s.active.unwrap() != g);
                if a is Commit || a is Abort {
                    assert(s.plans[s.active.unwrap()].nonce != s.plans[g].nonce);
                }
            },
            _ => {},
        }
    }
}

pub proof fn theorem_finish_requires_both_receipts(c: p::Constants, s: p::State, g: nat)
    requires p::current(s,g)
    ensures p::enabled(c,s,p::Action::Finish) <==>
        p::terminal(s.phases[g])
            && s.received.contains((g,p::Certificate::SourceDone))
            && s.received.contains((g,p::Certificate::DestinationDone)),
        (!s.received.contains((g,p::Certificate::SourceDone))
            || !s.received.contains((g,p::Certificate::DestinationDone))) ==>
            p::dispatch(c,s,p::Action::Finish) == s
{ }

pub open spec fn terminal_command(s: p::State, g: nat) -> p::Command {
    if s.phases[g] is Committed { p::Command::Commit } else { p::Command::Abort }
}

/// Internal lemma target, NOT a recovery assumption. Reachable bulk command
/// shape implies each participant is independently pre-application (guard) or
/// post-application (retained certificate). A merely phase-compatible ghost
/// state with mixed per-key terminality does not satisfy the reachable proof.
pub open spec fn cleanup_ready(s: p::State, g: nat) -> bool {
    p::current(s,g) && s.plans.contains_key(g) && p::terminal(s.phases[g])
        && s.commands.contains((g,terminal_command(s,g)))
        && (s.certificates.contains((g,p::Certificate::SourceDone))
            || p::local_guard(s,g,terminal_command(s,g),s.plans[g].src))
        && (s.certificates.contains((g,p::Certificate::DestinationDone))
            || p::local_guard(s,g,terminal_command(s,g),s.plans[g].dst))
}

} // verus!
