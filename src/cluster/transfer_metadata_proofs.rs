//! Source-certified participant metadata prefixes, separate from storage I/O.
//! A fresh deliver closes its metadata effect for every command, including the
//! semantic terminal certificate before cleanup. Empty masks retained private
//! bytes immediately; cleanup failure therefore cannot undo this prefix or turn
//! the encompassing handler into a stutter. Actual receipt emission is a second
//! event: only an observed successful ControlResult can mint its tracked witness.
use super::*;
use crate::participant::{ControlResult, Cleanup};
use crate::participant_proofs as metadata;
use crate::types::{Command, Certificate, ReplicaMeta, Status};

verus! {

/// Native metadata supplies the five metadata fields. Cells/coverage are
/// independent ghost projection effects, not values obtained from model.apply.
/// Start discards the destination's old private projection, Final starts a new
/// coverage round, and Empty hides bytes that the cleanup engine may still own.
pub open spec fn delivered_record(m: ReplicaMeta, old: p::Replica, command: Command) -> p::Replica {
    p::Replica {
        cell: if command is Start || m.role is Empty { p::empty_cell() } else { old.cell },
        role: metadata::role(m.role), epoch: m.epoch as nat, fence: m.fence as nat,
        terminal: m.terminal, round: m.round as nat,
        covered: if command is Start || command is Final { false } else { old.covered },
    }
}

/// Semantic availability and externally emitted completion are intentionally
/// different. No EmittedCertificate is created by this function or its caller.
pub open spec fn delivery_certificate(command: Command, source: bool) -> Option<p::Certificate> {
    match command {
        Command::Retire => Some(p::Certificate::Retired),
        Command::Commit | Command::Abort => Some(if source {
            p::Certificate::SourceDone
        } else { p::Certificate::DestinationDone }),
        _ => None,
    }
}

/// Full endpoint reconstructed from observed native metadata and its field frame.
pub open spec fn metadata_image(s:p::State,after:&Participant,plan:MigrationPlan,
    command:Command,keys:Seq<int>,labels:Map<int,Cell>) -> p::State {
    let owner = after.owner_view() as int;
    let records = Map::new(keys.to_set(),|k:int|
        delivered_record(after.local_meta(labels[k].0,labels[k].1.0).unwrap(),
            p::replica(s,owner,k),command));
    p::State {
        certificates:match delivery_certificate(command,after.owner_view() == plan.source) {
            Some(cert) => s.certificates.insert((plan.generation as nat,cert)),
            None => s.certificates,
        },
        ..transfer::terminal_state(s,owner,keys,records)
    }
}

proof fn delivered_record_matches(before: &Participant, after: &Participant,
    plan: MigrationPlan, command: Command, s: p::State, key: int, coordinate: Seq<u8>)
    requires
        after.control_frame(*before,plan,command),
        crate::bytes::contains_spec(plan.range,plan.range.table,coordinate),
        before.command_at(plan,command,before.drained_view(plan.range),coordinate),
        metadata::metadata(before.local_meta(plan.range.table,coordinate).unwrap(),
            p::replica(s,before.owner_view() as int,key)),
        s.physical.contains_key((before.owner_view() as int,key)),
    ensures
        after.local_meta(plan.range.table,coordinate).is_some(),
        delivered_record(after.local_meta(plan.range.table,coordinate).unwrap(),
            p::replica(s,before.owner_view() as int,key),command)
            == p::command_replica(p::replica(s,before.owner_view() as int,key),
                plan.generation as nat,metadata::command(command),before.owner_view() == plan.source),
{
    let owner = before.owner_view() as int;
    let old = p::replica(s,owner,key);
    metadata::native_effect_matches(before,after,plan,command,coordinate,old);
    let observed = after.local_meta(plan.range.table,coordinate).unwrap();
    // Field replay establishes that the observed native fields have no hidden
    // storage/coverage effect. delivered_record supplies those effects above.
    metadata::metadata_replay(s,owner,key,observed);
    let fields = log::apply_writes(s,metadata::metadata_writes(owner,key,observed));
    assert(metadata::metadata(observed,fields.physical[(owner,key)]));
    assert(fields.physical[(owner,key)].cell == old.cell);
    assert(fields.physical[(owner,key)].covered == old.covered);
    match command {
        Command::Start => {}, Command::Freeze => {}, Command::Final => {},
        Command::Retire => {}, Command::Commit => {}, Command::Abort => {},
    }
}

/// Call immediately after a *fresh successful Participant::deliver*, before any
/// cleanup I/O. The command set and immutable old-grant embedding authenticate
/// the input; command_at/control_frame are the real executable postconditions.
/// There is deliberately no storage-success premise and no caller-given replay
/// or model action. One immutable labels map is used on both sides.
pub proof fn metadata_delivered(c: p::Constants, before: &Participant, after: &Participant,
    plan: MigrationPlan, command: Command, result: &ControlResult,
    s: p::State, keys: Seq<int>, labels: Map<int,Cell>) -> (tracked out: ClosedTransfer)
    requires
        transfer::labels_cover(plan,s,keys,labels),
        transfer::local_coupling(before,plan,s,labels),
        result.status == Status::Ok, !before.command_seen(plan.generation,command),
        after.control_frame(*before,plan,command),
        after.owner_view() == before.owner_view(), after.lease_view() == before.lease_view(),
        forall|coordinate: Seq<u8>| crate::bytes::contains_spec(plan.range,plan.range.table,coordinate) ==>
            before.command_at(plan,command,before.drained_view(plan.range),coordinate),
        match command {
            Command::Start | Command::Final => before.owner_view() == plan.destination,
            Command::Freeze | Command::Retire => before.owner_view() == plan.source,
            Command::Commit | Command::Abort => before.owner_view() == plan.source || before.owner_view() == plan.destination,
        },
        command == Command::Retire ==> p::drained(s,s.plans[plan.generation as nat].keys) == before.drained_view(plan.range),
        s.commands.contains((plan.generation as nat,metadata::command(command))),
        forall|k: int| keys.to_set().contains(k) ==>
            s.physical.contains_key((before.owner_view() as int,k))
            && s.plans[plan.generation as nat].old[k].epoch == crate::directory::route(plan.old@,labels[k].1.0).epoch,
    ensures
        out.valid(c,s),
        out.after(s) == p::delivered(s,plan.generation as nat,metadata::command(command),before.owner_view() as int),
        transfer::local_coupling(after,plan,out.after(s),labels),
        out.after(s) == metadata_image(s,after,plan,command,keys,labels),
        out.after(s).plans == s.plans, out.after(s).sessions == s.sessions,
        match delivery_certificate(command,before.owner_view() == plan.source) {
            Some(cert) => out.after(s).certificates.contains((plan.generation as nat,cert)),
            None => out.after(s).certificates == s.certificates,
        },
{
    reveal(ClosedTransfer::valid);
    reveal(ClosedTransfer::after);
    let g = plan.generation as nat;
    let owner = before.owner_view() as int;
    let coordinates = |k: int| labels[k].1.0;
    assert forall|k: int| s.plans[g].keys.contains(k) implies
        before.command_at(plan,command,before.drained_view(plan.range),coordinates(k))
        && metadata::metadata(before.local_meta(plan.range.table,coordinates(k)).unwrap(),p::replica(s,owner,k))
        && s.plans[g].old[k].epoch == crate::directory::route(plan.old@,coordinates(k)).epoch by {
        assert(crate::storage::in_range(plan.range,labels[k]));
    }
    metadata::native_guard_matches(before,plan,command,s,coordinates);
    let records = Map::new(keys.to_set(),|k: int|
        delivered_record(after.local_meta(labels[k].0,labels[k].1.0).unwrap(),p::replica(s,owner,k),command));
    assert forall|k: int| keys.to_set().contains(k) implies
        after.local_meta(labels[k].0,labels[k].1.0).is_some()
        && records[k] == p::command_replica(p::replica(s,owner,k),g,metadata::command(command),before.owner_view() == plan.source) by {
        assert(crate::storage::in_range(plan.range,labels[k]));
        delivered_record_matches(before,after,plan,command,s,k,labels[k].1.0);
    }
    // The arbitrary-record replay lemma never refers to model delivery: its
    // inputs here are the records measured from actual post-deliver metadata.
    transfer::terminal_replay(s,owner,keys,records);
    let replica_writes = transfer::terminal_writes(owner,keys,records);
    let writes = match delivery_certificate(command,before.owner_view() == plan.source) {
        Some(cert) => {
            let write = log::Write::Certificate { generation:g,value:cert };
            log::append_write(s,replica_writes,write);
            replica_writes.push(write)
        },
        None => replica_writes,
    };
    let next = log::apply_writes(s,writes);
    let action = p::Action::Deliver { generation:g,command:metadata::command(command),owner };
    assert(next.physical =~= p::delivered(s,g,metadata::command(command),owner).physical) by {
        assert forall|q: (int,int)| s.physical.contains_key(q) implies
            next.physical[q] == p::delivered(s,g,metadata::command(command),owner).physical[q] by {
            if q.0 == owner && keys.to_set().contains(q.1) {
                assert(records[q.1] == p::command_replica(p::replica(s,owner,q.1),g,
                    metadata::command(command),before.owner_view() == plan.source));
            }
        }
    }
    assert(next == p::apply(c,s,action));
    log::accepted(c,s,next,action);
    assert forall|k: int| s.plans[g].keys.contains(k) implies
        after.local_meta(labels[k].0,labels[k].1.0).is_some()
        && metadata::metadata(after.local_meta(labels[k].0,labels[k].1.0).unwrap(),p::replica(next,owner,k)) by {
        assert(next.physical[(owner,k)] == records[k]);
    }
    ClosedTransfer { segment: log::Segment { writes,states:seq![s,next],actions:seq![action] } }
}

/// This certifies only the metadata-only duplicate/rejection branch. It never
/// consumes an outer transfer status and never claims that failed storage work
/// had no effects. Both unchanged local metadata and exact leases are required.
pub proof fn metadata_unchanged(c: p::Constants, before: &Participant, after: &Participant,
    plan: MigrationPlan, command: Command, result: &ControlResult,
    s: p::State, labels: Map<int,Cell>) -> (tracked out: ClosedTransfer)
    requires
        result.status != Status::Ok || before.command_seen(plan.generation,command),
        after.local_unchanged(*before), after.lease_view() == before.lease_view(),
        transfer::local_coupling(before,plan,s,labels),
    ensures out.valid(c,s), out.after(s) == s,
        transfer::local_coupling(after,plan,out.after(s),labels),
{
    reveal(ClosedTransfer::valid);
    reveal(ClosedTransfer::after);
    assert forall|k: int| s.plans[plan.generation as nat].keys.contains(k) implies
        after.local_meta(labels[k].0,labels[k].1.0).is_some()
        && metadata::metadata(after.local_meta(labels[k].0,labels[k].1.0).unwrap(),
            p::replica(s,after.owner_view() as int,k)) by {
        assert(after.local_meta(labels[k].0,labels[k].1.0) == before.local_meta(labels[k].0,labels[k].1.0));
    }
    log::stutter(c,s,s);
    ClosedTransfer { segment: log::Segment { writes:Seq::empty(),states:seq![s,s],actions:seq![p::Action::Stutter] } }
}

/// The exact lease expansion supplies drained; the native drain result supplies
/// source/Frozen/generation/nonterminal guards. Raw replay writes only the
/// certificate. drain_emitted below separately binds the real output witness.
pub proof fn drained(c: p::Constants, node: &Participant, plan: MigrationPlan,
    result: &ControlResult, s: p::State, keys: Seq<int>, labels: Map<int,Cell>)
    -> (tracked out: ClosedTransfer)
    requires
        result.status == Status::Ok, result.cleanup == Cleanup::None,
        result.certificate == Some(Certificate::Drained), node.capture_authorized(plan),
        transfer::labels_cover(plan,s,keys,labels), transfer::local_coupling(node,plan,s,labels),
        p::drained(s,s.plans[plan.generation as nat].keys) == node.drained_view(plan.range),
    ensures out.valid(c,s),
        out.after(s) == (p::State { certificates:s.certificates.insert((plan.generation as nat,p::Certificate::Drained)),..s }),
        transfer::local_coupling(node,plan,out.after(s),labels),
{
    reveal(ClosedTransfer::valid);
    reveal(ClosedTransfer::after);
    let g = plan.generation as nat;
    assert forall|k: int| s.plans[g].keys.contains(k) implies {
        let replica = p::replica(s,plan.source as int,k);
        replica.role is Frozen && replica.fence == g && !replica.terminal
    } by {
        assert(crate::storage::in_range(plan.range,labels[k]));
        assert(node.range_role(plan,crate::types::Role::Frozen,None,false));
        assert(metadata::metadata(node.local_meta(labels[k].0,labels[k].1.0).unwrap(),p::replica(s,plan.source as int,k)));
    }
    let write = log::Write::Certificate { generation:g,value:p::Certificate::Drained };
    log::single_write(s,write);
    let next = log::apply_write(s,write);
    let action = p::Action::Drain { generation:g };
    log::accepted(c,s,next,action);
    ClosedTransfer { segment: log::Segment { writes:seq![write],states:seq![s,next],actions:seq![action] } }
}

/// Real successful drain output, not mere semantic certificate availability.
pub proof fn drain_emitted(node: &Participant, plan: MigrationPlan, result: &ControlResult,
    s: p::State) -> (tracked out: EmittedCertificate)
    requires
        node.capture_authorized(plan), result.status == Status::Ok,
        result.cleanup == Cleanup::None, result.certificate == Some(Certificate::Drained),
        s.plans.contains_key(plan.generation as nat), s.plans[plan.generation as nat].src == plan.source,
        s.certificates.contains((plan.generation as nat,p::Certificate::Drained)),
    ensures out.matches(plan.generation,node.owner_view(),Certificate::Drained),
{
    reveal(EmittedCertificate::matches);
    EmittedCertificate { generation:plan.generation,owner:node.owner_view(),certificate:Certificate::Drained }
}

/// Retire emits immediately; no storage capability is needed. For duplicates,
/// semantic availability comes from the earlier metadata prefix, not a new
/// Deliver action. The observed return still has to contain the certificate.
pub proof fn retired_emitted(node: &Participant, plan: MigrationPlan, command: Command,
    result: &ControlResult, s: p::State) -> (tracked out: EmittedCertificate)
    requires
        command == Command::Retire, node.owner_view() == plan.source,
        result.status == Status::Ok, result.cleanup == Cleanup::None,
        result.certificate == Some(Certificate::Retired),
        s.plans.contains_key(plan.generation as nat), s.plans[plan.generation as nat].src == plan.source,
        s.certificates.contains((plan.generation as nat,p::Certificate::Retired)),
    ensures out.matches(plan.generation,node.owner_view(),Certificate::Retired),
{
    reveal(EmittedCertificate::matches);
    EmittedCertificate { generation:plan.generation,owner:node.owner_view(),certificate:Certificate::Retired }
}

/// Only Commit-at-destination and Abort-at-source can emit here. In particular,
/// cleanup-required terminal commands cannot use this constructor even when a
/// duplicate result eventually reports cleanup=None after physical completion.
pub proof fn noncleanup_terminal_emitted(node: &Participant, plan: MigrationPlan, command: Command,
    result: &ControlResult, s: p::State) -> (tracked out: EmittedCertificate)
    requires
        plan.source != plan.destination,
        (command == Command::Commit && node.owner_view() == plan.destination)
            || (command == Command::Abort && node.owner_view() == plan.source),
        result.status == Status::Ok, result.cleanup == Cleanup::None,
        result.certificate == Some(if node.owner_view() == plan.source { Certificate::SourceDone } else { Certificate::DestinationDone }),
        s.plans.contains_key(plan.generation as nat),
        s.plans[plan.generation as nat].src == plan.source, s.plans[plan.generation as nat].dst == plan.destination,
        s.certificates.contains((plan.generation as nat,if node.owner_view() == plan.source {
            p::Certificate::SourceDone
        } else { p::Certificate::DestinationDone })),
    ensures out.matches(plan.generation,node.owner_view(),if node.owner_view() == plan.source {
        Certificate::SourceDone
    } else { Certificate::DestinationDone }),
{
    reveal(EmittedCertificate::matches);
    EmittedCertificate { generation:plan.generation,owner:node.owner_view(),
        certificate:if node.owner_view() == plan.source { Certificate::SourceDone } else { Certificate::DestinationDone } }
}

} // verus!
