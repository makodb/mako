//! Local metadata and exact admissions. The embedding driver holds one mutex
//! across these methods and the native storage operation they authorize.
//! Storage completion is represented by unforgeable driver-owned capabilities;
//! a successful metadata transition alone is never a cleanup acknowledgement.
use vstd::prelude::*;
use std::collections::HashMap;
use crate::bytes::{compare, contains};
use crate::directory::RouteTable;
use crate::leases::LeaseRegistry;
use crate::types::*;
use crate::storage::{CompletedCopy, CompletedCleanup};
#[path = "participant_metadata.rs"]
mod metadata;
pub use metadata::*;
#[cfg(verus_keep_ghost)]
#[path = "participant_control_proofs.rs"]
mod control_proofs;

verus! {
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Cleanup { None, Source, Destination }

pub struct ControlResult {
    pub status: Status,
    pub certificate: Option<Certificate>,
    pub cleanup: Cleanup,
}

impl ControlResult {
    fn status(status: Status) -> (out: Self)
        ensures out.status == status, out.certificate == None, out.cleanup == Cleanup::None,
    {
        Self { status, certificate: None, cleanup: Cleanup::None }
    }
    fn certificate(certificate: Certificate) -> (out: Self)
        ensures out.status == Status::Ok, out.certificate == Some(certificate), out.cleanup == Cleanup::None,
    {
        Self { status: Status::Ok, certificate: Some(certificate), cleanup: Cleanup::None }
    }
}


pub open spec fn command_bit(command: Command) -> u8 {
    match command { Command::Start => 1, Command::Freeze => 2, Command::Final => 4,
        Command::Retire => 8, Command::Commit => 16, Command::Abort => 32 }
}
fn bit(command: Command) -> (out: u8)
    ensures out > 0, out == command_bit(command),
{
    match command { Command::Start => 1, Command::Freeze => 2, Command::Final => 4,
        Command::Retire => 8, Command::Commit => 16, Command::Abort => 32 }
}


#[derive(Clone, Copy)]
struct Receipt { applied: u8, cleanup: Cleanup, complete: bool, ready: bool }

pub struct Participant {
    owner: u32,
    tables: HashMap<u64, MetaTable>,
    leases: LeaseRegistry,
    receipts: HashMap<u64, Receipt>,
}

impl Participant {
    pub closed spec fn wf(&self) -> bool {
        self.leases.wf() && forall|table: u64| self.tables@.contains_key(table) ==> meta_wf(self.tables@[table].boundaries@)
    }
    pub closed spec fn lease_view(&self) -> Map<u64,crate::leases::SessionView> { self.leases@ }
    pub closed spec fn owner_view(&self) -> u32 { self.owner }
    pub closed spec fn same_metadata(&self, before: Self) -> bool {
        self.owner == before.owner && self.tables@ == before.tables@ && self.receipts@ == before.receipts@
    }
    pub proof fn metadata_reflexive(&self)
        ensures self.same_metadata(*self),
    {}
    pub closed spec fn drained_view(&self, range: KeyRange) -> bool { self.leases.drained_spec(range) }
    pub proof fn lease_facts(&self)
        requires self.wf(),
        ensures forall|client:u64| self.lease_view().contains_key(client) ==> self.lease_view()[client].wf(),
    {}

    pub fn new(owner: u32, tables: Vec<RouteTable>) -> (out: Result<Self,Status>)
        ensures match out { Ok(p) => p.wf(), Err(_) => true },
    {
        let mut local: HashMap<u64,MetaTable> = HashMap::new();
        let mut t = 0usize;
        while t < tables.len()
            invariant t <= tables.len(),
                forall|table: u64| local@.contains_key(table) ==> meta_wf(local@[table].boundaries@),
            decreases tables.len() - t,
        {
            let table = &tables[t];
            if local.contains_key(&table.table) || table.boundaries.len() == 0 { return Err(Status::Invalid); }
            if table.boundaries[0].start.len() != 0 { return Err(Status::Invalid); }
            let mut boundaries: Vec<MetaBoundary> = Vec::new();
            let mut i = 0usize;
            while i < table.boundaries.len()
                invariant i <= table.boundaries.len(),
                    table.boundaries.len() > 0, table.boundaries@[0].start@ == Seq::<u8>::empty(),
                    (boundaries.len() == 0 <==> i == 0), meta_ordered(boundaries@),
                    boundaries.len() > 0 ==> meta_wf(boundaries@),
                    i > 0 ==> forall|j: int| 0 <= j < boundaries.len() ==>
                        crate::bytes::cmp_spec(boundaries@[j].start@,table.boundaries@[i as int-1].start@) <= 0,
                decreases table.boundaries.len() - i,
            {
                let b = &table.boundaries[i];
                if b.grant.epoch != 0 { return Err(Status::Invalid); }
                if i > 0 && compare(&table.boundaries[i-1].start,&b.start) >= 0 { return Err(Status::Invalid); }
                proof {
                    assert forall|j: int| 0 <= j < boundaries.len() implies crate::bytes::cmp_spec(boundaries@[j].start@,b.start@) < 0 by {
                        crate::bytes::cmp_trans(boundaries@[j].start@,table.boundaries@[i as int-1].start@,b.start@);
                    }
                }
                MetaTable::push(&mut boundaries,&b.start,ReplicaMeta { epoch: 0, fence: 0,
                    terminal: true, role: if b.grant.owner == owner { Role::Serving } else { Role::Empty }, round: 0 });
                i += 1;
            }
            local.insert(table.table,MetaTable { boundaries });
            t += 1;
        }
        Ok(Self { owner, tables: local, leases: LeaseRegistry::new(), receipts: HashMap::new() })
    }

    pub closed spec fn table_wf(&self, table: u64) -> bool {
        self.tables@.contains_key(table) ==> meta_wf(self.tables@[table].boundaries@)
    }
    pub closed spec fn local_meta(&self, table: u64, coordinate: Seq<u8>) -> Option<ReplicaMeta> {
        if self.tables@.contains_key(table) { meta_route(self.tables@[table].boundaries@,coordinate) } else { None }
    }
    pub closed spec fn open_hold(&self, id: TxnId, table: u64, coordinate: Seq<u8>, grant: Grant) -> bool {
        self.leases.holds(id,table,coordinate,grant) && !self.leases@[id.client].terminal
    }
    pub closed spec fn open_range(&self, id: TxnId, table: u64, lo: Seq<u8>, hi: Option<Seq<u8>>, grant: Grant) -> bool {
        self.leases.holds_range(id,table,lo,hi,grant) && !self.leases@[id.client].terminal
    }
    pub fn owner(&self) -> (out: u32) ensures out == self.owner_view(), { self.owner }
    pub fn meta(&self, table: u64, coordinate: &[u8]) -> (out: Option<ReplicaMeta>)
        ensures self.table_wf(table) ==> out == self.local_meta(table,coordinate@),
    {
        match self.tables.get(&table) { Some(t) => t.lookup(coordinate), None => None }
    }
    pub fn begin(&mut self, id: TxnId) -> (out: Status)
        requires old(self).wf(), ensures final(self).wf(), final(self).same_metadata(*old(self)),
            crate::leases::begin_effect(old(self).lease_view(),final(self).lease_view(),id,out),
    { self.leases.begin(id) }
    pub fn resolve(&mut self, id: TxnId) -> (out: Status)
        requires old(self).wf(), ensures final(self).wf(), final(self).same_metadata(*old(self)),
            crate::leases::resolve_effect(old(self).lease_view(),final(self).lease_view(),id,out),
    { self.leases.resolve(id) }
    /// The engine calls this only after transaction commit/abort and native
    /// cleanup are terminal. It does not certify the engine's atomicity.
    pub fn finish(&mut self, id: TxnId) -> (out: Status)
        requires old(self).wf(),
        ensures final(self).wf(), crate::leases::finish_effect(old(self).lease_view(),final(self).lease_view(),id,out),
            final(self).same_metadata(*old(self)),
    { self.leases.finish(id) }
    pub fn release(&mut self, id: TxnId, table: u64, coordinate: &[u8]) -> (out: Status)
        requires old(self).wf(), ensures final(self).wf(), final(self).same_metadata(*old(self)),
            crate::leases::release_effect(old(self).lease_view(),final(self).lease_view(),id,table,coordinate@,out),
    { self.leases.release(id,table,coordinate) }
    pub fn held(&self, id: TxnId, table: u64, coordinate: &[u8]) -> (out: Option<Grant>)
        requires self.wf(),
        ensures match out { Some(g) => self.open_hold(id,table,coordinate@,g), None => true },
    { self.leases.held(id,table,coordinate) }

    /// Registration and local admission share this exclusive borrow. Existing
    /// open holds are not readmitted and remain usable while the range is Frozen.
    pub fn acquire(&mut self, id: TxnId, table: u64, coordinate: &[u8], grant: Grant) -> (out: Status)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).same_metadata(*old(self)),
            out == Status::Ok ==> final(self).open_hold(id,table,coordinate@,grant) && grant.owner == old(self).owner_view(),
            out == Status::Ok && !old(self).open_hold(id,table,coordinate@,grant) && old(self).table_wf(table) ==>
                old(self).local_meta(table,coordinate@).is_some()
                && old(self).local_meta(table,coordinate@).unwrap().role is Serving
                && old(self).local_meta(table,coordinate@).unwrap().epoch == grant.epoch,
            out != Status::Ok ==> final(self).lease_view() == old(self).lease_view(),
            out == Status::Ok ==> final(self).lease_view() == old(self).lease_view()
                || crate::leases::acquire_effect(old(self).lease_view(),final(self).lease_view(),id,table,coordinate@,grant,out),
            final(self).lease_view() != old(self).lease_view() ==> !old(self).open_hold(id,table,coordinate@,grant),
    {
        if grant.owner != self.owner { return Status::Retry; }
        if let Some(held) = self.leases.held(id,table,coordinate) {
            return if held.owner == grant.owner && held.epoch == grant.epoch { Status::Ok } else { Status::Retry };
        }
        match self.meta(table,coordinate) {
            Some(m) => {
                if !matches!(m.role,Role::Serving) || m.epoch != grant.epoch { return Status::Retry; }
                self.leases.acquire(id,table,coordinate,grant)
            }
            None => Status::NotFound,
        }
    }

    /// The router splits a cross-grant scan before this call. Each uniform
    /// segment is admitted and registered under this same exclusive borrow.
    pub fn acquire_range(&mut self, id: TxnId, table: u64, lo: &[u8], hi: Option<&[u8]>, grant: Grant) -> (out: Status)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).same_metadata(*old(self)),
            out == Status::Ok ==> final(self).open_range(id,table,lo@,crate::directory::hi_view(hi),grant)
                && grant.owner == old(self).owner_view(),
            out == Status::Ok && !old(self).open_range(id,table,lo@,crate::directory::hi_view(hi),grant) ==>
                forall|k: Seq<u8>| crate::directory::inside(k,lo@,crate::directory::hi_view(hi)) ==>
                    old(self).local_meta(table,k).is_some()
                    && old(self).local_meta(table,k).unwrap().role is Serving
                    && old(self).local_meta(table,k).unwrap().epoch == grant.epoch,
            out != Status::Ok ==> final(self).lease_view() == old(self).lease_view(),
            out == Status::Ok ==> final(self).lease_view() == old(self).lease_view()
                || crate::leases::acquire_range_effect(old(self).lease_view(),final(self).lease_view(),id,table,lo@,crate::directory::hi_view(hi),grant,out),
            final(self).lease_view() != old(self).lease_view() ==> !old(self).open_range(id,table,lo@,crate::directory::hi_view(hi),grant),
    {
        if let Some(h) = hi { if compare(lo,h) >= 0 { return Status::Invalid; } }
        if grant.owner != self.owner { return Status::Retry; }
        if let Some(held) = self.leases.held_range(id,table,lo,hi) {
            return if held.owner == grant.owner && held.epoch == grant.epoch { Status::Ok } else { Status::Retry };
        }
        let local = match self.tables.get(&table) { Some(t) => t, None => return Status::NotFound };
        if !local.serving_range(lo,hi,grant.epoch) { return Status::Retry; }
        self.leases.acquire_range(id,table,lo,hi,grant)
    }

    pub fn held_range(&self, id: TxnId, table: u64, lo: &[u8], hi: Option<&[u8]>) -> (out: Option<Grant>)
        requires self.wf(),
        ensures match out { Some(g) => self.open_range(id,table,lo@,crate::directory::hi_view(hi),g), None => true },
    { self.leases.held_range(id,table,lo,hi) }

    fn envelope(&self, plan: &MigrationPlan) -> (yes: bool)
        ensures yes ==> plan.generation > 0 && plan.source != plan.destination
            && (self.owner == plan.source || self.owner == plan.destination)
            && crate::directory::proper(plan.range.lo@,match plan.range.hi { Some(h) => Some(h@), None => None })
            && crate::directory::wellformed(plan.old@) && self.tables@.contains_key(plan.range.table),
    {
        if plan.generation == 0 || plan.source == plan.destination
            || (self.owner != plan.source && self.owner != plan.destination) { return false; }
        if let Some(hi) = &plan.range.hi { if compare(&plan.range.lo,hi) >= 0 { return false; } }
        if plan.old.len() == 0 || plan.old[0].start.len() != 0 { return false; }
        let mut i = 0usize;
        while i < plan.old.len()
            invariant i <= plan.old.len(), plan.old.len() > 0, plan.old@[0].start@ == Seq::<u8>::empty(),
                crate::directory::ordered(plan.old@.take(i as int)),
            decreases plan.old.len() - i,
        {
            if i > 0 && compare(&plan.old[i-1].start,&plan.old[i].start) >= 0 { return false; }
            proof {
                assert forall|a: int| 0 <= a < i implies crate::bytes::cmp_spec(plan.old@[a].start@,plan.old@[i as int].start@) < 0 by {
                    if a < i-1 {
                        assert(plan.old@.take(i as int)[a] == plan.old@[a]);
                        assert(plan.old@.take(i as int)[i as int-1] == plan.old@[i as int-1]);
                        crate::bytes::cmp_trans(plan.old@[a].start@,plan.old@[i as int-1].start@,plan.old@[i as int].start@);
                    }
                }
            }
            i += 1;
            proof {
                assert forall|a: int,b: int| 0 <= a < b < i implies
                    crate::bytes::cmp_spec(plan.old@.take(i as int)[a].start@,plan.old@.take(i as int)[b].start@) < 0 by {
                    if b < i-1 {
                        assert(plan.old@.take(i as int-1)[a] == plan.old@[a]);
                        assert(plan.old@.take(i as int-1)[b] == plan.old@[b]);
                    }
                }
            }
        }
        proof { assert(plan.old@.take(plan.old.len() as int) =~= plan.old@); }
        self.tables.contains_key(&plan.range.table)
    }

    fn old_grant(plan: &MigrationPlan, coordinate: &[u8]) -> (out: Option<Grant>)
        requires crate::directory::wellformed(plan.old@),
        ensures out == Some(crate::directory::route(plan.old@,coordinate@)),
    {
        if plan.old.len() == 0 { return None; }
        let mut lo = 1usize;
        let mut hi = plan.old.len();
        proof { crate::bytes::cmp_laws(Seq::empty(),coordinate@); }
        while lo < hi
            invariant 1 <= lo <= hi <= plan.old.len(), crate::directory::wellformed(plan.old@),
                forall|j: int| 0 <= j < lo ==> crate::bytes::cmp_spec(plan.old@[j].start@,coordinate@) <= 0,
                forall|j: int| hi <= j < plan.old.len() ==> crate::bytes::cmp_spec(plan.old@[j].start@,coordinate@) > 0,
            decreases hi - lo,
        {
            let mid = lo + (hi - lo) / 2;
            if compare(&plan.old[mid].start,coordinate) <= 0 {
                proof {
                    assert forall|j: int| 0 <= j <= mid implies crate::bytes::cmp_spec(plan.old@[j].start@,coordinate@) <= 0 by {
                        if j < mid { crate::bytes::cmp_trans(plan.old@[j].start@,plan.old@[mid as int].start@,coordinate@); }
                    }
                }
                lo = mid + 1;
            } else {
                proof {
                    crate::bytes::cmp_laws(plan.old@[mid as int].start@,coordinate@);
                    assert forall|j: int| mid <= j < plan.old.len() implies crate::bytes::cmp_spec(plan.old@[j].start@,coordinate@) > 0 by {
                        if j > mid {
                            crate::bytes::cmp_trans(coordinate@,plan.old@[mid as int].start@,plan.old@[j].start@);
                            crate::bytes::cmp_laws(coordinate@,plan.old@[j].start@);
                        }
                    }
                }
                hi = mid;
            }
        }
        proof { crate::directory_proofs::selected_route(plan.old@,lo as int-1,coordinate@); }
        Some(plan.old[lo-1].grant)
    }
    pub open spec fn command_at(&self, plan: MigrationPlan, command: Command, drained: bool, coordinate: Seq<u8>) -> bool {
        let m = self.local_meta(plan.range.table,coordinate);
        let old = crate::directory::route(plan.old@,coordinate);
        m.is_some() && old.owner == plan.source && guard_spec(m.unwrap(),old.epoch,plan.generation,command,
            self.owner_view() == plan.source,self.owner_view() == plan.destination,drained)
    }
    fn point_guard(&self, plan: &MigrationPlan, command: Command, coordinate: &[u8], drained: bool) -> (yes: bool)
        requires self.wf(), crate::directory::wellformed(plan.old@),
        ensures yes == self.command_at(*plan,command,drained,coordinate@),
    {
        match (self.meta(plan.range.table,coordinate), Self::old_grant(plan,coordinate)) {
            (Some(m),Some(old)) => old.owner == plan.source && command_guard(m,old.epoch,plan.generation,
                command,self.owner == plan.source,self.owner == plan.destination,drained),
            _ => false,
        }
    }
    fn range_guard(&self, plan: &MigrationPlan, command: Command, drained: bool) -> (yes: bool)
        requires self.wf(), crate::directory::wellformed(plan.old@),
        ensures yes ==> forall|k: Seq<u8>| crate::bytes::contains_spec(plan.range,plan.range.table,k) ==>
            self.command_at(*plan,command,drained,k),
    {
        if !self.point_guard(plan,command,&plan.range.lo,drained) { return false; }
        if let Some(table) = self.tables.get(&plan.range.table) {
            let mut i = 0usize;
            while i < table.boundaries.len()
                invariant i <= table.boundaries.len(), self.wf(), crate::directory::wellformed(plan.old@),
                    self.command_at(*plan,command,drained,plan.range.lo@),
                    forall|j: int| 0 <= j < i && crate::bytes::contains_spec(plan.range,plan.range.table,table.boundaries@[j].start@) ==>
                        self.command_at(*plan,command,drained,table.boundaries@[j].start@),
                decreases table.boundaries.len() - i,
            {
                let k = &table.boundaries[i].start;
                if contains(&plan.range,plan.range.table,k) && !self.point_guard(plan,command,k,drained) { return false; }
                i += 1;
            }
        } else { return false; }
        // Freeze's old epoch may change within one local metadata segment.
        let mut i = 0usize;
        while i < plan.old.len()
            invariant i <= plan.old.len(), self.wf(), crate::directory::wellformed(plan.old@),
                self.tables@.contains_key(plan.range.table),
                self.command_at(*plan,command,drained,plan.range.lo@),
                forall|j: int| 0 <= j < self.tables@[plan.range.table].boundaries.len()
                    && crate::bytes::contains_spec(plan.range,plan.range.table,self.tables@[plan.range.table].boundaries@[j].start@) ==>
                        self.command_at(*plan,command,drained,self.tables@[plan.range.table].boundaries@[j].start@),
                forall|j: int| 0 <= j < i && crate::bytes::contains_spec(plan.range,plan.range.table,plan.old@[j].start@) ==>
                    self.command_at(*plan,command,drained,plan.old@[j].start@),
            decreases plan.old.len() - i,
        {
            let k = &plan.old[i].start;
            if contains(&plan.range,plan.range.table,k) && !self.point_guard(plan,command,k,drained) { return false; }
            i += 1;
        }
        proof {
            let predicate: spec_fn(ReplicaMeta,Grant)->bool = |m: ReplicaMeta,g: Grant| g.owner == plan.source && guard_spec(m,g.epoch,plan.generation,command,
                self.owner == plan.source,self.owner == plan.destination,drained);
            let upper: Option<Seq<u8>> = match &plan.range.hi { Some(h) => Some(h@), None => None };
            control_proofs::joint_uniform(self.tables@[plan.range.table].boundaries@,plan.old@,
                plan.range.lo@,upper,predicate);
            assert forall|k: Seq<u8>| crate::bytes::contains_spec(plan.range,plan.range.table,k) implies self.command_at(*plan,command,drained,k) by {
                assert(crate::directory::inside(k,plan.range.lo@,upper));
                assert(control_proofs::joint(self.tables@[plan.range.table].boundaries@,plan.old@,k,predicate));
            }
        }
        true
    }

    fn result(&self, plan: &MigrationPlan, command: Command, receipt: Receipt) -> (out: ControlResult)
        ensures out.status == Status::Ok,
            out.certificate.is_some() ==> out.cleanup == Cleanup::None,
            out.certificate == Some(Certificate::Retired) ==> command == Command::Retire,
            out.certificate == Some(Certificate::SourceDone) ==> (command == Command::Commit || command == Command::Abort) && self.owner_view() == plan.source,
            out.certificate == Some(Certificate::DestinationDone) ==> (command == Command::Commit || command == Command::Abort) && self.owner_view() != plan.source,
    {
        match command {
            Command::Retire => ControlResult::certificate(Certificate::Retired),
            Command::Commit | Command::Abort => {
                if !matches!(receipt.cleanup,Cleanup::None) && !receipt.complete {
                    ControlResult { status: Status::Ok, certificate: None, cleanup: receipt.cleanup }
                } else {
                    ControlResult::certificate(if self.owner == plan.source { Certificate::SourceDone } else { Certificate::DestinationDone })
                }
            }
            _ => ControlResult::status(Status::Ok),
        }
    }

    pub closed spec fn command_seen(&self, generation: u64, command: Command) -> bool {
        self.receipts@.contains_key(generation) && (self.receipts@[generation].applied & command_bit(command)) != 0
    }
    pub open spec fn control_frame(&self, before: Self, plan: MigrationPlan, command: Command) -> bool {
        forall|table: u64,k: Seq<u8>| self.local_meta(table,k) ==
            if crate::bytes::contains_spec(plan.range,table,k) {
                match before.local_meta(table,k) {
                    Some(m) => Some(effect_spec(m,plan.generation,command,before.owner_view() == plan.source)), None => None,
                }
            } else { before.local_meta(table,k) }
    }

    pub fn deliver(&mut self, plan: &MigrationPlan, command: Command) -> (out: ControlResult)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).lease_view() == old(self).lease_view(),
            final(self).owner_view() == old(self).owner_view(),
            out.status != Status::Ok || old(self).command_seen(plan.generation,command) ==> final(self).same_metadata(*old(self)),
            out.status != Status::Ok || old(self).command_seen(plan.generation,command) ==> final(self).local_unchanged(*old(self)),
            out.certificate.is_some() ==> out.status == Status::Ok && out.cleanup == Cleanup::None,
            out.certificate == Some(Certificate::Retired) ==> command == Command::Retire,
            out.certificate == Some(Certificate::SourceDone) ==> (command == Command::Commit || command == Command::Abort) && old(self).owner_view() == plan.source,
            out.certificate == Some(Certificate::DestinationDone) ==> (command == Command::Commit || command == Command::Abort) && old(self).owner_view() == plan.destination,
            out.status == Status::Ok && !old(self).command_seen(plan.generation,command) ==>
                match command {
                    Command::Start | Command::Final => old(self).owner_view() == plan.destination,
                    Command::Freeze | Command::Retire => old(self).owner_view() == plan.source,
                    _ => old(self).owner_view() == plan.source || old(self).owner_view() == plan.destination,
                },
            out.status == Status::Ok && !old(self).command_seen(plan.generation,command) ==>
                final(self).control_frame(*old(self),*plan,command)
                && (forall|k: Seq<u8>| crate::bytes::contains_spec(plan.range,plan.range.table,k) ==>
                    old(self).command_at(*plan,command,old(self).drained_view(plan.range),k)),
    {
        if !self.envelope(plan) { return ControlResult::status(Status::Invalid); }
        let mask = bit(command);
        let mut receipt = match self.receipts.get(&plan.generation) {
            Some(r) => *r,
            None => Receipt { applied: 0, cleanup: Cleanup::None, complete: false, ready: false },
        };
        proof {
            assert(0u8 & mask == 0u8) by(bit_vector);
            assert(receipt.applied & mask != 0 ==> self.command_seen(plan.generation,command));
        }
        if receipt.applied & mask != 0 { return self.result(plan,command,receipt); }
        // A conflicting terminal decision cannot revive an already terminal role.
        if receipt.applied & 48 != 0 { return ControlResult::status(Status::Retry); }
        let drained = if matches!(command,Command::Retire) { self.leases.drained(&plan.range) } else { false };
        if !self.range_guard(plan,command,drained) { return ControlResult::status(Status::Retry); }
        proof {
            crate::bytes::cmp_laws(plan.range.lo@,plan.range.lo@);
            assert(crate::bytes::contains_spec(plan.range,plan.range.table,plan.range.lo@));
            assert(self.command_at(*plan,command,drained,plan.range.lo@));
            assert forall|k: Seq<u8>| crate::bytes::contains_spec(plan.range,plan.range.table,k) implies
                self.command_at(*plan,command,self.drained_view(plan.range),k) by {
                assert(self.command_at(*plan,command,drained,k));
            }
        }
        let ghost before_tables = self.tables@;
        if let Some(mut table) = self.tables.remove(&plan.range.table) {
            table.transform(&plan.range,plan.generation,Some(command),self.owner == plan.source);
            self.tables.insert(plan.range.table,table);
        }
        proof {
            assert forall|t: u64,k: Seq<u8>| self.local_meta(t,k) ==
                if crate::bytes::contains_spec(plan.range,t,k) {
                    match meta_route(before_tables[t].boundaries@,k) {
                        Some(m) => Some(effect_spec(m,plan.generation,command,self.owner == plan.source)), None => None,
                    }
                } else if before_tables.contains_key(t) { meta_route(before_tables[t].boundaries@,k) } else { None } by {
                if t != plan.range.table { assert(self.tables@.contains_key(t) == before_tables.contains_key(t)); }
            }
        }
        receipt.applied = receipt.applied | mask;
        if matches!(command,Command::Commit) && self.owner == plan.source { receipt.cleanup = Cleanup::Source; }
        if matches!(command,Command::Abort) && self.owner == plan.destination { receipt.cleanup = Cleanup::Destination; }
        self.receipts.insert(plan.generation,receipt);
        self.result(plan,command,receipt)
    }

    fn all_role(&self, plan: &MigrationPlan, role: Role, round: Option<u64>, terminal: bool) -> (yes: bool)
        ensures yes && self.table_wf(plan.range.table) ==> forall|k: Seq<u8>|
            crate::bytes::contains_spec(plan.range,plan.range.table,k) ==>
                self.local_meta(plan.range.table,k).is_some()
                && role_spec(self.local_meta(plan.range.table,k).unwrap(),plan.generation,role,round,terminal),
    {
        let table = match self.tables.get(&plan.range.table) { Some(t) => t, None => return false };
        let at_lo = match table.lookup(&plan.range.lo) { Some(m) => m, None => return false };
        if !role_guard(at_lo,plan.generation,role,round,terminal) { return false; }
        let mut i = 0usize;
        while i < table.boundaries.len()
            invariant i <= table.boundaries.len(),
                meta_wf(table.boundaries@) ==> meta_route(table.boundaries@,plan.range.lo@).is_some()
                    && role_spec(meta_route(table.boundaries@,plan.range.lo@).unwrap(),plan.generation,role,round,terminal),
                forall|j: int| 0 <= j < i && crate::bytes::contains_spec(plan.range,plan.range.table,table.boundaries@[j].start@) ==>
                    role_spec(table.boundaries@[j].meta,plan.generation,role,round,terminal),
            decreases table.boundaries.len() - i,
        {
            let b = &table.boundaries[i];
            if contains(&plan.range,plan.range.table,&b.start) && !role_guard(b.meta,plan.generation,role,round,terminal) { return false; }
            i += 1;
        }
        proof {
            if meta_wf(table.boundaries@) {
                uniform_meta(table.boundaries@,plan.range,|m: ReplicaMeta| role_spec(m,plan.generation,role,round,terminal));
            }
        }
        true
    }

    pub fn drain(&self, plan: &MigrationPlan) -> (out: ControlResult)
        ensures out.certificate == Some(Certificate::Drained) ==> self.drained_view(plan.range)
            && self.owner_view() == plan.source
            && (self.table_wf(plan.range.table) ==> forall|k: Seq<u8>| crate::bytes::contains_spec(plan.range,plan.range.table,k) ==>
                self.local_meta(plan.range.table,k).is_some()
                && role_spec(self.local_meta(plan.range.table,k).unwrap(),plan.generation,Role::Frozen,None,false)),
            out.certificate == Some(Certificate::Drained) ==> out.status == Status::Ok && out.cleanup == Cleanup::None,
            out.certificate == Some(Certificate::Drained) && self.wf() ==> self.capture_authorized(*plan),
    {
        if !self.envelope(plan) || self.owner != plan.source { return ControlResult::status(Status::Invalid); }
        if !self.all_role(plan,Role::Frozen,None,false) || !self.leases.drained(&plan.range) {
            return ControlResult::status(Status::Retry);
        }
        ControlResult::certificate(Certificate::Drained)
    }

    pub open spec fn range_role(&self, plan: MigrationPlan, role: Role, round: Option<u64>, terminal: bool) -> bool {
        forall|k: Seq<u8>| crate::bytes::contains_spec(plan.range,plan.range.table,k) ==>
            self.local_meta(plan.range.table,k).is_some()
            && role_spec(self.local_meta(plan.range.table,k).unwrap(),plan.generation,role,round,terminal)
    }
    pub closed spec fn cleanup_pending(&self, plan: MigrationPlan) -> bool {
        self.receipts@.contains_key(plan.generation)
            && !self.receipts@[plan.generation].complete
            && ((self.owner == plan.source && self.receipts@[plan.generation].cleanup == Cleanup::Source)
                || (self.owner == plan.destination && self.receipts@[plan.generation].cleanup == Cleanup::Destination))
    }
    pub open spec fn transfer_authorized(&self, plan: MigrationPlan, cleanup: bool) -> bool {
        if cleanup { self.cleanup_pending(plan) && self.range_role(plan,Role::Empty,None,true) }
        else { self.owner_view() == plan.destination && !self.ready_seen(plan.generation)
            && self.range_role(plan,Role::Stage,Some(1),false) }
    }
    /// Called once while constructing the checked adapter. Its immutable borrow
    /// is retained through every engine effect, not exported as an unlocked flag.
    pub(crate) fn authorize_transfer(&self, plan: &MigrationPlan, cleanup: bool) -> (status: Status)
        requires self.wf(),
        ensures status == Status::Ok ==> self.transfer_authorized(*plan,cleanup),
    {
        if !self.envelope(plan) { return Status::Invalid; }
        if cleanup {
            let receipt = match self.receipts.get(&plan.generation) { Some(r) => r, None => return Status::Retry };
            if receipt.complete { return Status::Retry; }
            if !((self.owner == plan.source && matches!(receipt.cleanup,Cleanup::Source))
                || (self.owner == plan.destination && matches!(receipt.cleanup,Cleanup::Destination))) {
                return Status::Retry;
            }
            if !self.all_role(plan,Role::Empty,None,true) { return Status::Retry; }
        } else {
            if self.owner != plan.destination { return Status::Invalid; }
            if let Some(receipt) = self.receipts.get(&plan.generation) {
                if receipt.ready { return Status::Retry; }
            }
            if !self.all_role(plan,Role::Stage,Some(1),false) { return Status::Retry; }
        }
        Status::Ok
    }
    pub open spec fn capture_authorized(&self, plan: MigrationPlan) -> bool {
        self.owner_view() == plan.source && self.drained_view(plan.range)
            && self.range_role(plan,Role::Frozen,None,false)
    }
    pub(crate) fn authorize_capture(&self, plan: &MigrationPlan) -> (status: Status)
        requires self.wf(),
        ensures status == Status::Ok ==> self.capture_authorized(*plan),
    {
        let result = self.drain(plan);
        match result.certificate { Some(Certificate::Drained) => Status::Ok, _ => Status::Retry }
    }

    pub closed spec fn ready_seen(&self, generation: u64) -> bool {
        self.receipts@.contains_key(generation) && self.receipts@[generation].ready
    }
    pub closed spec fn cleanup_done(&self, generation: u64) -> bool {
        self.receipts@.contains_key(generation) && self.receipts@[generation].cleanup != Cleanup::None
            && self.receipts@[generation].complete
    }
    pub open spec fn local_unchanged(&self, before: Self) -> bool {
        self.owner_view() == before.owner_view()
            && forall|table: u64,k: Seq<u8>| self.local_meta(table,k) == before.local_meta(table,k)
    }
    pub open spec fn seal_frame(&self, before: Self, plan: MigrationPlan) -> bool {
        forall|table: u64,k: Seq<u8>| self.local_meta(table,k) ==
            if crate::bytes::contains_spec(plan.range,table,k) {
                match before.local_meta(table,k) { Some(m) => Some(ReplicaMeta { role: Role::Ready, ..m }), None => None }
            } else { before.local_meta(table,k) }
    }

    /// The capability is only minted by the driver's exact final mirror, after
    /// source drain and all puts AND destination-only deletes have completed.
    pub(crate) fn seal(&mut self, plan: &MigrationPlan, completed: &CompletedCopy) -> (out: ControlResult)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).lease_view() == old(self).lease_view(),
            final(self).owner_view() == old(self).owner_view(),
            out.status != Status::Ok ==> out.certificate == None,
            out.status != Status::Ok || old(self).ready_seen(plan.generation) ==> final(self).same_metadata(*old(self)),
            out.status != Status::Ok ==> final(self).local_unchanged(*old(self)),
            out.status == Status::Ok ==> out.certificate == Some(Certificate::Ready)
                && completed.matches_spec(*plan,old(self).owner_view()),
            out.status == Status::Ok && !old(self).ready_seen(plan.generation) ==>
                final(self).seal_frame(*old(self),*plan)
                && (forall|k: Seq<u8>| crate::bytes::contains_spec(plan.range,plan.range.table,k) ==>
                    old(self).local_meta(plan.range.table,k).is_some()
                    && role_spec(old(self).local_meta(plan.range.table,k).unwrap(),plan.generation,Role::Stage,Some(1),false)),
    {
        if !self.envelope(plan) || self.owner != plan.destination || !completed.matches(plan,self.owner) {
            return ControlResult::status(Status::Invalid);
        }
        let mut receipt = match self.receipts.get(&plan.generation) { Some(r) => *r, None => return ControlResult::status(Status::Retry) };
        if receipt.ready { return ControlResult::certificate(Certificate::Ready); }
        if !self.all_role(plan,Role::Stage,Some(1),false) { return ControlResult::status(Status::Retry); }
        let ghost before_tables = self.tables@;
        if let Some(mut table) = self.tables.remove(&plan.range.table) {
            table.transform(&plan.range,plan.generation,None,false);
            self.tables.insert(plan.range.table,table);
        }
        proof {
            assert forall|t: u64,k: Seq<u8>| self.local_meta(t,k) ==
                if crate::bytes::contains_spec(plan.range,t,k) {
                    match meta_route(before_tables[t].boundaries@,k) {
                        Some(m) => Some(ReplicaMeta { role: Role::Ready, ..m }), None => None,
                    }
                } else if before_tables.contains_key(t) { meta_route(before_tables[t].boundaries@,k) } else { None } by {
                if t != plan.range.table { assert(self.tables@.contains_key(t) == before_tables.contains_key(t)); }
            }
        }
        receipt.ready = true;
        self.receipts.insert(plan.generation,receipt);
        ControlResult::certificate(Certificate::Ready)
    }

    pub(crate) fn complete_cleanup(&mut self, plan: &MigrationPlan, completed: CompletedCleanup) -> (out: ControlResult)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).lease_view() == old(self).lease_view(), final(self).local_unchanged(*old(self)),
            out.status != Status::Ok ==> final(self).same_metadata(*old(self)),
            out.status != Status::Ok ==> out.certificate == None,
            out.status == Status::Ok ==> completed.matches_spec(*plan,old(self).owner_view())
                && final(self).cleanup_done(plan.generation)
                && out.certificate == Some(if old(self).owner_view() == plan.source { Certificate::SourceDone } else { Certificate::DestinationDone }),
            out.status == Status::Ok && !old(self).cleanup_done(plan.generation) ==>
                forall|k: Seq<u8>| crate::bytes::contains_spec(plan.range,plan.range.table,k) ==>
                    old(self).local_meta(plan.range.table,k).is_some()
                    && role_spec(old(self).local_meta(plan.range.table,k).unwrap(),plan.generation,Role::Empty,None,true),
    {
        if !self.envelope(plan) || !completed.matches(plan,self.owner) { return ControlResult::status(Status::Invalid); }
        let mut receipt = match self.receipts.get(&plan.generation) { Some(r) => *r, None => return ControlResult::status(Status::Retry) };
        if matches!(receipt.cleanup,Cleanup::None) { return ControlResult::status(Status::Invalid); }
        if !receipt.complete && !self.all_role(plan,Role::Empty,None,true) { return ControlResult::status(Status::Retry); }
        receipt.complete = true;
        self.receipts.insert(plan.generation,receipt);
        ControlResult::certificate(if self.owner == plan.source { Certificate::SourceDone } else { Certificate::DestinationDone })
    }
}

pub open spec fn role_spec(m: ReplicaMeta, generation: u64, role: Role, round: Option<u64>, terminal: bool) -> bool {
    m.role == role && m.fence == generation && m.terminal == terminal
        && match round { Some(r) => m.round == r, None => true }
}
fn role_guard(m: ReplicaMeta, generation: u64, role: Role, round: Option<u64>, terminal: bool) -> (yes: bool)
    ensures yes == role_spec(m,generation,role,round,terminal),
{
    role_equal(m.role,role) && m.fence == generation && m.terminal == terminal
        && match round { Some(r) => m.round == r, None => true }
}
} // verus!
