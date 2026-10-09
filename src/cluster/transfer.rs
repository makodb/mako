//! Executable transfer composition. The caller retains its participant/storage
//! mutex through this entire call, including completion-capability consumption.
//! Only RawStore is a native engine boundary; it has no metadata/handler oracle.
use vstd::prelude::*;
use crate::types::*;
use crate::participant::{Participant, ControlResult, Cleanup};
use crate::storage::{self, Source, Store};
#[cfg(verus_keep_ghost)]
use crate::storage::{Image, first, identity, in_range, row_cell};

verus! {
/// A selected-range engine projection, not a global snapshot. The native host
/// guarantees exact ordered scan and atomic successful one-key effects, with
/// errors leaving the window unchanged. Each physical effect additionally
/// frames every other physical key; concurrent outside-window commits need not
/// stop. Admission + exact selected-range drain + nonreplicated migration are
/// the environmental premises which make this borrowed window exclusive.
pub trait RawStore {
    spec fn image(&self, range: KeyRange) -> Image;
    spec fn wf(&self) -> bool;
    fn scan(&self, range: &KeyRange, after: Option<&Row>) -> (result: Result<Option<Row>,Status>)
        requires self.wf(),
        ensures result is Ok ==> first(self.image(*range),*range,
            match after { None => None, Some(r) => Some(identity(*r)) },result->Ok_0);
    fn put(&mut self, range: &KeyRange, row: &Row) -> (status: Status)
        requires old(self).wf(), in_range(*range,row_cell(range.table,*row)),
        ensures final(self).wf(),
            status == Status::Ok ==> final(self).image(*range)
                == old(self).image(*range).insert(row_cell(range.table,*row),row.value@),
            status != Status::Ok ==> final(self).image(*range) == old(self).image(*range);
    fn delete(&mut self, range: &KeyRange, row: &Row) -> (status: Status)
        requires old(self).wf(), in_range(*range,row_cell(range.table,*row)),
        ensures final(self).wf(),
            status == Status::Ok ==> final(self).image(*range)
                == old(self).image(*range).remove(row_cell(range.table,*row)),
            status != Status::Ok ==> final(self).image(*range) == old(self).image(*range);
}

/// Private: no caller can fabricate authorization or detach the participant
/// borrow before performing a raw effect. The entire range was checked once.
struct GuardedStore<'a, K: RawStore> {
    participant: &'a Participant,
    raw: &'a mut K,
    plan: &'a MigrationPlan,
    cleanup: bool,
}
impl<'a,K: RawStore> Store for GuardedStore<'a,K> {
    type Context = (&'a Participant, &'a MigrationPlan, bool, K);
    #[verifier::prophetic]
    closed spec fn context(&self) -> Self::Context { (self.participant,self.plan,self.cleanup,*final(self.raw)) }
    closed spec fn image(&self) -> Image { self.raw.image(self.plan.range) }
    closed spec fn wf(&self) -> bool {
        self.raw.wf() && self.participant.wf()
            && self.participant.transfer_authorized(*self.plan,self.cleanup)
    }
    closed spec fn authorized(&self,plan: MigrationPlan,owner: u32,cleanup: bool) -> bool {
        storage::same_plan(*self.plan,plan) && owner == self.participant.owner_view()
            && cleanup == self.cleanup && self.participant.transfer_authorized(*self.plan,cleanup)
    }
    fn validate(&self,plan: &MigrationPlan,owner: u32,cleanup: bool) -> (status: Status) {
        if owner != self.participant.owner() || cleanup != self.cleanup
            || !storage::plans_match(self.plan,plan) { Status::Retry } else { Status::Ok }
    }
    fn scan(&self,plan: &MigrationPlan,after: Option<&Row>) -> (result: Result<Option<Row>,Status>) {
        if !storage::plans_match(self.plan,plan) { return Err(Status::Retry); }
        let result = self.raw.scan(&self.plan.range,after);
        proof {
            if result is Ok {
                assert forall|k: storage::Cell| storage::in_range(plan.range,k) ==
                    storage::in_range(self.plan.range,k) by {}
            }
        }
        result
    }
    fn put(&mut self,plan: &MigrationPlan,owner: u32,row: &Row) -> (status: Status) {
        self.raw.put(&self.plan.range,row)
    }
    fn delete(&mut self,plan: &MigrationPlan,owner: u32,row: &Row,cleanup: bool) -> (status: Status) {
        self.raw.delete(&self.plan.range,row)
    }
}

fn rejected(status: Status) -> (result: ControlResult)
    ensures result.status == status, result.certificate == None, result.cleanup == Cleanup::None,
{ ControlResult { status, certificate: None, cleanup: Cleanup::None } }

/// Authenticate local source state before native I/O, under the same immutable
/// participant borrow. EOF is a proof of all remaining absences, never an error.
pub fn capture<K: RawStore>(node: &Participant,store: &K,plan: &MigrationPlan,after: Option<&Row>)
    -> (result: Result<Option<Row>,Status>)
    requires node.wf(), store.wf(),
    ensures result is Ok ==> node.capture_authorized(*plan)
        && first(store.image(plan.range),plan.range,
            match after { None => None, Some(r) => Some(identity(*r)) },result->Ok_0),
{
    let status = node.authorize_capture(plan);
    match status { Status::Ok => {}, _ => return Err(status) }
    store.scan(&plan.range,after)
}

/// Final command delivery precedes this call. Native effects and Ready seal are
/// inseparable under the caller's one mutable participant borrow/mutex.
pub fn execute_final<K: RawStore,S: Source>(node: &mut Participant,store: &mut K,
    plan: &MigrationPlan,source: &S) -> (result: ControlResult)
    requires old(node).wf(), old(store).wf(), source.wf(),
    ensures final(node).wf(), final(store).wf(),
        final(node).lease_view() == old(node).lease_view(),
        final(node).owner_view() == old(node).owner_view(),
        result.certificate.is_some() ==> result.certificate == Some(Certificate::Ready)
            && result.status == Status::Ok
            && storage::complete(final(store).image(plan.range),old(store).image(plan.range),source.image(),plan.range)
            && old(node).transfer_authorized(*plan,false)
            && final(node).seal_frame(*old(node),*plan),
        storage::preserves_outside(final(store).image(plan.range),old(store).image(plan.range),plan.range),
        storage::prefix(final(store).image(plan.range),old(store).image(plan.range),source.image(),plan.range),
        result.certificate.is_none() ==> final(node).local_unchanged(*old(node)),
        result.certificate.is_none() ==> final(node).same_metadata(*old(node)),
        final(store).image(plan.range) != old(store).image(plan.range) ==> old(node).transfer_authorized(*plan,false),
{
    proof {
        node.metadata_reflexive();
        storage::prefix_at(store.image(plan.range),store.image(plan.range),source.image(),plan.range,None);
    }
    let status = node.authorize_transfer(plan,false);
    match status { Status::Ok => {}, _ => return rejected(status) }
    let owner = node.owner();
    let completed = {
        let mut guarded = GuardedStore { participant: &*node, raw: &mut *store, plan, cleanup: false };
        let completed = storage::mirror(plan,owner,&mut guarded,source);
        let raw = guarded.raw;
        proof { assert(raw.wf()); }
        completed
    };
    match completed {
        Err(status) => rejected(status),
        Ok(completed) => node.seal(plan,&completed),
    }
}

/// Terminal command delivery precedes cleanup. No completion receipt escapes
/// until every selected byte is absent and the private capability is consumed.
pub fn execute_cleanup<K: RawStore>(node: &mut Participant,store: &mut K,plan: &MigrationPlan)
    -> (result: ControlResult)
    requires old(node).wf(), old(store).wf(),
    ensures final(node).wf(), final(store).wf(),
        final(node).lease_view() == old(node).lease_view(), final(node).local_unchanged(*old(node)),
        result.certificate.is_some() ==> result.status == Status::Ok
            && result.certificate == Some(if old(node).owner_view() == plan.source { Certificate::SourceDone } else { Certificate::DestinationDone })
            && final(node).cleanup_done(plan.generation)
            && old(node).transfer_authorized(*plan,true)
            && storage::complete(final(store).image(plan.range),old(store).image(plan.range),Map::empty(),plan.range),
        storage::preserves_outside(final(store).image(plan.range),old(store).image(plan.range),plan.range),
        storage::prefix(final(store).image(plan.range),old(store).image(plan.range),Map::empty(),plan.range),
        result.certificate.is_none() ==> final(node).same_metadata(*old(node)),
{
    proof {
        node.metadata_reflexive();
        storage::prefix_at(store.image(plan.range),store.image(plan.range),Map::empty(),plan.range,None);
    }
    let status = node.authorize_transfer(plan,true);
    match status { Status::Ok => {}, _ => return rejected(status) }
    let owner = node.owner();
    let completed = {
        let mut guarded = GuardedStore { participant: &*node, raw: &mut *store, plan, cleanup: true };
        let completed = storage::cleanup(plan,owner,&mut guarded);
        let raw = guarded.raw;
        proof { assert(raw.wf()); }
        completed
    };
    match completed {
        Err(status) => rejected(status),
        Ok(completed) => node.complete_cleanup(plan,completed),
    }
}
} // verus!
