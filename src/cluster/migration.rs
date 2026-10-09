//! Single-writer migration coordinator. The caller holds its coordinator mutex
//! across each method and captures issued controls before releasing that mutex.
//! Certificates are authenticated transport receipts, not local participant facts.
//! The fixed node pool has no crash/restart or membership-reconfiguration protocol.
//!
//! Admission allocates the immutable replacement and retained record. Publication
//! switches a snapshot index/version; commit, abort and finish allocate nothing. All
//! records survive finish, fencing nonce replay and retaining decisions/commands.
use vstd::prelude::*;
use crate::bytes::{compare, copy_bytes};
use crate::directory::RouteTable;
#[cfg(verus_keep_ghost)]
use crate::bytes::{cmp_spec, cmp_trans};
#[cfg(verus_keep_ghost)]
use crate::directory::{wellformed, canonical, route, proper, source_owned, inside};
#[cfg(verus_keep_ghost)]
use crate::directory_proofs::equivalent;
use crate::types::{Boundary, Grant, TxnId, KeyRange, MigrationPlan, Phase, Command, Certificate, Outcome, Status};

verus! {

#[derive(Clone, Copy)]
pub struct Commands {
    pub start: bool, pub freeze: bool, pub final_copy: bool,
    pub retire: bool, pub commit: bool, pub abort: bool,
}
impl Commands {
    pub open spec fn has(self, c: Command) -> bool {
        match c { Command::Start => self.start, Command::Freeze => self.freeze,
            Command::Final => self.final_copy, Command::Retire => self.retire,
            Command::Commit => self.commit, Command::Abort => self.abort }
    }
    fn contains(&self, c: Command) -> (yes: bool)
        ensures yes == self.has(c),
    {
        match c { Command::Start => self.start, Command::Freeze => self.freeze,
            Command::Final => self.final_copy, Command::Retire => self.retire,
            Command::Commit => self.commit, Command::Abort => self.abort }
    }
}
#[derive(Clone, Copy)]
pub struct Receipts {
    pub drained: bool, pub ready: bool, pub retired: bool,
    pub source_done: bool, pub destination_done: bool,
}
impl Receipts {
    pub open spec fn has(self, c: Certificate) -> bool {
        match c { Certificate::Drained => self.drained, Certificate::Ready => self.ready,
            Certificate::Retired => self.retired, Certificate::SourceDone => self.source_done,
            Certificate::DestinationDone => self.destination_done }
    }
    pub open spec fn with(self, c: Certificate) -> Self {
        match c { Certificate::Drained => Self { drained: true, ..self },
            Certificate::Ready => Self { ready: true, ..self },
            Certificate::Retired => Self { retired: true, ..self },
            Certificate::SourceDone => Self { source_done: true, ..self },
            Certificate::DestinationDone => Self { destination_done: true, ..self } }
    }
}
#[derive(Clone, Copy)]
pub struct Control {
    pub phase: Phase,
    pub issued: Commands,
    pub received: Receipts,
    pub outcome: Option<Outcome>,
    pub replied: bool,
}
/// Retained immutable metadata. The original routing grants include epochs.
pub struct Record {
    pub plan: MigrationPlan,
    pub previous: usize,
    pub proposal: usize,
    pub slot: usize,
}
pub struct Native {
    pub nodes: Seq<u32>,
    pub snapshots: Seq<RouteTable>,
    pub current: Seq<usize>,
    pub records: Seq<Record>,
    pub controls: Seq<Control>,
    pub active: Option<usize>,
    pub next_generation: u64,
    pub published_version: u64,
}
pub open spec fn upper(r: KeyRange) -> Option<Seq<u8>> {
    match r.hi { Some(h) => Some(h@), None => None }
}
pub open spec fn same_nonce(a: TxnId, b: TxnId) -> bool {
    a.client == b.client && a.sequence == b.sequence
}
pub open spec fn terminal(p: Phase) -> bool { p is Committed || p is Aborted }
pub open spec fn initial_control() -> Control {
    Control { phase: Phase::Copy,
        issued: Commands { start: true, freeze: false, final_copy: false, retire: false, commit: false, abort: false },
        received: Receipts { drained: false, ready: false, retired: false, source_done: false, destination_done: false },
        outcome: None, replied: false }
}
pub open spec fn control_valid(c: Control,g: u64) -> bool {
    c.outcome == if terminal(c.phase) { Some(Outcome { generation:g,committed:c.phase is Committed }) } else { None }
    && (c.replied ==> c.outcome is Some)
}
pub open spec fn layout(v: Native) -> bool {
    v.next_generation >= 1
    && v.published_version < v.next_generation
    && v.records.len() == v.controls.len()
    && (forall|i: int| 0 <= i < v.snapshots.len() ==> wellformed(v.snapshots[i].boundaries@) && canonical(v.snapshots[i].boundaries@))
    && (forall|i: int| 0 <= i < v.current.len() ==> #[trigger] v.current[i] < v.snapshots.len())
    && (forall|i: int| 0 <= i < v.records.len() ==> {
        let r = #[trigger] v.records[i];
        r.plan.generation < v.next_generation && r.plan.generation > 0
        && r.proposal < v.snapshots.len() && r.previous < v.snapshots.len() && r.slot < v.current.len()
        && control_valid(v.controls[i],r.plan.generation)
    })
    && (forall|i: int, j: int| 0 <= i < j < v.records.len() ==>
        (#[trigger] v.records[i]).plan.generation < (#[trigger] v.records[j]).plan.generation
        && !same_nonce(v.records[i].plan.nonce, v.records[j].plan.nonce))
    && match v.active { Some(i) => i < v.records.len(), None => true }
}
pub open spec fn receipt_owner(p: MigrationPlan, c: Certificate) -> u32 {
    match c { Certificate::Ready | Certificate::DestinationDone => p.destination, _ => p.source }
}

pub struct Coordinator {
    nodes: Vec<u32>,
    snapshots: Vec<RouteTable>,
    current: Vec<usize>,
    records: Vec<Record>,
    controls: Vec<Control>,
    active: Option<usize>,
    next_generation: u64,
    published_version: u64,
}
impl View for Coordinator {
    type V = Native;
    closed spec fn view(&self) -> Native {
        Native { nodes: self.nodes@, snapshots: self.snapshots@, current: self.current@,
            records: self.records@, controls: self.controls@, active: self.active, next_generation: self.next_generation,
            published_version: self.published_version }
    }
}
impl Coordinator {
    pub open spec fn wf(&self) -> bool { layout(self@) }

    pub fn new(nodes: Vec<u32>, tables: Vec<RouteTable>) -> (out: Result<Self,Status>)
        ensures match out { Ok(c) => c.wf() && c@.nodes == nodes@ && c@.snapshots == tables@
            && c@.records.len() == 0 && c@.controls.len() == 0 && c@.active is None
            && c@.next_generation == 1 && c@.current.len() == tables.len()
            && c@.published_version == 0
            && (forall|i: int| 0 <= i < tables.len() ==> c@.current[i] == i)
            && bootstrap(nodes@,tables@), Err(_) => true },
    {
        if nodes.len() < 2 { return Err(Status::Invalid); }
        let mut i = 0usize;
        while i < nodes.len()
            invariant i <= nodes.len(),
                forall|a: int,b: int| 0 <= a < b < i ==> nodes@[a] != nodes@[b],
            decreases nodes.len() - i,
        {
            let mut j = 0usize;
            while j < i
                invariant j <= i < nodes.len(),
                    forall|a: int,b: int| 0 <= a < b < i ==> nodes@[a] != nodes@[b],
                    forall|a: int| 0 <= a < j ==> nodes@[a] != nodes@[i as int],
                decreases i-j,
            {
                if nodes[j] == nodes[i] { return Err(Status::Invalid); }
                j += 1;
            }
            i += 1;
        }
        let mut current = Vec::new();
        let mut t = 0usize;
        while t < tables.len()
            invariant t <= tables.len(), current.len() == t,
                nodes.len() >= 2,
                forall|i: int,j: int| 0 <= i < j < nodes.len() ==> nodes@[i] != nodes@[j],
                forall|i: int| 0 <= i < t ==> current@[i] == i,
                forall|i: int| 0 <= i < t ==> valid_initial_table(#[trigger] tables@[i],nodes@),
                forall|i: int,j: int| 0 <= i < j < t ==> tables[i].table != tables[j].table,
            decreases tables.len()-t,
        {
            if !validate_table(&tables[t],&nodes) { return Err(Status::Invalid); }
            let mut j = 0usize;
            while j < t
                invariant j <= t < tables.len(),
                    forall|a: int,b: int| 0 <= a < b < t ==> tables[a].table != tables[b].table,
                    forall|a: int| 0 <= a < j ==> tables[a].table != tables[t as int].table,
                decreases t-j,
            {
                if tables[j].table == tables[t].table { return Err(Status::Invalid); }
                j += 1;
            }
            current.push(t);
            t += 1;
        }
        let out = Self { nodes, snapshots: tables, current, records: Vec::new(), controls: Vec::new(),
            active: None, next_generation: 1, published_version: 0 };
        proof {
            assert forall|i: int| 0 <= i < out.snapshots.len() implies wellformed(out.snapshots@[i].boundaries@) && canonical(out.snapshots@[i].boundaries@) by {
                assert(valid_initial_table(out.snapshots@[i],out.nodes@));
            }
            assert(out.wf());
            assert(bootstrap(out.nodes@,out.snapshots@));
        }
        Ok(out)
    }

    fn find_nonce(&self, nonce: TxnId) -> (out: Option<usize>)
        ensures match out { Some(i) => i < self@.records.len() && same_nonce(self@.records[i as int].plan.nonce,nonce),
            None => forall|i: int| 0 <= i < self@.records.len() ==> !same_nonce(self@.records[i].plan.nonce,nonce) },
    {
        let mut i = 0usize;
        while i < self.records.len()
            invariant i <= self.records.len(),
                forall|j: int| 0 <= j < i ==> !same_nonce(self.records@[j].plan.nonce,nonce),
            decreases self.records.len()-i,
        {
            let n = self.records[i].plan.nonce;
            if n.client == nonce.client && n.sequence == nonce.sequence { return Some(i); }
            i += 1;
        }
        None
    }
    fn find_generation(&self, generation: u64) -> (out: Option<usize>)
        requires self.wf(),
        ensures match out { Some(i) => i < self@.records.len() && self@.records[i as int].plan.generation == generation,
            None => forall|i: int| 0 <= i < self@.records.len() ==> self@.records[i].plan.generation != generation },
    {
        let mut lo = 0usize;
        let mut hi = self.records.len();
        while lo < hi
            invariant lo <= hi <= self.records.len(), self.wf(),
                forall|j: int| 0 <= j < lo ==> self.records@[j].plan.generation < generation,
                forall|j: int| hi <= j < self.records.len() ==> self.records@[j].plan.generation >= generation,
            decreases hi-lo,
        {
            let mid = lo + (hi-lo)/2;
            if self.records[mid].plan.generation < generation {
                proof {
                    assert forall|j: int| 0 <= j <= mid implies self.records@[j].plan.generation < generation by {
                        if j < mid { assert(self.records@[j].plan.generation < self.records@[mid as int].plan.generation); }
                    }
                }
                lo = mid + 1;
            } else {
                proof {
                    assert forall|j: int| mid <= j < self.records.len() implies self.records@[j].plan.generation >= generation by {
                        if mid < j { assert(self.records@[mid as int].plan.generation < self.records@[j].plan.generation); }
                    }
                }
                hi = mid;
            }
        }
        if lo < self.records.len() && self.records[lo].plan.generation == generation { Some(lo) } else {
            proof {
                assert forall|j: int| 0 <= j < self.records.len() implies self.records@[j].plan.generation != generation by {
                    if j > lo { assert(self.records@[lo as int].plan.generation < self.records@[j].plan.generation); }
                }
            }
            None
        }
    }
    fn find_table(&self, table: u64) -> (out: Option<usize>)
        requires self.wf(),
        ensures match out { Some(i) => i < self@.current.len() && self@.snapshots[self@.current[i as int] as int].table == table,
            None => forall|i: int| 0 <= i < self@.current.len() ==> self@.snapshots[self@.current[i] as int].table != table },
    {
        proof {
            reveal(<Coordinator as View>::view);
            assert forall|j: int| 0 <= j < self.current.len() implies self.current@[j] < self.snapshots.len() by {
                assert(self@.current[j] < self@.snapshots.len());
            }
        }
        let mut i = 0usize;
        while i < self.current.len()
            invariant self.wf(), i <= self.current.len(),
                forall|j: int| 0 <= j < self.current.len() ==> #[trigger] self.current@[j] < self.snapshots.len(),
                forall|j: int| 0 <= j < i ==> self.snapshots@[self.current@[j] as int].table != table,
            decreases self.current.len()-i,
        {
            proof { assert(self.current@[i as int] < self.snapshots.len()); }
            if self.snapshots[self.current[i]].table == table { return Some(i); }
            i += 1;
        }
        None
    }

    pub fn begin(&mut self, nonce: TxnId, source: u32, destination: u32, range: KeyRange) -> (out: Result<u64,Status>)
        requires old(self).wf(),
        ensures final(self).wf(), begin_effect(old(self)@,final(self)@,nonce,source,destination,range,out),
    {
        if self.find_nonce(nonce).is_some() { return Err(Status::Invalid); }
        if self.active.is_some() { return Err(Status::Busy); }
        if self.next_generation == u64::MAX { return Err(Status::Exhausted); }
        if source == destination || !node_exists(&self.nodes,source) || !node_exists(&self.nodes,destination) {
            return Err(Status::Invalid);
        }
        let Some(slot) = self.find_table(range.table) else { return Err(Status::NotFound); };
        let previous = self.current[slot];
        let upper = match &range.hi { Some(h) => Some(h.as_slice()), None => None };
        proof { assert(crate::migration::upper(range) == crate::directory::hi_view(upper)); }
        let proposal = match self.snapshots[previous].move_range(&range.lo,upper,source,destination,self.next_generation) {
            Ok(p) => p, Err(s) => return Err(s),
        };
        let old = copy_boundaries(&self.snapshots[previous].boundaries);
        let generation = self.next_generation;
        let index = self.records.len();
        let proposal_index = self.snapshots.len();
        let ghost before = self@;
        self.snapshots.push(proposal);
        self.records.push(Record { plan: MigrationPlan { generation, nonce, source, destination, range, old },
            previous, proposal: proposal_index, slot });
        self.controls.push(Control { phase: Phase::Copy,
            issued: Commands { start: true, freeze: false, final_copy: false, retire: false, commit: false, abort: false },
            received: Receipts { drained: false, ready: false, retired: false, source_done: false, destination_done: false },
            outcome: None, replied: false });
        self.next_generation = generation + 1;
        self.active = Some(index);
        proof {
            assert forall|i: int,j: int| 0 <= i < j < self.records.len() implies
                self.records@[i].plan.generation < self.records@[j].plan.generation
                && !same_nonce(self.records@[i].plan.nonce,self.records@[j].plan.nonce) by {
                if j == before.records.len() { assert(i < before.records.len()); }
            }
            assert(self.snapshots@.drop_last() == before.snapshots);
            assert(self.snapshots@.last() == self.snapshots@[proposal_index as int]);
            assert(self.records@[index as int].plan.range == range);
            assert(self@ == (Native { snapshots: self.snapshots@, records: before.records.push(self.records@[index as int]),
                controls: before.controls.push(initial_control()),active:Some(index),next_generation:(generation+1) as u64, ..before }));
            assert(begin_effect(before,self@,nonce,source,destination,range,Ok(generation)));
        }
        Ok(generation)
    }

    /// Rejected or repeated requests are exact native-state stutters.
    pub fn request_freeze(&mut self) -> (status: Status)
        requires old(self).wf(),
        ensures final(self).wf(), transition_effect(old(self)@,final(self)@,Request::Freeze,status),
    { self.advance(Request::Freeze) }
    pub fn request_final(&mut self) -> (status: Status)
        requires old(self).wf(),
        ensures final(self).wf(), transition_effect(old(self)@,final(self)@,Request::Final,status),
    { self.advance(Request::Final) }
    pub fn request_retire(&mut self) -> (status: Status)
        requires old(self).wf(),
        ensures final(self).wf(), transition_effect(old(self)@,final(self)@,Request::Retire,status),
    { self.advance(Request::Retire) }
    pub fn commit(&mut self) -> (status: Status)
        requires old(self).wf(),
        ensures final(self).wf(), transition_effect(old(self)@,final(self)@,Request::Commit,status),
    { self.advance(Request::Commit) }
    pub fn abort(&mut self) -> (status: Status)
        requires old(self).wf(),
        ensures final(self).wf(), transition_effect(old(self)@,final(self)@,Request::Abort,status),
    { self.advance(Request::Abort) }
    pub fn finish(&mut self) -> (status: Status)
        requires old(self).wf(),
        ensures final(self).wf(), transition_effect(old(self)@,final(self)@,Request::Finish,status),
    { self.advance(Request::Finish) }

    fn advance(&mut self, request: Request) -> (status: Status)
        requires old(self).wf(),
        ensures final(self).wf(), transition_effect(old(self)@,final(self)@,request,status),
    {
        let Some(i) = self.active else { return Status::NotFound; };
        let mut c = self.controls[i];
        let p = &self.records[i];
        match request {
            Request::Freeze => {
                if !matches!(c.phase,Phase::Copy) { return Status::Retry; }
                c.phase = Phase::Freezing; c.issued.freeze = true;
            },
            Request::Final => {
                if !matches!(c.phase,Phase::Freezing) || !c.received.drained { return Status::Retry; }
                c.phase = Phase::Final; c.issued.final_copy = true;
            },
            Request::Retire => {
                if !matches!(c.phase,Phase::Final) || !c.received.ready { return Status::Retry; }
                c.phase = Phase::Retiring; c.issued.retire = true;
            },
            Request::Commit => {
                if !matches!(c.phase,Phase::Retiring) || !c.received.retired { return Status::Retry; }
                // The replacement was allocated and validated at admission.
                self.current.set(p.slot,p.proposal);
                self.published_version = p.plan.generation;
                c.phase = Phase::Committed; c.issued.commit = true;
                c.outcome = Some(Outcome { generation: p.plan.generation, committed: true });
            },
            Request::Abort => {
                if matches!(c.phase,Phase::Committed) || matches!(c.phase,Phase::Aborted) { return Status::Retry; }
                c.phase = Phase::Aborted; c.issued.abort = true;
                c.outcome = Some(Outcome { generation: p.plan.generation, committed: false });
            },
            Request::Finish => {
                if !(matches!(c.phase,Phase::Committed) || matches!(c.phase,Phase::Aborted))
                    || !c.received.source_done || !c.received.destination_done { return Status::Retry; }
                self.active = None;
            },
        }
        self.controls.set(i,c);
        Status::Ok
    }

    /// The RPC boundary must authenticate issuance for this generation and role.
    /// Receipt is intentionally independent of phase, allowing authentic reorders.
    pub fn receive(&mut self, generation: u64, owner: u32, certificate: Certificate) -> (status: Status)
        requires old(self).wf(),
        ensures final(self).wf(), receive_effect(old(self)@,final(self)@,generation,owner,certificate,status),
    {
        let Some(i) = self.find_generation(generation) else { return Status::NotFound; };
        proof { unique_generation(self@,generation,i as int); }
        let p = &self.records[i].plan;
        let expected = match certificate { Certificate::Ready | Certificate::DestinationDone => p.destination, _ => p.source };
        if owner != expected { return Status::Invalid; }
        let mut c = self.controls[i];
        match certificate {
            Certificate::Drained => c.received.drained = true,
            Certificate::Ready => c.received.ready = true,
            Certificate::Retired => c.received.retired = true,
            Certificate::SourceDone => c.received.source_done = true,
            Certificate::DestinationDone => c.received.destination_done = true,
        }
        self.controls.set(i,c);
        Status::Ok
    }

    pub fn outcome(&self, nonce: TxnId) -> (out: Option<Outcome>)
        requires self.wf(),
        ensures out == outcome_at(self@,nonce),
    {
        let Some(i) = self.find_nonce(nonce) else { return None; };
        proof { unique_nonce(self@,nonce,i as int); }
        self.controls[i].outcome
    }
    /// Records the actual administrative reply separately from mere observation.
    pub fn reply(&mut self, nonce: TxnId) -> (out: Option<Outcome>)
        requires old(self).wf(),
        ensures final(self).wf(), reply_effect(old(self)@,final(self)@,nonce,out),
    {
        let Some(i) = self.find_nonce(nonce) else { return None; };
        proof { unique_nonce(self@,nonce,i as int); }
        let mut c = self.controls[i];
        let Some(outcome) = c.outcome else { return None; };
        c.replied = true;
        self.controls.set(i,c);
        Some(outcome)
    }
    /// These accessors share the caller's immutable coordinator borrow. A codec
    /// reads the version and every table while holding that same borrow/lock.
    pub fn table_count(&self) -> (out: usize)
        ensures out == self@.current.len(),
    { self.current.len() }
    pub fn table_at(&self, index: usize) -> (out: Option<&RouteTable>)
        requires self.wf(),
        ensures match out {
            Some(t) => index < self@.current.len() && *t == self@.snapshots[self@.current[index as int] as int]
                && wellformed(t.boundaries@) && canonical(t.boundaries@),
            None => index >= self@.current.len(),
        },
    {
        if index >= self.current.len() { return None; }
        proof {
            reveal(<Coordinator as View>::view);
            assert(self@.current[index as int] < self@.snapshots.len());
        }
        Some(&self.snapshots[self.current[index]])
    }
    pub fn published_version(&self) -> (out: u64)
        ensures out == self@.published_version,
    { self.published_version }
    pub fn active_plan(&self) -> (out: Option<&MigrationPlan>)
        requires self.wf(),
        ensures match (out,self@.active) { (Some(p),Some(i)) => *p == self@.records[i as int].plan,
            (None,None) => true, _ => false },
    { match self.active { Some(i) => Some(&self.records[i].plan), None => None } }
    pub fn current_phase(&self) -> (out: Option<Phase>)
        requires self.wf(),
        ensures out == match self@.active { Some(i) => Some(self@.controls[i as int].phase), None => None },
    { match self.active { Some(i) => Some(self.controls[i].phase), None => None } }
    pub fn command_issued(&self, generation: u64, command: Command) -> (out: bool)
        requires self.wf(),
        ensures out == (exists|i: int| 0 <= i < self@.records.len() && self@.records[i].plan.generation == generation
            && self@.controls[i].issued.has(command)),
    {
        match self.find_generation(generation) {
            Some(i) => { proof { unique_generation(self@,generation,i as int); } self.controls[i].issued.contains(command) },
            None => false,
        }
    }
    /// Retained plans support retransmission after finish without recreating work.
    pub fn plan(&self, generation: u64) -> (out: Option<&MigrationPlan>)
        requires self.wf(),
        ensures match out { Some(p) => exists|i: int| 0 <= i < self@.records.len() && *p == self@.records[i].plan && p.generation == generation,
            None => forall|i: int| 0 <= i < self@.records.len() ==> self@.records[i].plan.generation != generation },
    { match self.find_generation(generation) { Some(i) => Some(&self.records[i].plan), None => None } }
    pub fn lookup(&self, table: u64, coordinate: &[u8]) -> (out: Option<Grant>)
        requires self.wf(),
        ensures match out { Some(g) => exists|i: int| 0 <= i < self@.current.len()
            && self@.snapshots[self@.current[i] as int].table == table
            && g == route(self@.snapshots[self@.current[i] as int].boundaries@,coordinate@),
            None => forall|i: int| 0 <= i < self@.current.len() ==> self@.snapshots[self@.current[i] as int].table != table },
    {
        let Some(i) = self.find_table(table) else { return None; };
        Some(self.snapshots[self.current[i]].lookup(coordinate))
    }
}

pub open spec fn valid_initial_table(t: RouteTable, nodes: Seq<u32>) -> bool {
    wellformed(t.boundaries@) && canonical(t.boundaries@)
        && forall|i: int| 0 <= i < t.boundaries.len() ==> t.boundaries@[i].grant.epoch == 0 && nodes.contains(t.boundaries@[i].grant.owner)
}
pub open spec fn bootstrap(nodes: Seq<u32>, tables: Seq<RouteTable>) -> bool {
    nodes.len() >= 2 && (forall|i: int,j: int| 0 <= i < j < nodes.len() ==> nodes[i] != nodes[j])
        && (forall|i: int| 0 <= i < tables.len() ==> valid_initial_table(tables[i],nodes))
        && (forall|i: int,j: int| 0 <= i < j < tables.len() ==> tables[i].table != tables[j].table)
}
fn node_exists(nodes: &Vec<u32>, node: u32) -> (yes: bool)
    ensures yes == nodes@.contains(node),
{
    let mut i = 0usize;
    while i < nodes.len()
        invariant i <= nodes.len(), forall|j: int| 0 <= j < i ==> nodes@[j] != node,
        decreases nodes.len()-i,
    {
        if nodes[i] == node { return true; }
        i += 1;
    }
    false
}
fn validate_table(t: &RouteTable, nodes: &Vec<u32>) -> (yes: bool)
    ensures yes ==> valid_initial_table(*t,nodes@),
{
    if t.boundaries.len() == 0 || t.boundaries[0].start.len() != 0 { return false; }
    let mut i = 0usize;
    while i < t.boundaries.len()
        invariant i <= t.boundaries.len(), t.boundaries.len() > 0, t.boundaries@[0].start@ == Seq::<u8>::empty(),
            forall|a: int,b: int| 0 <= a < b < i ==> cmp_spec(t.boundaries@[a].start@,t.boundaries@[b].start@) < 0,
            forall|a: int| 0 < a < i ==> #[trigger] t.boundaries@[a].grant != t.boundaries@[a-1].grant,
            forall|a: int| 0 <= a < i ==> t.boundaries@[a].grant.epoch == 0 && nodes@.contains(t.boundaries@[a].grant.owner),
        decreases t.boundaries.len()-i,
    {
        let b = &t.boundaries[i];
        if b.grant.epoch != 0 || !node_exists(nodes,b.grant.owner) { return false; }
        if i > 0 {
            let previous = &t.boundaries[i-1];
            if compare(&previous.start,&b.start) >= 0 || previous.grant.owner == b.grant.owner { return false; }
            proof {
                assert forall|a: int| 0 <= a < i implies cmp_spec(t.boundaries@[a].start@,b.start@) < 0 by {
                    if a < i-1 { cmp_trans(t.boundaries@[a].start@,previous.start@,b.start@); }
                }
            }
        }
        i += 1;
    }
    true
}
fn copy_boundaries(p: &Vec<Boundary>) -> (out: Vec<Boundary>)
    ensures equivalent(out@,p@),
{
    let mut out = Vec::new();
    let mut i = 0usize;
    while i < p.len()
        invariant i <= p.len(), out.len() == i, equivalent(out@,p@.take(i as int)),
        decreases p.len()-i,
    {
        out.push(Boundary { start: copy_bytes(&p[i].start), grant: p[i].grant });
        i += 1;
    }
    out
}

pub proof fn unique_nonce(v: Native,n: TxnId,i: int)
    requires layout(v), 0 <= i < v.records.len(), same_nonce(v.records[i].plan.nonce,n),
    ensures forall|j: int| 0 <= j < v.records.len() && same_nonce(v.records[j].plan.nonce,n) ==> j == i,
{
    assert forall|j: int| 0 <= j < v.records.len() && same_nonce(v.records[j].plan.nonce,n) implies j == i by {
        if j < i { assert(!same_nonce(v.records[j].plan.nonce,v.records[i].plan.nonce)); }
        if i < j { assert(!same_nonce(v.records[i].plan.nonce,v.records[j].plan.nonce)); }
    }
}
pub proof fn unique_generation(v: Native,g: u64,i: int)
    requires layout(v), 0 <= i < v.records.len(), v.records[i].plan.generation == g,
    ensures forall|j: int| 0 <= j < v.records.len() && v.records[j].plan.generation == g ==> j == i,
{
    assert forall|j: int| 0 <= j < v.records.len() && v.records[j].plan.generation == g implies j == i by {
        if j < i { assert(v.records[j].plan.generation < v.records[i].plan.generation); }
        if i < j { assert(v.records[i].plan.generation < v.records[j].plan.generation); }
    }
}
pub open spec fn outcome_at(v: Native,n: TxnId) -> Option<Outcome> {
    if exists|i: int| 0 <= i < v.records.len() && same_nonce(v.records[i].plan.nonce,n) {
        v.controls[choose|i: int| 0 <= i < v.records.len() && same_nonce(v.records[i].plan.nonce,n)].outcome
    } else { None }
}

/// Exact field effects checked on the executable bodies, not model-oracle guards.
pub open spec fn begin_effect(b: Native,z: Native,nonce: TxnId,src: u32,dst: u32,r: KeyRange,result: Result<u64,Status>) -> bool {
    match result {
        Err(_) => z == b,
        Ok(g) => {
            let i = b.records.len() as int;
            let rec = z.records[i];
            b.active is None && b.next_generation < u64::MAX && g == b.next_generation
            && i <= usize::MAX
            && src != dst && b.nodes.contains(src) && b.nodes.contains(dst)
            && (forall|j: int| 0 <= j < b.records.len() ==> !same_nonce(b.records[j].plan.nonce,nonce))
            && rec.slot < b.current.len() && rec.previous == b.current[rec.slot as int]
            && b.snapshots[rec.previous as int].table == r.table
            && rec.proposal == b.snapshots.len()
            && rec.plan.generation == g && rec.plan.nonce == nonce && rec.plan.source == src && rec.plan.destination == dst && rec.plan.range == r
            && equivalent(rec.plan.old@,b.snapshots[rec.previous as int].boundaries@)
            && proper(r.lo@,upper(r)) && source_owned(b.snapshots[rec.previous as int].boundaries@,r.lo@,upper(r),src)
            && z.snapshots.len() == b.snapshots.len()+1 && z.snapshots.drop_last() == b.snapshots
            && z.snapshots.last().table == r.table
            && (forall|k: Seq<u8>| route(z.snapshots.last().boundaries@,k) == if inside(k,r.lo@,upper(r)) { Grant { owner: dst, epoch: g } } else { route(b.snapshots[rec.previous as int].boundaries@,k) })
            && z == (Native { snapshots: z.snapshots, records: b.records.push(rec), controls: b.controls.push(initial_control()), active: Some(i as usize), next_generation: (g+1) as u64, ..b })
        }
    }
}
#[derive(Clone, Copy)]
pub enum Request { Freeze, Final, Retire, Commit, Abort, Finish }
pub open spec fn permitted(c: Control,r: Request) -> bool {
    match r { Request::Freeze => c.phase is Copy,
        Request::Final => c.phase is Freezing && c.received.drained,
        Request::Retire => c.phase is Final && c.received.ready,
        Request::Commit => c.phase is Retiring && c.received.retired,
        Request::Abort => !terminal(c.phase),
        Request::Finish => terminal(c.phase) && c.received.source_done && c.received.destination_done }
}
pub open spec fn changed(c: Control,r: Request,g: u64) -> Control {
    match r {
        Request::Freeze => Control { phase: Phase::Freezing, issued: Commands { freeze: true, ..c.issued }, ..c },
        Request::Final => Control { phase: Phase::Final, issued: Commands { final_copy: true, ..c.issued }, ..c },
        Request::Retire => Control { phase: Phase::Retiring, issued: Commands { retire: true, ..c.issued }, ..c },
        Request::Commit => Control { phase: Phase::Committed, issued: Commands { commit: true, ..c.issued }, outcome: Some(Outcome { generation: g, committed: true }), ..c },
        Request::Abort => Control { phase: Phase::Aborted, issued: Commands { abort: true, ..c.issued }, outcome: Some(Outcome { generation: g, committed: false }), ..c },
        Request::Finish => c,
    }
}
pub open spec fn transition_effect(b: Native,z: Native,r: Request,status: Status) -> bool {
    match b.active {
        None => status == Status::NotFound && z == b,
        Some(i) => if !permitted(b.controls[i as int],r) { status == Status::Retry && z == b }
        else { status == Status::Ok && z == (Native {
            controls: b.controls.update(i as int,changed(b.controls[i as int],r,b.records[i as int].plan.generation)),
            current: if r is Commit { b.current.update(b.records[i as int].slot as int,b.records[i as int].proposal) } else { b.current },
            published_version: if r is Commit { b.records[i as int].plan.generation } else { b.published_version },
            active: if r is Finish { None } else { b.active }, ..b }) }
    }
}
pub open spec fn receive_effect(b: Native,z: Native,g: u64,owner: u32,cert: Certificate,status: Status) -> bool {
    if exists|i: int| 0 <= i < b.records.len() && b.records[i].plan.generation == g {
        let i = choose|i: int| 0 <= i < b.records.len() && b.records[i].plan.generation == g;
        if receipt_owner(b.records[i].plan,cert) != owner { status == Status::Invalid && z == b }
        else { status == Status::Ok && z == (Native { controls: b.controls.update(i,
            Control { received: b.controls[i].received.with(cert), ..b.controls[i] }), ..b }) }
    } else { status == Status::NotFound && z == b }
}
pub open spec fn reply_effect(b: Native,z: Native,n: TxnId,out: Option<Outcome>) -> bool {
    out == outcome_at(b,n) && match out {
        None => z == b,
        Some(_) => {
            let i = choose|i: int| 0 <= i < b.records.len() && same_nonce(b.records[i].plan.nonce,n);
            z == (Native { controls: b.controls.update(i,Control { replied: true, ..b.controls[i] }), ..b })
        }
    }
}
} // verus!
