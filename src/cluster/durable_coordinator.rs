//! Admission-fenced durable wrapper of the native single-writer Coordinator.
//! The caller serializes the namespace and retains the captured admission on
//! queued work. Only successful native operations enter the atomic journal.
//! An uncertain/failed append fences the candidate; recovery replays KV bytes.
use vstd::prelude::*;
use crate::bytes::compare;
use crate::directory::RouteTable;
use crate::migration::{Coordinator, Request};
use crate::types::{Certificate, Command, KeyRange, Outcome, Phase, Status, TxnId};
use crate::durable_kv::{Admission, DurableKv, Journal, JournalError, Namespace};
pub use crate::durable_coordinator_codec::Operation;
use crate::durable_coordinator_codec::{encode_bootstrap, decode_bootstrap, encode_operation, decode_operation};
#[cfg(verus_keep_ghost)]
use crate::durable_coordinator_codec::{bootstrap_wire, operation_wire};
#[cfg(verus_keep_ghost)]
use crate::durable_codec::range_same;
#[cfg(verus_keep_ghost)]
use crate::migration::{self as native, Native};

verus! {
#[derive(Clone, Copy)]
pub enum Response { Begun(u64), Applied, Replied(Outcome) }
pub enum Error { Journal(JournalError), Native(Status), Inactive }

pub open spec fn same_intent(p: crate::types::MigrationPlan,n: TxnId,s: u32,d: u32,r: KeyRange) -> bool {
    native::same_nonce(p.nonce,n) && p.source == s && p.destination == d && range_same(p.range,r)
}
fn equal_range(a: &KeyRange,b: &KeyRange) -> (yes: bool)
    ensures yes == range_same(*a,*b),
{
    if a.table != b.table || compare(&a.lo,&b.lo) != 0 { return false; }
    match (&a.hi,&b.hi) {
        (None,None) => true,
        (Some(x),Some(y)) => compare(x,y) == 0,
        _ => false,
    }
}
/// Includes retained exact-intent retries, not a second generation allocator.
pub open spec fn effect(b: Native,z: Native,op: Operation,result: Result<Response,Status>) -> bool {
    match op {
        Operation::Begin { nonce,source,destination,range } => {
            if exists|i: int| 0 <= i < b.records.len() && native::same_nonce(b.records[i].plan.nonce,nonce) {
                let i = choose|i: int| 0 <= i < b.records.len() && native::same_nonce(b.records[i].plan.nonce,nonce);
                z == b && if same_intent(b.records[i].plan,nonce,source,destination,range) {
                    result == Ok(Response::Begun(b.records[i].plan.generation))
                } else { result == Err(Status::Invalid) }
            } else {
                match result {
                    Ok(Response::Begun(g)) => native::begin_effect(b,z,nonce,source,destination,range,Ok(g)),
                    Err(s) => native::begin_effect(b,z,nonce,source,destination,range,Err(s)),
                    _ => false,
                }
            }
        },
        Operation::Request(r) => match result {
            Ok(Response::Applied) => native::transition_effect(b,z,r,Status::Ok),
            Err(s) => s != Status::Ok && native::transition_effect(b,z,r,s), _ => false,
        },
        Operation::Receive { generation,owner,certificate } => match result {
            Ok(Response::Applied) => native::receive_effect(b,z,generation,owner,certificate,Status::Ok),
            Err(s) => s != Status::Ok && native::receive_effect(b,z,generation,owner,certificate,s), _ => false,
        },
        Operation::Reply(nonce) => match result {
            Ok(Response::Replied(outcome)) => native::reply_effect(b,z,nonce,Some(outcome)),
            Err(Status::Retry) => native::reply_effect(b,z,nonce,None), _ => false,
        },
    }
}
fn apply_begin(actor: &mut Coordinator,nonce: TxnId,source: u32,destination: u32,range: KeyRange)
    -> (out: Result<Response,Status>)
    requires old(actor).wf(),
    ensures final(actor).wf(),
        effect(old(actor)@,final(actor)@,Operation::Begin {nonce,source,destination,range},out),
        out is Err ==> final(actor)@ == old(actor)@,
{
    let ghost before = actor@;
    let retained = actor.plan_for_nonce(nonce);
    match retained {
        Some(plan) => {
            proof {
                let i = choose|i: int| 0 <= i < before.records.len()
                    && *plan == before.records[i].plan && native::same_nonce(plan.nonce,nonce);
                native::unique_nonce(before,nonce,i);
                assert(before.records[i].plan == *plan);
            }
            if plan.source != source || plan.destination != destination || !equal_range(&plan.range,&range) {
                return Err(Status::Invalid);
            }
            Ok(Response::Begun(plan.generation))
        },
        None => {
            proof {
                assert(forall|i: int| 0 <= i < before.records.len()
                    ==> !native::same_nonce(before.records[i].plan.nonce,nonce));
            }
            let result = actor.begin(nonce,source,destination,range);
            proof { assert(native::begin_effect(before,actor@,nonce,source,destination,range,result)); }
            match result { Ok(g) => Ok(Response::Begun(g)), Err(s) => Err(s) }
        },
    }
}
fn apply(actor: &mut Coordinator,op: Operation) -> (out: Result<Response,Status>)
    requires old(actor).wf(),
    ensures final(actor).wf(), effect(old(actor)@,final(actor)@,op,out),
        out is Err ==> final(actor)@ == old(actor)@,
{
    match op {
        Operation::Begin { nonce,source,destination,range } => apply_begin(actor,nonce,source,destination,range),
        Operation::Request(r) => {
            let s = match r {
                Request::Freeze => actor.request_freeze(), Request::Final => actor.request_final(),
                Request::Retire => actor.request_retire(), Request::Commit => actor.commit(),
                Request::Abort => actor.abort(), Request::Finish => actor.finish(),
            };
            if matches!(s,Status::Ok) { Ok(Response::Applied) } else { Err(s) }
        },
        Operation::Receive { generation,owner,certificate } => {
            let s = actor.receive(generation,owner,certificate);
            if matches!(s,Status::Ok) { Ok(Response::Applied) } else { Err(s) }
        },
        Operation::Reply(nonce) => match actor.reply(nonce) {
            Some(o) => Ok(Response::Replied(o)), None => Err(Status::Retry),
        },
    }
}
pub open spec fn initialized(v: Native,nodes: Seq<u32>,tables: Seq<RouteTable>) -> bool {
    native::layout(v) && native::bootstrap(nodes,tables) && v.nodes == nodes && v.snapshots == tables
    && nodes.len() <= usize::MAX && tables.len() <= usize::MAX
    && v.records.len() == 0 && v.controls.len() == 0 && v.active is None
    && v.next_generation == 1 && v.published_version == 0 && v.current.len() == tables.len()
    && (forall|i: int| 0 <= i < tables.len() ==> v.current[i] == i)
}
fn new_actor(nodes: Vec<u32>,tables: Vec<RouteTable>) -> (out: Result<Coordinator,Status>)
    ensures match out { Ok(actor) => actor.wf() && initialized(actor@,nodes@,tables@), Err(_) => true },
{
    let node_count = nodes.len();
    let table_count = tables.len();
    Coordinator::new(nodes,tables)
}
pub proof fn initial_replay(v: Native,nodes: Seq<u32>,tables: Seq<RouteTable>)
    requires initialized(v,nodes,tables),
    ensures replays(seq![bootstrap_wire(nodes,tables)],v),
{
    hide(initialized); hide(bootstrap_wire);
    assert(native::layout(v)) by { reveal(initialized); }
    let record = bootstrap_wire(nodes,tables);
    let records = seq![record];
    assert(records.len() == 1 && records[0] == record);
    assert(initialized(v,nodes,tables) && records[0] == bootstrap_wire(nodes,tables));
    let witness = (nodes,tables);
    assert(bootstrap_record(v,records[0],witness));
    assert(exists|nt: (Seq<u32>,Seq<RouteTable>)| #[trigger] bootstrap_record(v,records[0],nt));
}
fn replay_bootstrap(bytes: &[u8]) -> (out: Result<Coordinator,Status>)
    ensures match out { Ok(actor) => actor.wf() && replays(seq![bytes@],actor@), Err(_) => true },
{
    hide(replays);
    let (nodes,tables) = match decode_bootstrap(bytes) { Ok(v) => v, Err(e) => return Err(e) };
    let ghost ns = nodes@;
    let ghost ts = tables@;
    let actor = match new_actor(nodes,tables) { Ok(actor) => actor, Err(e) => return Err(e) };
    proof { initial_replay(actor@,ns,ts); }
    Ok(actor)
}
fn replay_entry(actor: &mut Coordinator,bytes: &[u8],Ghost(records): Ghost<Seq<Seq<u8>>>)
    -> (out: Result<(),Status>)
    requires old(actor).wf(), replays(records,old(actor)@),
    ensures final(actor).wf(), out is Ok ==> replays(records.push(bytes@),final(actor)@),
{
    hide(replays); hide(effect); hide(operation_wire);
    let op = match decode_operation(bytes) { Ok(op) => op, Err(e) => return Err(e) };
    let ghost before = actor@;
    let ghost input = op;
    let response = match apply(actor,op) { Ok(r) => r, Err(e) => return Err(e) };
    proof { extend_replay(records,before,actor@,input,response); }
    Ok(())
}
pub open spec fn bootstrap_record(v: Native,entry: Seq<u8>,nt: (Seq<u32>,Seq<RouteTable>)) -> bool {
    initialized(v,nt.0,nt.1) && entry == bootstrap_wire(nt.0,nt.1)
}
pub open spec fn operation_record(v: Native,entry: Seq<u8>,step: (Native,Operation,Response)) -> bool {
    entry == operation_wire(step.1) && effect(step.0,v,step.1,Ok(step.2))
}
/// A relational fold of *actual native effects* over committed byte records.
/// Witnesses are proof-only: executable recovery obtains every input from KV.
pub open spec fn replays(records: Seq<Seq<u8>>,v: Native) -> bool
    decreases records.len(),
{
    records.len() > 0 && native::layout(v) && if records.len() == 1 {
        exists|nt: (Seq<u32>,Seq<RouteTable>)| #[trigger] bootstrap_record(v,records[0],nt)
    } else {
        exists|step: (Native,Operation,Response)| replays(records.drop_last(),step.0)
            && #[trigger] operation_record(v,records.last(),step)
    }
}
pub proof fn native_counts(v: Native)
    requires native::layout(v),
    ensures v.records.len() == v.controls.len(),
        match v.active { Some(i) => i < v.records.len(), None => true },
{}
pub proof fn native_current_bound(v: Native,index: int)
    requires native::layout(v), 0 <= index < v.current.len(),
    ensures v.current[index] < v.snapshots.len(),
{}
pub proof fn native_record_bounds(v: Native,index: int)
    requires native::layout(v), 0 <= index < v.records.len(),
    ensures v.records[index].previous < v.snapshots.len(),
        v.records[index].proposal < v.snapshots.len(), v.records[index].slot < v.current.len(),
{}
pub proof fn replay_bounds(records: Seq<Seq<u8>>,v: Native)
    requires replays(records,v),
    ensures records.len() > 0, native::layout(v),
        v.records.len() == v.controls.len(),
{
    hide(effect); hide(initialized); hide(native::layout); hide(operation_wire); hide(bootstrap_wire);
    native_counts(v);
}
pub proof fn replay_base(records: Seq<Seq<u8>>,v: Native) -> (nt: (Seq<u32>,Seq<RouteTable>))
    requires replays(records,v), records.len() == 1,
    ensures initialized(v,nt.0,nt.1), records[0] == bootstrap_wire(nt.0,nt.1),
{
    hide(initialized); hide(bootstrap_wire); hide(native::layout);
    choose|nt: (Seq<u32>,Seq<RouteTable>)| #[trigger] bootstrap_record(v,records[0],nt)
}
pub proof fn replay_predecessor(records: Seq<Seq<u8>>,v: Native) -> (step: (Native,Operation,Response))
    requires replays(records,v), records.len() > 1,
    ensures replays(records.drop_last(),step.0), records.last() == operation_wire(step.1),
        effect(step.0,v,step.1,Ok(step.2)), native::layout(step.0), native::layout(v),
        step.0.records.len() == step.0.controls.len(),
{
    hide(effect); hide(operation_wire); hide(initialized); hide(native::layout);
    let step = choose|step: (Native,Operation,Response)| replays(records.drop_last(),step.0)
        && #[trigger] operation_record(v,records.last(),step);
    replay_bounds(records.drop_last(),step.0);
    step
}
pub proof fn extend_replay(records: Seq<Seq<u8>>,b: Native,z: Native,op: Operation,response: Response)
    requires replays(records,b), native::layout(z), effect(b,z,op,Ok(response)),
    ensures replays(records.push(operation_wire(op)),z),
{
    hide(effect); hide(initialized); hide(operation_wire); hide(bootstrap_wire); hide(native::layout);
    replay_bounds(records,b);
    let all = records.push(operation_wire(op));
    assert(all.drop_last() == records && all.last() == operation_wire(op));
    let witness = (b,op,response);
    assert(replays(all.drop_last(),witness.0) && operation_record(z,all.last(),witness));
    assert(exists|step: (Native,Operation,Response)| replays(all.drop_last(),step.0)
        && #[trigger] operation_record(z,all.last(),step));
}
/// Every possible backend-settled image has a native replay witness. This is a
/// wrapper theorem, never an assumption in the trusted byte-KV interface.
pub open spec fn recoverable<K: DurableKv>(kv: &K,namespace: Namespace) -> bool {
    forall|m: Map<Seq<u8>,Seq<u8>>| crate::durable_kv::settled_map(kv.committed(),kv.pending(),m) ==>
        exists|incarnation: u64,records: Seq<Seq<u8>>,v: Native|
            crate::durable_kv::has_prefix(m,namespace,incarnation,records) && replays(records,v)
}
pub proof fn recoverable_image<K: DurableKv>(kv: &K,namespace: Namespace,incarnation: u64,records: Seq<Seq<u8>>,v: Native)
    requires replays(records,v), crate::durable_kv::recoverable_prefix(kv,namespace,incarnation,records),
    ensures recoverable(kv,namespace),
{
    assert forall|m: Map<Seq<u8>,Seq<u8>>| crate::durable_kv::settled_map(kv.committed(),kv.pending(),m) implies
        exists|inc: u64,rs: Seq<Seq<u8>>,state: Native|
            crate::durable_kv::has_prefix(m,namespace,inc,rs) && replays(rs,state) by {
        assert(crate::durable_kv::has_prefix(m,namespace,incarnation,records));
    }
}
pub proof fn recoverable_append<K: DurableKv>(kv: &K,m: Map<Seq<u8>,Seq<u8>>,ns: Namespace,inc: u64,
    records: Seq<Seq<u8>>,before: Native,after: Native,op: Operation,response: Response)
    requires replays(records,before), native::layout(after), effect(before,after,op,Ok(response)),
        records.len() < u64::MAX, crate::durable_kv::has_prefix(m,ns,inc,records),
        crate::durable_kv::settlement_options(kv,m,crate::durable_kv::append_map(m,ns,inc,records,operation_wire(op))),
    ensures recoverable(kv,ns),
{
    extend_replay(records,before,after,op,response);
    crate::durable_kv::append_prefix(m,ns,inc,records,operation_wire(op));
    assert forall|candidate: Map<Seq<u8>,Seq<u8>>| crate::durable_kv::settled_map(kv.committed(),kv.pending(),candidate) implies
        exists|incarnation: u64,rs: Seq<Seq<u8>>,state: Native|
            crate::durable_kv::has_prefix(candidate,ns,incarnation,rs) && replays(rs,state) by {
        if candidate == m {
            assert(crate::durable_kv::has_prefix(candidate,ns,inc,records) && replays(records,before));
        } else {
            assert(crate::durable_kv::has_prefix(candidate,ns,inc,records.push(operation_wire(op)))
                && replays(records.push(operation_wire(op)),after));
        }
    }
}

pub struct DurableCoordinator {
    actor: Coordinator,
    journal: Journal,
    sound: bool,
}
impl DurableCoordinator {
    pub closed spec fn view(&self) -> Native { self.actor@ }
    pub closed spec fn records(&self) -> Seq<Seq<u8>> { self.journal.records() }
    pub closed spec fn namespace(&self) -> Namespace { self.journal.namespace_spec() }
    pub closed spec fn incarnation(&self) -> u64 { self.journal.incarnation_spec() }
    pub closed spec fn active(&self) -> bool { self.journal.active() }
    pub closed spec fn replayable(&self) -> bool { self.sound }
    pub proof fn durable_image<K: DurableKv>(&self,kv: &K,admission: Admission)
        requires self.wf(), self.consistent(kv), self.admitted(admission),
        ensures replays(self.records(),self.view()), recoverable(kv,self.namespace()),
            crate::durable_kv::has_prefix(kv.committed(),self.namespace(),self.incarnation(),self.records()),
    {
        self.journal.committed_records(kv);
        recoverable_image(kv,self.namespace(),self.incarnation(),self.records(),self.view());
    }
    pub closed spec fn wf(&self) -> bool {
        self.actor.wf() && self.journal.wf()
            && (self.sound ==> replays(self.journal.records(),self.actor@))
            && (!self.sound ==> !self.journal.active())
    }
    pub closed spec fn admitted(&self,a: Admission) -> bool {
        self.sound && self.journal.active() && a.namespace == self.journal.namespace_spec()
            && a.incarnation == self.journal.incarnation_spec()
    }
    pub closed spec fn consistent<K: DurableKv>(&self,kv: &K) -> bool { self.journal.consistent(kv) }
    pub fn create<K: DurableKv>(kv: &mut K,namespace: Namespace,nodes: Vec<u32>,tables: Vec<RouteTable>)
        -> (out: Result<Self,Error>)
        requires old(kv).wf(),
        ensures final(kv).wf(), match out { Ok(c) => c.wf() && c.consistent(final(kv))
            && !c.active() && c.replayable() && c.records() == seq![bootstrap_wire(nodes@,tables@)]
            && initialized(c.view(),nodes@,tables@)
            && c.namespace() == namespace && replays(c.records(),c.view()) && recoverable(final(kv),namespace),
            Err(Error::Journal(_)) => crate::durable_kv::bootstrap_options(final(kv),old(kv).committed(),
                old(kv).pending(),namespace,bootstrap_wire(nodes@,tables@))
                && (exists|v: Native| initialized(v,nodes@,tables@)),
            Err(_) => true },
    {
        hide(replays); hide(bootstrap_wire);
        if !namespace.coordinator { return Err(Error::Native(Status::Invalid)); }
        let bytes = match encode_bootstrap(&nodes,&tables) { Ok(b) => b, Err(e) => return Err(Error::Native(e)) };
        let ghost ns = nodes@;
        let ghost ts = tables@;
        let actor = match new_actor(nodes,tables) { Ok(a) => a, Err(e) => return Err(Error::Native(e)) };
        proof {
            assert(initialized(actor@,ns,ts));
            assert(exists|v: Native| initialized(v,ns,ts));
        }
        let journal = match Journal::create(kv,namespace,bytes) { Ok(j) => j, Err(e) => return Err(Error::Journal(e)) };
        proof {
            initial_replay(actor@,ns,ts);
            journal.committed_records(kv);
            recoverable_image(kv,namespace,journal.incarnation_spec(),journal.records(),actor@);
        }
        Ok(Self { actor,journal,sound: true })
    }
    pub fn recover<K: DurableKv>(kv: &mut K,namespace: Namespace) -> (out: Result<Self,Error>)
        requires old(kv).wf(),
        ensures final(kv).wf(), match out { Ok(c) => c.wf() && c.consistent(final(kv))
            && !c.active() && c.replayable() && replays(c.records(),c.view())
            && c.namespace() == namespace && recoverable(final(kv),namespace), Err(_) => true },
    {
        hide(replays); hide(effect); hide(bootstrap_wire); hide(operation_wire);
        if !namespace.coordinator { return Err(Error::Native(Status::Invalid)); }
        let mut journal = match Journal::recover(kv,namespace) { Ok(j) => j, Err(e) => return Err(Error::Journal(e)) };
        let first = match journal.read_record(kv,0) { Ok(b) => b, Err(e) => return Err(Error::Journal(e)) };
        proof { journal.committed_records(kv); }
        let mut actor = match replay_bootstrap(&first) { Ok(a) => a, Err(_) => return Err(Error::Journal(JournalError::Corrupt)) };
        proof { assert(journal.records().take(1) == seq![first@]); }
        let mut i = 1u64;
        while i < journal.count()
            invariant kv.wf(), journal.wf(), journal.consistent(kv), !journal.active(),
                journal.namespace_spec() == namespace, journal.records().len() <= u64::MAX,
                actor.wf(), 1 <= i <= journal.records().len(),
                replays(journal.records().take(i as int),actor@),
            decreases journal.records().len()-i,
        {
            let bytes = match journal.read_record(kv,i) { Ok(b) => b, Err(e) => return Err(Error::Journal(e)) };
            match replay_entry(&mut actor,&bytes,Ghost(journal.records().take(i as int))) {
                Ok(()) => {},
                Err(_) => return Err(Error::Journal(JournalError::Corrupt)),
            }
            proof { assert(journal.records().take(i as int+1) == journal.records().take(i as int).push(bytes@)); }
            i += 1;
        }
        proof {
            assert(journal.records().take(i as int) == journal.records());
            journal.committed_records(kv);
            recoverable_image(kv,namespace,journal.incarnation_spec(),journal.records(),actor@);
        }
        Ok(Self { actor,journal,sound: true })
    }
    /// Failed candidates can never be reactivated; discard and recover them.
    pub fn activate<K: DurableKv>(&mut self,kv: &mut K) -> (out: Result<Admission,Error>)
        requires old(self).wf(), old(kv).wf(), old(self).consistent(old(kv)),
        ensures final(self).wf(), final(kv).wf(),
            final(self).view() == old(self).view(), final(self).records() == old(self).records(),
            replays(old(self).records(),old(self).view()) ==> recoverable(final(kv),old(self).namespace()),
            match out { Ok(a) => final(self).admitted(a) && final(self).consistent(final(kv)), Err(_) => true },
    {
        proof {
            self.journal.committed_records(kv);
            if replays(self.records(),self.view()) {
                recoverable_image(kv,self.namespace(),self.incarnation(),self.records(),self.view());
            }
        }
        if !self.sound { return Err(Error::Inactive); }
        let ghost map = kv.committed();
        let ghost ns = self.namespace();
        let ghost inc = self.incarnation();
        let ghost records = self.records();
        let ghost state = self.view();
        let result = self.journal.activate(kv);
        proof {
            crate::durable_kv::activate_prefix(map,ns,inc,(inc+1) as u64,records);
            assert forall|candidate: Map<Seq<u8>,Seq<u8>>| crate::durable_kv::settled_map(kv.committed(),kv.pending(),candidate) implies
                exists|incarnation: u64,rs: Seq<Seq<u8>>,v: Native|
                    crate::durable_kv::has_prefix(candidate,ns,incarnation,rs) && replays(rs,v) by {
                if candidate == map {
                    assert(crate::durable_kv::has_prefix(candidate,ns,inc,records) && replays(records,state));
                } else {
                    assert(crate::durable_kv::has_prefix(candidate,ns,(inc+1) as u64,records) && replays(records,state));
                }
            }
        }
        match result { Ok(a) => Ok(a), Err(e) => Err(Error::Journal(e)) }
    }
    /// Encode first; apply privately; append atomically; only then publish result.
    /// Receive inputs must be authenticated receipts from the indicated native
    /// participant role; journal persistence does not manufacture that evidence.
    pub fn execute<K: DurableKv>(&mut self,kv: &mut K,admission: &Admission,op: Operation)
        -> (out: Result<Response,Error>)
        requires old(self).wf(), old(kv).wf(), old(self).consistent(old(kv)),
        ensures final(self).wf(), final(kv).wf(),
            old(self).admitted(*admission) ==> recoverable(final(kv),old(self).namespace()),
            match out {
                Ok(response) => old(self).admitted(*admission) && final(self).admitted(*admission)
                    && final(self).consistent(final(kv))
                    && effect(old(self).view(),final(self).view(),op,Ok(response))
                    && final(self).records() == old(self).records().push(operation_wire(op))
                    && replays(final(self).records(),final(self).view()),
                Err(Error::Journal(_)) => !final(self).replayable() && !final(self).active(),
                Err(_) => final(self).view() == old(self).view(),
            },
    {
        proof { if self.sound {
            self.journal.committed_records(kv);
            recoverable_image(kv,self.namespace(),self.incarnation(),self.records(),self.view());
        } }
        if !self.sound || !self.journal.check(admission) { return Err(Error::Inactive); }
        if self.journal.count() == u64::MAX {
            self.sound = false; self.journal.fence();
            return Err(Error::Journal(JournalError::Exhausted));
        }
        let bytes = match encode_operation(&op) { Ok(b) => b, Err(e) => return Err(Error::Native(e)) };
        let ghost before = self.actor@;
        let ghost records = self.journal.records();
        let ghost input = op;
        let ghost map = kv.committed();
        let ghost ns = self.namespace();
        let ghost inc = self.incarnation();
        let response = match apply(&mut self.actor,op) { Ok(r) => r, Err(e) => return Err(Error::Native(e)) };
        let result = self.journal.append(kv,bytes);
        proof { recoverable_append(kv,map,ns,inc,records,before,self.actor@,input,response); }
        match result {
            Ok(()) => {
                proof { extend_replay(records,before,self.actor@,input,response); }
                Ok(response)
            },
            Err(e) => { self.sound = false; self.journal.fence(); Err(Error::Journal(e)) },
        }
    }
    /// No mutable actor/journal handle exists in this API. The immutable borrow
    /// also prevents append/activation while a caller observes routing or plans.
    pub fn actor(&self,admission: &Admission) -> (out: Result<&Coordinator,Error>)
        requires self.wf(),
        ensures match out { Ok(c) => self.admitted(*admission) && c.wf() && c@ == self.view()
            && replays(self.records(),c@), Err(_) => !self.admitted(*admission) },
    {
        if !self.sound || !self.journal.check(admission) { return Err(Error::Inactive); }
        Ok(&self.actor)
    }
}

/// Work is derived from durable phase and receipts, never from a fresh Copy
/// assumption. Start/Freeze/Final include their ordinary idempotent metadata
/// delivery before the existing drain/final-copy worker is invoked.
/// After StartDestination succeeds, execute Request::Freeze under the same
/// captured admission. Drain, final-copy, retire and decision actions feed their
/// authenticated certificates back through Operation::Receive. Advance actions
/// are journaled through execute; never call a mutable native actor directly.
pub enum ResumeAction {
    StartDestination { destination: u32 },
    FreezeAndDrain { source: u32 },
    FinalCopy { destination: u32 },
    RetireSource { source: u32 },
    DeliverDecision { owner: u32, command: Command, certificate: Certificate },
    Advance(Request),
}
pub enum ResumeWork { Idle, Active { generation: u64, phase: Phase, action: ResumeAction } }
pub open spec fn resume_matches(v: Native,work: ResumeWork) -> bool {
    match (v.active,work) {
        (None,ResumeWork::Idle) => true,
        (Some(i),ResumeWork::Active {generation,phase,action}) => {
            let p = v.records[i as int].plan; let c = v.controls[i as int];
            generation == p.generation && phase == c.phase && match action {
                ResumeAction::StartDestination {destination} => phase is Copy && c.issued.start && destination == p.destination,
                ResumeAction::FreezeAndDrain {source} => phase is Freezing && c.issued.freeze && !c.received.drained && source == p.source,
                ResumeAction::FinalCopy {destination} => phase is Final && c.issued.final_copy && !c.received.ready && destination == p.destination,
                ResumeAction::RetireSource {source} => phase is Retiring && c.issued.retire && !c.received.retired && source == p.source,
                ResumeAction::Advance(r) => native::permitted(c,r),
                ResumeAction::DeliverDecision {owner,command,certificate} =>
                    native::terminal(phase) && c.issued.has(command)
                    && command == (if phase is Committed {Command::Commit} else {Command::Abort})
                    && ((!c.received.source_done && owner == p.source && certificate is SourceDone)
                        || (c.received.source_done && !c.received.destination_done
                            && owner == p.destination && certificate is DestinationDone)),
            }
        },
        _ => false,
    }
}
impl DurableCoordinator {
    pub fn resume(&self,admission: &Admission) -> (out: Result<ResumeWork,Error>)
        requires self.wf(),
        ensures match out { Ok(work) => self.admitted(*admission) && resume_matches(self.view(),work), Err(_) => true },
    {
        let actor = match self.actor(admission) { Ok(a) => a, Err(e) => return Err(e) };
        let Some(plan) = actor.active_plan() else { return Ok(ResumeWork::Idle); };
        let Some(control) = actor.active_control() else { return Err(Error::Native(Status::Invalid)); };
        let action = match control.phase {
            Phase::Copy => {
                if !control.issued.start { return Err(Error::Native(Status::Invalid)); }
                ResumeAction::StartDestination { destination: plan.destination }
            },
            Phase::Freezing => {
                if !control.issued.freeze { return Err(Error::Native(Status::Invalid)); }
                if control.received.drained { ResumeAction::Advance(Request::Final) }
                else { ResumeAction::FreezeAndDrain { source: plan.source } }
            },
            Phase::Final => {
                if !control.issued.final_copy { return Err(Error::Native(Status::Invalid)); }
                if control.received.ready { ResumeAction::Advance(Request::Retire) }
                else { ResumeAction::FinalCopy { destination: plan.destination } }
            },
            Phase::Retiring => {
                if !control.issued.retire { return Err(Error::Native(Status::Invalid)); }
                if control.received.retired { ResumeAction::Advance(Request::Commit) }
                else { ResumeAction::RetireSource { source: plan.source } }
            },
            Phase::Committed | Phase::Aborted => {
                let command = if matches!(control.phase,Phase::Committed) { Command::Commit } else { Command::Abort };
                if (matches!(control.phase,Phase::Committed) && !control.issued.commit)
                    || (matches!(control.phase,Phase::Aborted) && !control.issued.abort) {
                    return Err(Error::Native(Status::Invalid));
                }
                if !control.received.source_done {
                    ResumeAction::DeliverDecision { owner: plan.source,command,certificate: Certificate::SourceDone }
                } else if !control.received.destination_done {
                    ResumeAction::DeliverDecision { owner: plan.destination,command,certificate: Certificate::DestinationDone }
                } else { ResumeAction::Advance(Request::Finish) }
            },
        };
        Ok(ResumeWork::Active { generation: plan.generation,phase: control.phase,action })
    }
}
} // verus!
