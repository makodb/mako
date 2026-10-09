//! Field-level coupling for the actual participant guards/effects. These lemmas
//! deliberately do not certify the native engine's mirror or deletion boundary.
use vstd::prelude::*;
use crate::participant::{guard_spec,effect_spec};
use crate::types::{ReplicaMeta,Role,Command};
use crate::sharding_placement as p;
use crate::ghost_log as log;
verus! {
pub open spec fn role(r: Role) -> p::Role {
    match r { Role::Empty => p::Role::Empty, Role::Serving => p::Role::Serving,
        Role::Frozen => p::Role::Frozen, Role::Retired => p::Role::Retired,
        Role::Stage => p::Role::Stage, Role::Ready => p::Role::Ready }
}
pub open spec fn command(c: Command) -> p::Command {
    match c { Command::Start => p::Command::Start, Command::Freeze => p::Command::Freeze,
        Command::Final => p::Command::Final, Command::Retire => p::Command::Retire,
        Command::Commit => p::Command::Commit, Command::Abort => p::Command::Abort }
}
pub open spec fn metadata(m: ReplicaMeta, r: p::Replica) -> bool {
    r.role == role(m.role) && r.epoch == m.epoch && r.fence == m.fence
        && r.terminal == m.terminal && r.round == m.round
}

pub proof fn effect_matches(m: ReplicaMeta, r: p::Replica, g: u64, c: Command, source: bool)
    requires metadata(m,r),
    ensures metadata(effect_spec(m,g,c,source),p::command_replica(r,g as nat,command(c),source)),
{ }

/// The interval walk must establish this pointwise premise for the actual
/// selected key set. No master phase or desired postcondition is a guard.
pub proof fn guard_matches(s: p::State, g: u64, c: Command, owner: int,
    local: spec_fn(int) -> ReplicaMeta)
    requires
        forall|k: int| s.plans[g as nat].keys.contains(k) ==> metadata(local(k),p::replica(s,owner,k)),
        forall|k: int| s.plans[g as nat].keys.contains(k) ==> guard_spec(local(k),
            s.plans[g as nat].old[k].epoch as u64,g,c,owner == s.plans[g as nat].src,
            owner == s.plans[g as nat].dst,p::drained(s,s.plans[g as nat].keys)),
        forall|k: int| s.plans[g as nat].keys.contains(k) ==> s.plans[g as nat].old[k].epoch <= u64::MAX,
    ensures p::local_guard(s,g as nat,command(c),owner),
{
    assert forall|k: int| s.plans[g as nat].keys.contains(k) implies {
        let r = p::replica(s,owner,k);
        match command(c) {
            p::Command::Start => owner == s.plans[g as nat].dst && r.role is Empty && r.fence < g,
            p::Command::Freeze => owner == s.plans[g as nat].src && r.role is Serving
                && r.epoch == s.plans[g as nat].old[k].epoch && r.fence < g,
            p::Command::Final => owner == s.plans[g as nat].dst && r.role is Stage
                && r.fence == g && !r.terminal && r.round == 0,
            p::Command::Retire => owner == s.plans[g as nat].src && r.role is Frozen
                && r.fence == g && !r.terminal && p::drained(s,s.plans[g as nat].keys),
            p::Command::Commit => (owner == s.plans[g as nat].dst && r.role is Ready
                || owner == s.plans[g as nat].src && r.role is Retired) && r.fence == g && !r.terminal,
            p::Command::Abort => (owner == s.plans[g as nat].src || owner == s.plans[g as nat].dst)
                && (r.fence < g || r.fence == g && !r.terminal),
        }
    } by { assert(metadata(local(k),p::replica(s,owner,k))); }
}

pub open spec fn metadata_writes(owner: int, key: int, m: ReplicaMeta) -> Seq<log::Write> {
    seq![log::Write::ReplicaRole { owner,key,role:role(m.role) },
        log::Write::ReplicaEpoch { owner,key,epoch:m.epoch as nat },
        log::Write::ReplicaFence { owner,key,fence:m.fence as nat },
        log::Write::ReplicaTerminal { owner,key,terminal:m.terminal },
        log::Write::ReplicaRound { owner,key,round:m.round as nat }]
}

/// The independent raw replay leaves native storage and coverage untouched.
/// Start/Final require the separate coverage/cell writes; Commit/Abort's Empty
/// abstraction must be closed only with the cleanup capability and certificate.
pub proof fn metadata_replay(s: p::State, owner: int, key: int, m: ReplicaMeta)
    requires s.physical.contains_key((owner,key)),
    ensures metadata(m,log::apply_writes(s,metadata_writes(owner,key,m)).physical[(owner,key)]),
        log::apply_writes(s,metadata_writes(owner,key,m)).physical[(owner,key)].cell == s.physical[(owner,key)].cell,
        log::apply_writes(s,metadata_writes(owner,key,m)).physical[(owner,key)].covered == s.physical[(owner,key)].covered,
        forall|q: (int,int)| q != (owner,key) ==> log::apply_writes(s,metadata_writes(owner,key,m)).physical[q] == s.physical[q],
{
    reveal_with_fuel(log::apply_writes,6);
}

/// Inputs are the byte/logical-key embedding and exact lease expansion, not an
/// assumed handler transition. `command_at` is established by real deliver's
/// local interval walk; neither this bridge nor the actor reads master phase.
pub proof fn native_guard_matches(actor: &crate::participant::Participant,
    plan: crate::types::MigrationPlan, c: Command, s: p::State, coordinates: spec_fn(int)->Seq<u8>)
    requires
        s.plans[plan.generation as nat].src == plan.source,
        s.plans[plan.generation as nat].dst == plan.destination,
        c == Command::Retire ==> p::drained(s,s.plans[plan.generation as nat].keys) == actor.drained_view(plan.range),
        forall|k: int| s.plans[plan.generation as nat].keys.contains(k) ==>
            actor.command_at(plan,c,actor.drained_view(plan.range),coordinates(k)),
        forall|k: int| s.plans[plan.generation as nat].keys.contains(k) ==>
            metadata(actor.local_meta(plan.range.table,coordinates(k)).unwrap(),p::replica(s,actor.owner_view() as int,k)),
        forall|k: int| s.plans[plan.generation as nat].keys.contains(k) ==>
            s.plans[plan.generation as nat].old[k].epoch == crate::directory::route(plan.old@,coordinates(k)).epoch,
    ensures p::local_guard(s,plan.generation as nat,command(c),actor.owner_view() as int),
{
    let local = |k: int| actor.local_meta(plan.range.table,coordinates(k)).unwrap();
    // Drain gates retirement only. New-owner transactions may already hold the
    // range during old-owner Commit cleanup; Abort may retain unresolved holders.
    assert forall|k:int| s.plans[plan.generation as nat].keys.contains(k) implies
        guard_spec(local(k),s.plans[plan.generation as nat].old[k].epoch as u64,
            plan.generation,c,actor.owner_view() == plan.source,
            actor.owner_view() == plan.destination,p::drained(s,s.plans[plan.generation as nat].keys)) by {
        match c {
            Command::Start => {},Command::Freeze => {},Command::Final => {},
            Command::Retire => {},Command::Commit => {},Command::Abort => {},
        }
    }
    guard_matches(s,plan.generation,c,actor.owner_view() as int,local);
}

/// A native range write supplies exactly these field values to the raw log.
/// Cells and covered bits are intentionally not claimed here: storage/mirror
/// code supplies their independent writes before the segment is closed.
pub proof fn native_effect_matches(before: &crate::participant::Participant,
    after: &crate::participant::Participant, plan: crate::types::MigrationPlan,
    c: Command, coordinate: Seq<u8>, r: p::Replica)
    requires after.control_frame(*before,plan,c),
        crate::bytes::contains_spec(plan.range,plan.range.table,coordinate),
        before.local_meta(plan.range.table,coordinate).is_some(),
        metadata(before.local_meta(plan.range.table,coordinate).unwrap(),r),
    ensures after.local_meta(plan.range.table,coordinate).is_some(),
        metadata(after.local_meta(plan.range.table,coordinate).unwrap(),
            p::command_replica(r,plan.generation as nat,command(c),before.owner_view() == plan.source)),
{
    effect_matches(before.local_meta(plan.range.table,coordinate).unwrap(),r,plan.generation,c,before.owner_view() == plan.source);
}
} // verus!
