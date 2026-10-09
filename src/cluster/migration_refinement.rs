//! Master-field refinement of the same-source contracts in migration.rs.
//!
//! Raw writes below are selected from actual assignments, independently of the
//! placement dispatcher. Closing a segment proves both the native field image
//! and its placement step. Receipt authenticity is an explicit premise supplied
//! by verified participant issuance plus the authenticated RPC boundary; owner
//! checking alone does not establish issuance. The node pool is fixed.
//!
//! Byte observations use a finite support containing the observed coordinates
//! and interval endpoints. A nonempty abstract range includes an empty-cell
//! observation at its lower endpoint even when no physical row currently exists.
use vstd::prelude::*;
use crate::migration as n;
use crate::types::{Grant, TxnId, MigrationPlan, Command, Certificate, Phase, Outcome, Status};
use crate::directory::{route, inside, proper};
use crate::directory_partition as d;
use crate::sharding_partition as partition;
use crate::sharding_placement as p;
use crate::ghost_log as log;

verus! {
pub open spec fn nonce(n: TxnId) -> int { n.client as int * 18446744073709551616 + n.sequence as int }
pub proof fn nonce_injective(a: TxnId,b: TxnId)
    ensures nonce(a) >= 0, (nonce(a) == nonce(b)) == n::same_nonce(a,b),
{
    if a.client < b.client { assert(nonce(a) < nonce(b)); }
    if b.client < a.client { assert(nonce(b) < nonce(a)); }
}
pub open spec fn grant(g: Grant) -> p::Grant { p::Grant { owner: g.owner as int, epoch: g.epoch as nat } }
pub open spec fn phase(q: Phase) -> p::Phase {
    match q { Phase::Copy => p::Phase::Copy, Phase::Freezing => p::Phase::Freezing,
        Phase::Final => p::Phase::Final, Phase::Retiring => p::Phase::Retiring,
        Phase::Committed => p::Phase::Committed, Phase::Aborted => p::Phase::Aborted }
}
pub open spec fn command(q: Command) -> p::Command {
    match q { Command::Start => p::Command::Start, Command::Freeze => p::Command::Freeze,
        Command::Final => p::Command::Final, Command::Retire => p::Command::Retire,
        Command::Commit => p::Command::Commit, Command::Abort => p::Command::Abort }
}
pub open spec fn certificate(q: Certificate) -> p::Certificate {
    match q { Certificate::Drained => p::Certificate::Drained, Certificate::Ready => p::Certificate::Ready,
        Certificate::Retired => p::Certificate::Retired, Certificate::SourceDone => p::Certificate::SourceDone,
        Certificate::DestinationDone => p::Certificate::DestinationDone }
}
pub open spec fn outcome(q: Outcome) -> p::Outcome { p::Outcome { generation: q.generation as nat, committed: q.committed } }
pub open spec fn action(r: n::Request) -> p::Action {
    match r { n::Request::Freeze => p::Action::RequestFreeze, n::Request::Final => p::Action::RequestFinal,
        n::Request::Retire => p::Action::RequestRetire, n::Request::Commit => p::Action::Commit,
        n::Request::Abort => p::Action::Abort, n::Request::Finish => p::Action::Finish }
}
pub open spec fn active(v: n::Native) -> Option<nat> {
    match v.active { Some(i) => Some(v.records[i as int].plan.generation as nat), None => None }
}
pub open spec fn controls_at(v: n::Native,s: p::State,i: int) -> bool {
    let rec = v.records[i]; let ctl = v.controls[i]; let g = rec.plan.generation as nat;
    s.plans.contains_key(g) && s.phases.contains_key(g)
    && s.plans[g].nonce == nonce(rec.plan.nonce) && s.plans[g].src == rec.plan.source && s.plans[g].dst == rec.plan.destination
    && s.phases[g] == phase(ctl.phase)
    && (forall|c: Command| s.commands.contains((g,command(c))) == ctl.issued.has(c))
    && (forall|c: Certificate| s.received.contains((g,certificate(c))) == ctl.received.has(c))
    && (match ctl.outcome { Some(o) => s.outcomes.contains_key(nonce(rec.plan.nonce)) && s.outcomes[nonce(rec.plan.nonce)] == outcome(o),
        None => !s.outcomes.contains_key(nonce(rec.plan.nonce)) })
    && (ctl.replied ==> ctl.outcome is Some && s.replies.contains_key(nonce(rec.plan.nonce))
        && s.replies[nonce(rec.plan.nonce)] == outcome(ctl.outcome.unwrap()))
    && (!ctl.replied ==> !s.replies.contains_key(nonce(rec.plan.nonce)))
}
/// This relation covers all retained records, not just the active migration.
/// Other actors' certificates/replicas/sessions are deliberately not projected
/// from coordinator receipt bits. The raw frame preserves them exactly.
pub open spec fn master(v: n::Native,s: p::State) -> bool {
    s.next_generation == v.next_generation && s.active == active(v)
    && (forall|i: int| 0 <= i < v.records.len() ==> controls_at(v,s,i))
    && (forall|g: nat| s.plans.contains_key(g) == (exists|i: int| 0 <= i < v.records.len() && v.records[i].plan.generation == g))
    && s.phases.dom() == s.plans.dom()
    && (forall|g: nat,c: p::Command| s.commands.contains((g,c)) ==> s.plans.contains_key(g))
    && (forall|g: nat,c: p::Certificate| s.received.contains((g,c)) ==> s.plans.contains_key(g))
    && (forall|id: int| s.outcomes.contains_key(id) ==> exists|i: int| 0 <= i < v.records.len() && nonce(v.records[i].plan.nonce) == id)
    && (forall|id: int| s.replies.contains_key(id) ==> exists|i: int| 0 <= i < v.records.len() && nonce(v.records[i].plan.nonce) == id)
}

/// Native snapshot observations and a fixed finite byte-order interpretation.
pub struct Observations {
    pub bytes: Map<int,Seq<u8>>,
    pub slots: Map<int,usize>,
    pub support: Seq<Seq<u8>>,
}
pub open spec fn observes(c: p::Constants,v: n::Native,o: Observations) -> bool {
    o.bytes.dom() == c.keys && o.slots.dom() == c.keys && d::sorted(o.support)
    && c.table.dom() == c.keys && c.coordinate.dom() == c.keys
    && (forall|k: int| c.keys.contains(k) ==> {
        let slot = o.slots[k];
        slot < v.current.len() && o.support.contains(o.bytes[k])
        && c.table[k] == v.snapshots[v.current[slot as int] as int].table
        && c.coordinate[k] == d::rank(o.support,o.bytes[k])
    })
}
pub open spec fn directory(c: p::Constants,v: n::Native,o: Observations) -> Map<int,p::Grant> {
    Map::new(c.keys,|k: int| grant(route(v.snapshots[v.current[o.slots[k] as int] as int].boundaries@,o.bytes[k])))
}
pub open spec fn range_observed(o: Observations,r: crate::types::KeyRange) -> bool {
    o.support.contains(r.lo@) && match n::upper(r) { Some(h) => o.support.contains(h), None => true }
}
pub open spec fn selected(c: p::Constants,o: Observations,r: crate::types::KeyRange) -> Set<int> {
    p::selected(c,r.table as int,d::rank(o.support,r.lo@),d::image_hi(o.support,n::upper(r)))
}
pub proof fn range_observation(c: p::Constants,v: n::Native,o: Observations,r: crate::types::KeyRange,k: int)
    requires n::layout(v), observes(c,v,o), range_observed(o,r), c.keys.contains(k),
    ensures selected(c,o,r).contains(k) == (c.table[k] == r.table && inside(o.bytes[k],r.lo@,n::upper(r))),
{
    d::rank_order(o.support,r.lo@,o.bytes[k]);
    if let Some(h) = n::upper(r) { d::rank_order(o.support,o.bytes[k],h); }
}
pub proof fn valid_range_observation(o: Observations,r: crate::types::KeyRange)
    requires d::sorted(o.support), range_observed(o,r), proper(r.lo@,n::upper(r)),
    ensures partition::valid_range(d::rank(o.support,r.lo@),d::image_hi(o.support,n::upper(r))),
{
    d::rank_order(o.support,r.lo@,r.lo@);
    if let Some(h) = n::upper(r) { d::rank_order(o.support,r.lo@,h); }
}
pub open spec fn prepared(v: n::Native,i: int) -> bool {
    let rec = v.records[i]; let r = rec.plan.range;
    v.snapshots[rec.previous as int].table == r.table && v.snapshots[rec.proposal as int].table == r.table
    && proper(r.lo@,n::upper(r))
    && crate::directory_proofs::equivalent(rec.plan.old@,v.snapshots[rec.previous as int].boundaries@)
    && (forall|k: Seq<u8>| route(v.snapshots[rec.proposal as int].boundaries@,k) ==
        if inside(k,r.lo@,n::upper(r)) { Grant { owner: rec.plan.destination, epoch: rec.plan.generation } }
        else { route(v.snapshots[rec.previous as int].boundaries@,k) })
}
pub proof fn begin_prepares(b: n::Native,z: n::Native,id: TxnId,src: u32,dst: u32,r: crate::types::KeyRange,g: u64)
    requires n::layout(b), n::begin_effect(b,z,id,src,dst,r,Ok(g)),
    ensures prepared(z,b.records.len() as int),
{}

/// The scalar partition theorem is applied to the actual immutable proposal,
/// never used as an executable replacement or as a handler's assumed result.
pub proof fn proposal_observation(v: n::Native,i: int,k: Seq<u8>)
    requires n::layout(v), 0 <= i < v.records.len(), prepared(v,i),
    ensures ({
        let rec = v.records[i]; let r = rec.plan.range;
        let old = v.snapshots[rec.previous as int].boundaries@;
        let new = v.snapshots[rec.proposal as int].boundaries@;
        let support = d::support(d::observation_keys(old,new,r.lo@,n::upper(r),k));
        partition::route(d::image(new,support),d::rank(support,k)) == partition::route(
            partition::reassign(d::image(old,support),d::rank(support,r.lo@),d::image_hi(support,n::upper(r)),
                d::grant_id(Grant { owner: rec.plan.destination, epoch: rec.plan.generation })),d::rank(support,k))
    }),
{
    let rec = v.records[i]; let r = rec.plan.range;
    d::replacement_observation(v.snapshots[rec.previous as int].boundaries@,v.snapshots[rec.proposal as int].boundaries@,
        r.lo@,n::upper(r),Grant { owner: rec.plan.destination, epoch: rec.plan.generation },k);
}

pub open spec fn transition_writes(b: n::Native,z: n::Native,r: n::Request,snapshot: Map<int,p::Grant>,status: Status) -> Seq<log::Write> {
    if status != Status::Ok { Seq::empty() } else {
        let i = b.active.unwrap() as int; let rec = b.records[i]; let g = rec.plan.generation as nat;
        match r {
            n::Request::Freeze => seq![log::Write::Phase { generation:g,value:phase(z.controls[i].phase) }, log::Write::Command { generation:g,value:p::Command::Freeze }],
            n::Request::Final => seq![log::Write::Phase { generation:g,value:phase(z.controls[i].phase) }, log::Write::Command { generation:g,value:p::Command::Final }],
            n::Request::Retire => seq![log::Write::Phase { generation:g,value:phase(z.controls[i].phase) }, log::Write::Command { generation:g,value:p::Command::Retire }],
            n::Request::Commit => seq![log::Write::DirectoryAppend { snapshot }, log::Write::Phase { generation:g,value:phase(z.controls[i].phase) },
                log::Write::Command { generation:g,value:p::Command::Commit }, log::Write::Outcome { nonce:nonce(rec.plan.nonce),value:outcome(z.controls[i].outcome.unwrap()) }],
            n::Request::Abort => seq![log::Write::Phase { generation:g,value:phase(z.controls[i].phase) }, log::Write::Command { generation:g,value:p::Command::Abort },
                log::Write::Outcome { nonce:nonce(rec.plan.nonce),value:outcome(z.controls[i].outcome.unwrap()) }],
            n::Request::Finish => seq![log::Write::Active { generation:active(z) }],
        }
    }
}
pub open spec fn transition_image(b: n::Native,z: n::Native,s: p::State,r: n::Request,snapshot: Map<int,p::Grant>,status: Status) -> p::State {
    if status != Status::Ok { s } else {
        let i = b.active.unwrap() as int; let rec = b.records[i]; let g = rec.plan.generation as nat;
        match r {
            n::Request::Freeze => p::State { phases:s.phases.insert(g,phase(z.controls[i].phase)),commands:s.commands.insert((g,p::Command::Freeze)),..s },
            n::Request::Final => p::State { phases:s.phases.insert(g,phase(z.controls[i].phase)),commands:s.commands.insert((g,p::Command::Final)),..s },
            n::Request::Retire => p::State { phases:s.phases.insert(g,phase(z.controls[i].phase)),commands:s.commands.insert((g,p::Command::Retire)),..s },
            n::Request::Commit => p::State { directory:s.directory.push(snapshot),phases:s.phases.insert(g,phase(z.controls[i].phase)),commands:s.commands.insert((g,p::Command::Commit)),
                outcomes:s.outcomes.insert(nonce(rec.plan.nonce),outcome(z.controls[i].outcome.unwrap())),..s },
            n::Request::Abort => p::State { phases:s.phases.insert(g,phase(z.controls[i].phase)),commands:s.commands.insert((g,p::Command::Abort)),
                outcomes:s.outcomes.insert(nonce(rec.plan.nonce),outcome(z.controls[i].outcome.unwrap())),..s },
            n::Request::Finish => p::State { active:active(z),..s },
        }
    }
}
/// Replay is composed one field at a time. These lemmas deliberately take no
/// Native state or placement-action guard: they describe only the raw writes.
pub proof fn phase_command_fields(s: p::State,g: nat,ph: p::Phase,cmd: p::Command)
    ensures
        log::writes_ok(s,seq![log::Write::Phase { generation:g,value:ph },log::Write::Command { generation:g,value:cmd }]),
        log::apply_writes(s,seq![log::Write::Phase { generation:g,value:ph },log::Write::Command { generation:g,value:cmd }])
            == (p::State { phases:s.phases.insert(g,ph),commands:s.commands.insert((g,cmd)),..s }),
{
    let first = log::Write::Phase { generation:g,value:ph };
    let second = log::Write::Command { generation:g,value:cmd };
    log::single_write(s,first);
    log::append_write(s,seq![first],second);
    assert(seq![first,second] =~= seq![first].push(second));
}
pub proof fn decision_fields(s: p::State,g: nat,ph: p::Phase,cmd: p::Command,id: int,result: p::Outcome)
    ensures
        log::writes_ok(s,seq![log::Write::Phase { generation:g,value:ph },log::Write::Command { generation:g,value:cmd },
            log::Write::Outcome { nonce:id,value:result }]),
        log::apply_writes(s,seq![log::Write::Phase { generation:g,value:ph },log::Write::Command { generation:g,value:cmd },
            log::Write::Outcome { nonce:id,value:result }])
            == (p::State { phases:s.phases.insert(g,ph),commands:s.commands.insert((g,cmd)),outcomes:s.outcomes.insert(id,result),..s }),
{
    let prefix = seq![log::Write::Phase { generation:g,value:ph },log::Write::Command { generation:g,value:cmd }];
    let last = log::Write::Outcome { nonce:id,value:result };
    phase_command_fields(s,g,ph,cmd);
    log::append_write(s,prefix,last);
    assert(seq![log::Write::Phase { generation:g,value:ph },log::Write::Command { generation:g,value:cmd },last] =~= prefix.push(last));
}
pub proof fn publication_fields(s: p::State,snapshot: Map<int,p::Grant>,g: nat,ph: p::Phase,id: int,result: p::Outcome)
    ensures
        log::writes_ok(s,seq![log::Write::DirectoryAppend { snapshot },log::Write::Phase { generation:g,value:ph },
            log::Write::Command { generation:g,value:p::Command::Commit },log::Write::Outcome { nonce:id,value:result }]),
        log::apply_writes(s,seq![log::Write::DirectoryAppend { snapshot },log::Write::Phase { generation:g,value:ph },
            log::Write::Command { generation:g,value:p::Command::Commit },log::Write::Outcome { nonce:id,value:result }])
            == (p::State { directory:s.directory.push(snapshot),phases:s.phases.insert(g,ph),
                commands:s.commands.insert((g,p::Command::Commit)),outcomes:s.outcomes.insert(id,result),..s }),
{
    let first = log::Write::DirectoryAppend { snapshot };
    let rest = seq![log::Write::Phase { generation:g,value:ph },log::Write::Command { generation:g,value:p::Command::Commit },
        log::Write::Outcome { nonce:id,value:result }];
    log::single_write(s,first);
    decision_fields(log::apply_write(s,first),g,ph,p::Command::Commit,id,result);
    log::concat_writes(s,seq![first],rest);
    assert(seq![first,log::Write::Phase { generation:g,value:ph },log::Write::Command { generation:g,value:p::Command::Commit },
        log::Write::Outcome { nonce:id,value:result }] =~= seq![first]+rest);
}

pub proof fn transition_fields(b: n::Native,z: n::Native,s: p::State,r: n::Request,snapshot: Map<int,p::Grant>,status: Status)
    ensures log::writes_ok(s,transition_writes(b,z,r,snapshot,status)),
        log::apply_writes(s,transition_writes(b,z,r,snapshot,status)) == transition_image(b,z,s,r,snapshot,status),
{
    if status == Status::Ok {
        let i = b.active.unwrap() as int;
        let rec = b.records[i];
        let g = rec.plan.generation as nat;
        let ph = phase(z.controls[i].phase);
        match r {
            n::Request::Freeze => phase_command_fields(s,g,ph,p::Command::Freeze),
            n::Request::Final => phase_command_fields(s,g,ph,p::Command::Final),
            n::Request::Retire => phase_command_fields(s,g,ph,p::Command::Retire),
            n::Request::Commit => publication_fields(s,snapshot,g,ph,nonce(rec.plan.nonce),outcome(z.controls[i].outcome.unwrap())),
            n::Request::Abort => decision_fields(s,g,ph,p::Command::Abort,nonce(rec.plan.nonce),outcome(z.controls[i].outcome.unwrap())),
            n::Request::Finish => log::single_write(s,log::Write::Active { generation:active(z) }),
        }
    }
}
pub proof fn transition_guard(b: n::Native,z: n::Native,s: p::State,c: p::Constants,r: n::Request,status: Status)
    requires n::layout(b), master(b,s), n::transition_effect(b,z,r,status),
    ensures p::enabled(c,s,action(r)) == (status == Status::Ok),
{
    if let Some(i) = b.active {
        let g = b.records[i as int].plan.generation as nat;
        let ctl = b.controls[i as int];
        assert(controls_at(b,s,i as int));
        assert(s.received.contains((g,certificate(Certificate::Drained))) == ctl.received.has(Certificate::Drained));
        assert(s.received.contains((g,certificate(Certificate::Ready))) == ctl.received.has(Certificate::Ready));
        assert(s.received.contains((g,certificate(Certificate::Retired))) == ctl.received.has(Certificate::Retired));
        assert(s.received.contains((g,certificate(Certificate::SourceDone))) == ctl.received.has(Certificate::SourceDone));
        assert(s.received.contains((g,certificate(Certificate::DestinationDone))) == ctl.received.has(Certificate::DestinationDone));
        match r { n::Request::Freeze => {}, n::Request::Final => {}, n::Request::Retire => {},
            n::Request::Commit => {}, n::Request::Abort => {}, n::Request::Finish => {} }
    }
}
/// Field-level replay for the six master requests. The directory equality in
/// this lemma is discharged by publication_observation below from the actual
/// snapshot-index write; it is not an environmental or handler premise.
pub proof fn transition_replay(b: n::Native,z: n::Native,s: p::State,c: p::Constants,r: n::Request,status: Status,snapshot: Map<int,p::Grant>)
    requires n::layout(b), master(b,s), n::transition_effect(b,z,r,status),
        r is Commit && status == Status::Ok ==> snapshot == Map::new(p::directory(s).dom(),|k: int|
            if s.plans[s.active.unwrap()].keys.contains(k) { p::Grant { owner:s.plans[s.active.unwrap()].dst,epoch:s.active.unwrap() } } else { p::directory(s)[k] }),
    ensures log::writes_ok(s,transition_writes(b,z,r,snapshot,status)),
        log::apply_writes(s,transition_writes(b,z,r,snapshot,status)) == p::dispatch(c,s,action(r)),
{
    transition_guard(b,z,s,c,r,status);
    transition_fields(b,z,s,r,snapshot,status);
    if let Some(i) = b.active { assert(controls_at(b,s,i as int)); }
    match r {
        n::Request::Freeze => { assert(log::apply_writes(s,transition_writes(b,z,r,snapshot,status)) == p::dispatch(c,s,action(r))); },
        n::Request::Final => { assert(log::apply_writes(s,transition_writes(b,z,r,snapshot,status)) == p::dispatch(c,s,action(r))); },
        n::Request::Retire => { assert(log::apply_writes(s,transition_writes(b,z,r,snapshot,status)) == p::dispatch(c,s,action(r))); },
        n::Request::Commit => { assert(log::apply_writes(s,transition_writes(b,z,r,snapshot,status)) == p::dispatch(c,s,action(r))); },
        n::Request::Abort => { assert(log::apply_writes(s,transition_writes(b,z,r,snapshot,status)) == p::dispatch(c,s,action(r))); },
        n::Request::Finish => { assert(log::apply_writes(s,transition_writes(b,z,r,snapshot,status)) == p::dispatch(c,s,action(r))); },
    }
}

pub proof fn publication_observation(c: p::Constants,b: n::Native,z: n::Native,o: Observations,s: p::State)
    requires n::layout(b), n::transition_effect(b,z,n::Request::Commit,Status::Ok),
        observes(c,b,o), s.active == active(b),
        b.active is Some, prepared(b,b.active.unwrap() as int),
        b.current[b.records[b.active.unwrap() as int].slot as int] == b.records[b.active.unwrap() as int].previous,
        range_observed(o,b.records[b.active.unwrap() as int].plan.range),
        p::directory(s) == directory(c,b,o),
        s.plans[s.active.unwrap()].keys == selected(c,o,b.records[b.active.unwrap() as int].plan.range),
        s.plans[s.active.unwrap()].dst == b.records[b.active.unwrap() as int].plan.destination,
        forall|k: int| c.keys.contains(k) ==>
            (c.table[k] == b.records[b.active.unwrap() as int].plan.range.table) == (o.slots[k] == b.records[b.active.unwrap() as int].slot),
    ensures directory(c,z,o) == Map::new(p::directory(s).dom(),|k: int|
        if s.plans[s.active.unwrap()].keys.contains(k) { p::Grant { owner:s.plans[s.active.unwrap()].dst,epoch:s.active.unwrap() } } else { p::directory(s)[k] }),
{
    let i = b.active.unwrap() as int; let rec = b.records[i]; let r = rec.plan.range;
    assert forall|k: int| c.keys.contains(k) implies directory(c,z,o)[k] ==
        if s.plans[s.active.unwrap()].keys.contains(k) { p::Grant { owner:s.plans[s.active.unwrap()].dst,epoch:s.active.unwrap() } }
        else { p::directory(s)[k] } by {
        range_observation(c,b,o,r,k);
        if o.slots[k] == rec.slot {
            proposal_observation(b,i,o.bytes[k]);
            assert(z.current[o.slots[k] as int] == rec.proposal);
        } else { assert(z.current[o.slots[k] as int] == b.current[o.slots[k] as int]); }
    }
    assert(directory(c,z,o) =~= Map::new(p::directory(s).dom(),|k: int|
        if s.plans[s.active.unwrap()].keys.contains(k) { p::Grant { owner:s.plans[s.active.unwrap()].dst,epoch:s.active.unwrap() } } else { p::directory(s)[k] }));
}

/// Authentic issuance is the one receipt premise: a participant already emitted
/// this generation/certificate. Neither native receipt nor phase is substituted
/// for this independent fact. Rejected unknown/wrong-origin messages stutter.
pub proof fn receive_replay(b: n::Native,z: n::Native,s: p::State,c: p::Constants,g: u64,owner: u32,cert: Certificate,status: Status)
    requires n::layout(b), master(b,s), n::receive_effect(b,z,g,owner,cert,status),
        status == Status::Ok ==> s.certificates.contains((g as nat,certificate(cert))),
    ensures ({
        let writes = if status == Status::Ok { seq![log::Write::Received { generation:g as nat,value:certificate(cert) }] } else { Seq::empty() };
        let a = if status == Status::Ok { p::Action::Receive { generation:g as nat,certificate:certificate(cert) } } else { p::Action::Stutter };
        log::writes_ok(s,writes) && log::apply_writes(s,writes) == p::dispatch(c,s,a)
        && (status != Status::Ok ==> z == b)
    }),
{
    if status == Status::Ok { log::single_write(s,log::Write::Received { generation:g as nat,value:certificate(cert) }); }
}
pub proof fn reply_replay(b: n::Native,z: n::Native,s: p::State,c: p::Constants,id: TxnId,result: Option<Outcome>)
    requires n::layout(b), master(b,s), n::reply_effect(b,z,id,result),
    ensures ({
        let writes = match result { Some(o) => seq![log::Write::Reply { nonce:nonce(id),value:outcome(o) }], None => Seq::empty() };
        let a = match result { Some(_) => p::Action::Reply { nonce:nonce(id) }, None => p::Action::Stutter };
        log::writes_ok(s,writes) && log::apply_writes(s,writes) == p::dispatch(c,s,a)
        && (result is None ==> z == b)
    }),
{
    if let Some(o) = result {
        let i = choose|i: int| 0 <= i < b.records.len() && n::same_nonce(b.records[i].plan.nonce,id);
        nonce_injective(b.records[i].plan.nonce,id);
        assert(controls_at(b,s,i));
        log::single_write(s,log::Write::Reply { nonce:nonce(id),value:outcome(o) });
    }
}

pub open spec fn begin_action(o: Observations,id: TxnId,src: u32,dst: u32,r: crate::types::KeyRange) -> p::Action {
    p::Action::Begin { nonce:nonce(id),src:src as int,dst:dst as int,table:r.table as int,lo:d::rank(o.support,r.lo@),hi:d::image_hi(o.support,n::upper(r)) }
}
pub open spec fn begin_plan(c: p::Constants,b: n::Native,z: n::Native,o: Observations) -> p::Plan {
    let plan = z.records.last().plan;
    p::Plan { nonce:nonce(plan.nonce),src:plan.source as int,dst:plan.destination as int,
        table:plan.range.table as int,lo:d::rank(o.support,plan.range.lo@),hi:d::image_hi(o.support,n::upper(plan.range)),
        keys:selected(c,o,plan.range),old:directory(c,b,o) }
}
pub open spec fn record_begin(g: nat,plan: p::Plan,phase: p::Phase,next: nat,active: Option<nat>) -> Seq<log::Write> {
    seq![log::Write::Plan { generation:g,value:plan },log::Write::Phase { generation:g,value:phase },
        log::Write::Command { generation:g,value:p::Command::Start },log::Write::NextGeneration { generation:next },
        log::Write::Active { generation:active }]
}
pub proof fn replay_begin_fields(s: p::State,g: nat,plan: p::Plan,phase: p::Phase,next: nat,active: Option<nat>)
    ensures log::writes_ok(s,record_begin(g,plan,phase,next,active)),
        log::apply_writes(s,record_begin(g,plan,phase,next,active)) == (p::State {
            plans:s.plans.insert(g,plan),phases:s.phases.insert(g,phase),commands:s.commands.insert((g,p::Command::Start)),
            next_generation:next,active,..s }),
{
    reveal_with_fuel(log::apply_writes,6);
    reveal_with_fuel(log::writes_ok,6);
}
pub open spec fn begin_writes(c: p::Constants,b: n::Native,z: n::Native,o: Observations) -> Seq<log::Write> {
    record_begin(z.records.last().plan.generation as nat,begin_plan(c,b,z,o),phase(z.controls.last().phase),z.next_generation as nat,active(z))
}
pub proof fn begin_replay(c: p::Constants,b: n::Native,z: n::Native,o: Observations,s: p::State,id: TxnId,src: u32,dst: u32,r: crate::types::KeyRange,g: u64)
    requires n::layout(b), n::begin_effect(b,z,id,src,dst,r,Ok(g)), master(b,s),
        observes(c,b,o), range_observed(o,r), p::directory(s) == directory(c,b,o),
        // Fixed pool identity; certificate transport cannot invent node IDs.
        forall|node: u32| b.nodes.contains(node) ==> c.shards.contains(node as int),
        // Include an empty-cell observation if this byte interval has no rows.
        selected(c,o,r) != Set::<int>::empty(),
        forall|k: int| c.keys.contains(k) && c.table[k] == r.table ==> o.slots[k] == z.records.last().slot,
    ensures p::enabled(c,s,begin_action(o,id,src,dst,r)),
        log::writes_ok(s,begin_writes(c,b,z,o)),
        log::apply_writes(s,begin_writes(c,b,z,o)) == p::dispatch(c,s,begin_action(o,id,src,dst,r)),
{
    nonce_injective(id,id);
    valid_range_observation(o,r);
    assert(!s.outcomes.contains_key(nonce(id))) by {
        if s.outcomes.contains_key(nonce(id)) {
            let j = choose|j: int| 0 <= j < b.records.len() && nonce(b.records[j].plan.nonce) == nonce(id);
            nonce_injective(b.records[j].plan.nonce,id);
        }
    }
    assert forall|k: int| selected(c,o,r).contains(k) implies p::directory(s)[k].owner == src by {
        range_observation(c,b,o,r,k);
    }
    assert(p::enabled(c,s,begin_action(o,id,src,dst,r)));
    replay_begin_fields(s,z.records.last().plan.generation as nat,begin_plan(c,b,z,o),phase(z.controls.last().phase),z.next_generation as nat,active(z));
    assert(z.records.last().plan.generation == s.next_generation);
    assert(phase(z.controls.last().phase) == p::Phase::Copy);
    assert(z.next_generation as nat == s.next_generation + 1);
    assert(active(z) == Some(s.next_generation));
    assert(begin_plan(c,b,z,o) == (p::Plan { nonce:nonce(id),src:src as int,dst:dst as int,table:r.table as int,
        lo:d::rank(o.support,r.lo@),hi:d::image_hi(o.support,n::upper(r)),keys:selected(c,o,r),old:p::directory(s) }));
}

pub proof fn distinct_records(v: n::Native,i: int,j: int)
    requires n::layout(v), 0 <= i < v.records.len(), 0 <= j < v.records.len(), i != j,
    ensures v.records[i].plan.generation != v.records[j].plan.generation,
        nonce(v.records[i].plan.nonce) != nonce(v.records[j].plan.nonce),
{
    nonce_injective(v.records[i].plan.nonce,v.records[j].plan.nonce);
    if i < j { assert(!n::same_nonce(v.records[i].plan.nonce,v.records[j].plan.nonce)); }
    else { assert(!n::same_nonce(v.records[j].plan.nonce,v.records[i].plan.nonce)); }
}

/// Mutation/projection coupling: not just a proof about the independent logger.
/// The premise is the postcondition checked on Coordinator::advance's body.
pub proof fn transition_projection(c: p::Constants,b: n::Native,z: n::Native,s: p::State,r: n::Request,status: Status,snapshot: Map<int,p::Grant>)
    requires n::layout(b), master(b,s), n::transition_effect(b,z,r,status),
    ensures master(z,log::apply_writes(s,transition_writes(b,z,r,snapshot,status))),
{
    let after = log::apply_writes(s,transition_writes(b,z,r,snapshot,status));
    transition_fields(b,z,s,r,snapshot,status);
    if status == Status::Ok {
        let i = b.active.unwrap() as int;
        assert(controls_at(b,s,i));
        assert(n::control_valid(b.controls[i],b.records[i].plan.generation));
        assert forall|j: int| 0 <= j < z.records.len() implies controls_at(z,after,j) by {
            assert(controls_at(b,s,j));
            if j != i { distinct_records(b,i,j); }
            assert forall|cmd: Command| after.commands.contains((z.records[j].plan.generation as nat,command(cmd)))
                == z.controls[j].issued.has(cmd) by {
                assert(s.commands.contains((b.records[j].plan.generation as nat,command(cmd))) == b.controls[j].issued.has(cmd));
                match cmd { Command::Start => {}, Command::Freeze => {}, Command::Final => {},
                    Command::Retire => {}, Command::Commit => {}, Command::Abort => {} }
            }
            assert forall|cert: Certificate| after.received.contains((z.records[j].plan.generation as nat,certificate(cert)))
                == z.controls[j].received.has(cert) by {}
        }
        assert forall|id: int| after.outcomes.contains_key(id) implies exists|j: int| 0 <= j < z.records.len() && nonce(z.records[j].plan.nonce) == id by {
            if id == nonce(b.records[i].plan.nonce) { assert(nonce(z.records[i].plan.nonce) == id); }
        }
        assert(after.phases.dom() == after.plans.dom());
        assert(after.next_generation == z.next_generation && after.active == active(z));
        assert(forall|g: nat,c: p::Command| after.commands.contains((g,c)) ==> after.plans.contains_key(g));
        assert(forall|g: nat,c: p::Certificate| after.received.contains((g,c)) ==> after.plans.contains_key(g));
        assert(master(z,after));
    }
}

pub proof fn receipt_insert(received: Set<(nat,p::Certificate)>,before: n::Receipts,g: nat,cert: Certificate)
    requires forall|other: Certificate| received.contains((g,certificate(other))) == before.has(other),
    ensures forall|other: Certificate| received.insert((g,certificate(cert))).contains((g,certificate(other))) == before.with(cert).has(other),
{
    assert forall|other: Certificate| received.insert((g,certificate(cert))).contains((g,certificate(other))) == before.with(cert).has(other) by {
        assert(received.contains((g,certificate(other))) == before.has(other));
        match other { Certificate::Drained => {}, Certificate::Ready => {}, Certificate::Retired => {},
            Certificate::SourceDone => {}, Certificate::DestinationDone => {} }
        match cert { Certificate::Drained => {}, Certificate::Ready => {}, Certificate::Retired => {},
            Certificate::SourceDone => {}, Certificate::DestinationDone => {} }
    }
}
pub proof fn receive_projection(b: n::Native,z: n::Native,s: p::State,g: u64,owner: u32,cert: Certificate,status: Status)
    requires n::layout(b), master(b,s), n::receive_effect(b,z,g,owner,cert,status),
    ensures master(z,if status == Status::Ok { log::apply_write(s,log::Write::Received { generation:g as nat,value:certificate(cert) }) } else { s }),
{
    if status == Status::Ok {
        let i = choose|i: int| 0 <= i < b.records.len() && b.records[i].plan.generation == g;
        let after = log::apply_write(s,log::Write::Received { generation:g as nat,value:certificate(cert) });
        assert(controls_at(b,s,i));
        receipt_insert(s.received,b.controls[i].received,g as nat,cert);
        assert forall|j: int| 0 <= j < z.records.len() implies controls_at(z,after,j) by {
            assert(controls_at(b,s,j));
            if i != j { distinct_records(b,i,j); }
            assert forall|other: Certificate| after.received.contains((z.records[j].plan.generation as nat,certificate(other)))
                == z.controls[j].received.has(other) by {
                assert(s.received.contains((b.records[j].plan.generation as nat,certificate(other))) == b.controls[j].received.has(other));
                if j == i {
                    assert(after.received.contains((g as nat,certificate(other))) == b.controls[i].received.with(cert).has(other));
                }
            }
        }
    }
}
pub proof fn reply_projection(b: n::Native,z: n::Native,s: p::State,id: TxnId,result: Option<Outcome>)
    requires n::layout(b), master(b,s), n::reply_effect(b,z,id,result),
    ensures master(z,match result { Some(o) => log::apply_write(s,log::Write::Reply { nonce:nonce(id),value:outcome(o) }), None => s }),
{
    if let Some(o) = result {
        let i = choose|i: int| 0 <= i < b.records.len() && n::same_nonce(b.records[i].plan.nonce,id);
        nonce_injective(b.records[i].plan.nonce,id);
        let after = log::apply_write(s,log::Write::Reply { nonce:nonce(id),value:outcome(o) });
        assert(controls_at(b,s,i));
        assert forall|j: int| 0 <= j < z.records.len() implies controls_at(z,after,j) by {
            assert(controls_at(b,s,j));
            if i != j { distinct_records(b,i,j); }
        }
        assert forall|other: int| after.replies.contains_key(other) implies exists|j: int| 0 <= j < z.records.len() && nonce(z.records[j].plan.nonce) == other by {
            if other == nonce(id) { assert(nonce(z.records[i].plan.nonce) == other); }
        }
    }
}
pub proof fn begin_projection(c: p::Constants,b: n::Native,z: n::Native,o: Observations,s: p::State,id: TxnId,src: u32,dst: u32,r: crate::types::KeyRange,g: u64)
    requires n::layout(b), master(b,s), n::begin_effect(b,z,id,src,dst,r,Ok(g)),
    ensures master(z,log::apply_writes(s,begin_writes(c,b,z,o))),
{
    let i = b.records.len() as int;
    let after = log::apply_writes(s,begin_writes(c,b,z,o));
    reveal_with_fuel(log::apply_writes,6);
    assert(!s.plans.contains_key(g as nat)) by {
        assert forall|j: int| 0 <= j < b.records.len() implies b.records[j].plan.generation != g by {
            assert(b.records[j].plan.generation < b.next_generation);
        }
    }
    assert(!s.outcomes.contains_key(nonce(id)) && !s.replies.contains_key(nonce(id))) by {
        assert forall|j: int| 0 <= j < b.records.len() implies nonce(b.records[j].plan.nonce) != nonce(id) by {
            nonce_injective(b.records[j].plan.nonce,id);
        }
    }
    assert forall|j: int| 0 <= j < z.records.len() implies controls_at(z,after,j) by {
        if j < i { assert(controls_at(b,s,j)); assert(b.records[j].plan.generation < g); }
        assert forall|cmd: Command| after.commands.contains((z.records[j].plan.generation as nat,command(cmd)))
            == z.controls[j].issued.has(cmd) by { match cmd { _ => {} } }
        assert forall|cert: Certificate| after.received.contains((z.records[j].plan.generation as nat,certificate(cert)))
            == z.controls[j].received.has(cert) by {}
    }
    assert forall|generation: nat| after.plans.contains_key(generation) == (exists|j: int| 0 <= j < z.records.len() && z.records[j].plan.generation == generation) by {
        if generation == g { assert(z.records[i].plan.generation == generation); }
        if exists|j: int| 0 <= j < z.records.len() && z.records[j].plan.generation == generation {
            let j = choose|j: int| 0 <= j < z.records.len() && z.records[j].plan.generation == generation;
            if j < i { assert(b.records[j].plan.generation == generation); }
        }
        if s.plans.contains_key(generation) {
            let j = choose|j: int| 0 <= j < b.records.len() && b.records[j].plan.generation == generation;
            assert(z.records[j].plan.generation == generation);
        }
    }
    assert forall|other: int| after.outcomes.contains_key(other) implies exists|j: int| 0 <= j < z.records.len() && nonce(z.records[j].plan.nonce) == other by {
        let j = choose|j: int| 0 <= j < b.records.len() && nonce(b.records[j].plan.nonce) == other;
        assert(nonce(z.records[j].plan.nonce) == other);
    }
    assert forall|other: int| after.replies.contains_key(other) implies exists|j: int| 0 <= j < z.records.len() && nonce(z.records[j].plan.nonce) == other by {
        let j = choose|j: int| 0 <= j < b.records.len() && nonce(b.records[j].plan.nonce) == other;
        assert(nonce(z.records[j].plan.nonce) == other);
    }
}

pub proof fn initial_master(v: n::Native,c: p::Constants)
    requires v.records.len() == 0, v.controls.len() == 0, v.active is None, v.next_generation == 1,
    ensures master(v,p::initial(c)),
{}
pub proof fn prepared_retained(b: n::Native,z: n::Native,i: int)
    requires prepared(b,i), z.records == b.records, z.snapshots == b.snapshots,
    ensures prepared(z,i),
{}
pub proof fn prepared_after_begin(b: n::Native,z: n::Native,i: int,id: TxnId,src: u32,dst: u32,r: crate::types::KeyRange,g: u64)
    requires n::layout(b), 0 <= i < b.records.len(), prepared(b,i), n::begin_effect(b,z,id,src,dst,r,Ok(g)),
    ensures prepared(z,i),
{}
pub proof fn rejected_admission(b: n::Native,z: n::Native,c: p::Constants,s: p::State,id: TxnId,src: u32,dst: u32,r: crate::types::KeyRange,error: Status)
    requires n::begin_effect(b,z,id,src,dst,r,Err(error)), master(b,s),
    ensures z == b, master(z,s), log::certificate(c,s,log::Segment { writes:Seq::empty(),states:seq![s,s],actions:seq![p::Action::Stutter] }),
{}

/// A caller closes only after its concrete-field theorem and raw replay theorem
/// have supplied this endpoint. No model action is used to implement the write.
pub proof fn segment(c: p::Constants,s: p::State,writes: Seq<log::Write>,a: p::Action) -> (out: log::Segment)
    requires log::writes_ok(s,writes), log::apply_writes(s,writes) == p::dispatch(c,s,a),
    ensures log::certificate(c,s,out), out.writes == writes,
{
    let z = log::apply_writes(s,writes);
    log::Segment { writes,states:seq![s,z],actions:seq![a] }
}
pub proof fn receive_segment(c: p::Constants,b: n::Native,z: n::Native,s: p::State,g: u64,owner: u32,cert: Certificate,status: Status)
    -> (out: log::Segment)
    requires n::layout(b), master(b,s), n::receive_effect(b,z,g,owner,cert,status),
        status == Status::Ok ==> s.certificates.contains((g as nat,certificate(cert))),
    ensures log::certificate(c,s,out), master(z,log::apply_writes(s,out.writes)),
        out.writes == if status == Status::Ok { seq![log::Write::Received { generation:g as nat,value:certificate(cert) }] } else { Seq::empty() },
{
    receive_replay(b,z,s,c,g,owner,cert,status);
    receive_projection(b,z,s,g,owner,cert,status);
    let writes = if status == Status::Ok { seq![log::Write::Received { generation:g as nat,value:certificate(cert) }] } else { Seq::empty() };
    let a = if status == Status::Ok { p::Action::Receive { generation:g as nat,certificate:certificate(cert) } } else { p::Action::Stutter };
    if status == Status::Ok { log::single_write(s,log::Write::Received { generation:g as nat,value:certificate(cert) }); }
    segment(c,s,writes,a)
}
pub proof fn reply_segment(c: p::Constants,b: n::Native,z: n::Native,s: p::State,id: TxnId,result: Option<Outcome>)
    -> (out: log::Segment)
    requires n::layout(b), master(b,s), n::reply_effect(b,z,id,result),
    ensures log::certificate(c,s,out), master(z,log::apply_writes(s,out.writes)),
        out.writes == match result { Some(o) => seq![log::Write::Reply { nonce:nonce(id),value:outcome(o) }], None => Seq::empty() },
{
    reply_replay(b,z,s,c,id,result);
    reply_projection(b,z,s,id,result);
    let writes = match result { Some(o) => seq![log::Write::Reply { nonce:nonce(id),value:outcome(o) }], None => Seq::empty() };
    let a = match result { Some(_) => p::Action::Reply { nonce:nonce(id) }, None => p::Action::Stutter };
    if let Some(o) = result { log::single_write(s,log::Write::Reply { nonce:nonce(id),value:outcome(o) }); }
    segment(c,s,writes,a)
}

} // verus!
