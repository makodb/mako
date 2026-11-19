//! Executable transfer composition. The caller retains the participant borrow
//! through every physical effect and completion-capability consumption.
//! Only RawStore is a native engine boundary; it has no metadata/handler oracle.
//! Native Frozen scans additionally require the caller's completed STO cut;
//! the conditional durable capture API below retains its separate lease guard.
use vstd::prelude::*;
use crate::types::*;
use crate::participant::{Participant, ControlResult, Cleanup};
use crate::storage::{self, Source, Store};
#[cfg(verus_keep_ghost)]
use crate::storage::{Image, first, identity, in_range, row_cell};
#[cfg(all(test, not(verus_keep_ghost)))]
#[path = "transfer_tests.rs"]
mod tests;

verus! {
/// Erased borrow scope. None permits any window; Some binds a durable adapter
/// to the range whose physical byte/version frames it retains.
pub open spec fn scope_allows(scope:Option<KeyRange>,range:KeyRange)->bool {
    match scope {
        None=>true,
        Some(window)=>window.table==range.table && window.lo@==range.lo@
            && match (window.hi,range.hi) { (None,None)=>true,(Some(a),Some(b))=>a@==b@,_=>false },
    }
}
/// A selected-range engine projection, not a global snapshot. The native host
/// guarantees exact ordered scan and atomic successful one-key effects, with
/// errors leaving the window unchanged. Each physical effect additionally
/// frames every other physical key; concurrent outside-window commits need not
/// stop. Admission + exact selected-range drain + nonreplicated migration are
/// the environmental premises which make this borrowed window exclusive.
pub trait RawStore {
    type Context;
    #[verifier::prophetic]
    spec fn context(&self) -> Self::Context;
    spec fn image(&self, range: KeyRange) -> Image;
    spec fn scope(&self) -> Option<KeyRange>;
    spec fn wf(&self) -> bool;
    /// Proof-only stabilized I/O boundary; every primitive succeeds while this
    /// borrowed range remains available. Production exposes no stability claim.
    spec fn available(&self,range: KeyRange) -> bool;
    fn scan(&self, range: &KeyRange, after: Option<&Row>) -> (result: Result<Option<Row>,Status>)
        requires self.wf(),scope_allows(self.scope(),*range),
        ensures result is Ok ==> first(self.image(*range),*range,
            match after { None => None, Some(r) => Some(identity(*r)) },result->Ok_0),
            self.available(*range) ==> result is Ok;
    fn put(&mut self, range: &KeyRange, row: &Row) -> (status: Status)
        requires old(self).wf(),scope_allows(old(self).scope(),*range),in_range(*range,row_cell(range.table,*row)),
        ensures final(self).wf(),final(self).scope()==old(self).scope(),
            final(self).context()==old(self).context(),
            final(self).available(*range) == old(self).available(*range),
            old(self).available(*range) ==> status == Status::Ok,
            status == Status::Ok ==> final(self).image(*range)
                == old(self).image(*range).insert(row_cell(range.table,*row),row.value@),
            status != Status::Ok ==> final(self).image(*range) == old(self).image(*range);
    fn delete(&mut self, range: &KeyRange, row: &Row) -> (status: Status)
        requires old(self).wf(),scope_allows(old(self).scope(),*range),in_range(*range,row_cell(range.table,*row)),
        ensures final(self).wf(),final(self).scope()==old(self).scope(),
            final(self).context()==old(self).context(),
            final(self).available(*range) == old(self).available(*range),
            old(self).available(*range) ==> status == Status::Ok,
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
    type Context = (&'a Participant, &'a MigrationPlan, bool, K, K::Context, Option<KeyRange>);
    #[verifier::prophetic]
    closed spec fn context(&self) -> Self::Context { (self.participant,self.plan,self.cleanup,*final(self.raw),self.raw.context(),self.raw.scope()) }
    closed spec fn image(&self) -> Image { self.raw.image(self.plan.range) }
    closed spec fn wf(&self) -> bool {
        self.raw.wf() && self.participant.wf()
            && scope_allows(self.raw.scope(),self.plan.range)
            && self.participant.transfer_authorized(*self.plan,self.cleanup)
    }
    closed spec fn authorized(&self,plan: MigrationPlan,owner: u32,cleanup: bool) -> bool {
        storage::same_plan(*self.plan,plan) && owner == self.participant.owner_view()
            && cleanup == self.cleanup && self.participant.transfer_authorized(*self.plan,cleanup)
    }
    closed spec fn available(&self,plan: MigrationPlan) -> bool {
        storage::same_plan(*self.plan,plan) && self.raw.available(self.plan.range)
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
    requires node.wf(), store.wf(),scope_allows(store.scope(),plan.range),
    ensures result is Ok ==> node.capture_authorized(*plan)
        && first(store.image(plan.range),plan.range,
            match after { None => None, Some(r) => Some(identity(*r)) },result->Ok_0),
        node.envelope_view(*plan) && node.capture_authorized(*plan)
            && store.available(plan.range) ==> result is Ok,
{
    let status = node.authorize_capture(plan);
    match status { Status::Ok => {}, _ => return Err(status) }
    store.scan(&plan.range,after)
}
/// Native metadata authority is independent of the conditional lease book.
pub open spec fn frozen_capture_authorized(node: Participant,plan: MigrationPlan) -> bool {
    node.envelope_view(plan) && node.owner_view() == plan.source
        && node.range_role(plan,Role::Frozen,None,false)
}

/// Exact frozen-source scan under an immutable, full-scope metadata borrow.
/// The production caller MUST first complete its retained generation-bound STO
/// cut and keep Frozen metadata unchanged through this call. That real engine
/// exclusion supplies RawStore's stable exact-image contract; this wrapper
/// neither inspects lease emptiness nor establishes engine drainage by itself.
/// Concurrent live precopy scans must not use this exact frozen-image API.
pub fn capture_frozen<K: RawStore>(node: &Participant,store: &K,plan: &MigrationPlan,after: Option<&Row>)
    -> (result: Result<Option<Row>,Status>)
    requires node.wf(),store.wf(),scope_allows(store.scope(),plan.range),
    ensures result is Ok ==> frozen_capture_authorized(*node,*plan)
        && first(store.image(plan.range),plan.range,
            match after { None => None, Some(r) => Some(identity(*r)) },result->Ok_0),
        frozen_capture_authorized(*node,*plan) && store.available(plan.range) ==> result is Ok,
{
    if !node.frozen_source(plan) { return Err(Status::Retry); }
    store.scan(&plan.range,after)
}

pub open spec fn precopy_authorized(node: Participant,plan: MigrationPlan) -> bool {
    node.envelope_view(plan) && node.owner_view() == plan.destination
        && node.range_role(plan,Role::Stage,Some(0),false)
}

/// Advisory round-zero write. The immutable participant borrow covers the
/// complete plan check and raw effect, so Final/Abort cannot race between them.
/// This returns only a status: no coverage, CompletedCopy, or Ready is minted.
/// The single private destination job supplies exclusive access to its Stage
/// window; it must finish before Final/Abort can mutate this metadata.
pub fn precopy_put<K: RawStore>(node: &Participant,store: &mut K,plan: &MigrationPlan,row: &Row)
    -> (status: Status)
    requires node.wf(),old(store).wf(),scope_allows(old(store).scope(),plan.range),
    ensures final(store).wf(),final(store).scope() == old(store).scope(),
        final(store).context() == old(store).context(),
        final(store).available(plan.range) == old(store).available(plan.range),
        status == Status::Ok ==> precopy_authorized(*node,*plan)
            && in_range(plan.range,row_cell(plan.range.table,*row))
            && final(store).image(plan.range) == old(store).image(plan.range).insert(row_cell(plan.range.table,*row),row.value@),
        status != Status::Ok ==> final(store).image(plan.range) == old(store).image(plan.range),
        storage::preserves_outside(final(store).image(plan.range),old(store).image(plan.range),plan.range),
        precopy_authorized(*node,*plan) && in_range(plan.range,row_cell(plan.range.table,*row))
            && old(store).available(plan.range) ==> status == Status::Ok,
{
    if !node.background_destination(plan) { return Status::Retry; }
    if !crate::bytes::contains(&plan.range,plan.range.table,&row.coordinate) { return Status::Invalid; }
    let ghost before = store.image(plan.range);
    let status = store.put(&plan.range,row);
    proof {
        assert forall|k: storage::Cell| !in_range(plan.range,k) implies
            storage::value(store.image(plan.range),k) == storage::value(before,k) by {
            assert(k != row_cell(plan.range.table,*row));
        }
    }
    status
}

/// A concrete selected coordinate witnesses refusal; a check of only the first
/// interval could not establish this fact for a later changed interval.
pub proof fn precopy_fenced_at(node: Participant,plan: MigrationPlan,key: Seq<u8>)
    requires crate::bytes::contains_spec(plan.range,plan.range.table,key),
        match node.local_meta(plan.range.table,key) {
            None => true,
            Some(m) => m.role != Role::Stage || m.fence != plan.generation || m.round != 0 || m.terminal,
        },
    ensures !precopy_authorized(node,plan),
{}

/// The actual control-frame postcondition of successful first Final/Abort
/// delivery makes every old round-zero packet in that range ineligible.
pub proof fn precopy_after_final_or_abort(before: Participant,after: Participant,
    plan: MigrationPlan,command: Command,key: Seq<u8>)
    requires after.control_frame(before,plan,command),
        command == Command::Final || command == Command::Abort,
        crate::bytes::contains_spec(plan.range,plan.range.table,key),
    ensures !precopy_authorized(after,plan),
{
    assert(after.local_meta(plan.range.table,key) == match before.local_meta(plan.range.table,key) {
        Some(m) => Some(crate::participant::effect_spec(m,plan.generation,command,before.owner_view() == plan.source)),
        None => None,
    });
    precopy_fenced_at(after,plan,key);
}

/// A newly started generation may have the same destination and round zero;
/// its installed generation fence still excludes the old job's physical write.
pub proof fn precopy_old_generation(node: Participant,old_plan: MigrationPlan,
    new_plan: MigrationPlan,key: Seq<u8>)
    requires old_plan.generation != new_plan.generation,
        crate::bytes::contains_spec(old_plan.range,old_plan.range.table,key),
        old_plan.range.table == new_plan.range.table,
        crate::bytes::contains_spec(new_plan.range,new_plan.range.table,key),
        node.range_role(new_plan,Role::Stage,Some(0),false),
    ensures !precopy_authorized(node,old_plan),
{
    assert(node.local_meta(old_plan.range.table,key).unwrap().fence == new_plan.generation);
    precopy_fenced_at(node,old_plan,key);
}


/// Final command delivery precedes this call. Native effects and Ready seal are
/// inseparable under the caller's one mutable participant borrow/mutex.
pub fn execute_final<K: RawStore,S: Source>(node: &mut Participant,store: &mut K,
    plan: &MigrationPlan,source: &S) -> (result: ControlResult)
    requires old(node).wf(),old(store).wf(),scope_allows(old(store).scope(),plan.range),source.wf(),
    ensures final(node).wf(),final(store).wf(),final(store).scope()==old(store).scope(),
        final(store).context()==old(store).context(),
        final(node).lease_view() == old(node).lease_view(),
        final(node).owner_view() == old(node).owner_view(),
        final(node).table_ids()==old(node).table_ids(),
        result.cleanup==crate::participant::Cleanup::None,
        result.certificate.is_some() ==> final(node).receipt_view()=={
            let r=old(node).receipt_view()[plan.generation];
            old(node).receipt_view().insert(plan.generation,(r.0,r.1,r.2,true))
        },
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
        old(node).envelope_view(*plan) && old(node).transfer_authorized(*plan,false)
            && old(node).has_receipt(plan.generation) && old(store).available(plan.range) && source.available(*plan) ==>
            result.status == Status::Ok && result.certificate == Some(Certificate::Ready),
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
    requires old(node).wf(),old(store).wf(),scope_allows(old(store).scope(),plan.range),
    ensures final(node).wf(),final(store).wf(),final(store).scope()==old(store).scope(),
        final(store).context()==old(store).context(),
        final(node).lease_view() == old(node).lease_view(), final(node).local_unchanged(*old(node)),
        final(node).table_ids()==old(node).table_ids(),
        result.cleanup==crate::participant::Cleanup::None,
        result.certificate.is_some() ==> final(node).receipt_view()=={
            let r=old(node).receipt_view()[plan.generation];
            old(node).receipt_view().insert(plan.generation,(r.0,r.1,true,r.3))
        },
        result.certificate.is_some() ==> result.status == Status::Ok
            && result.certificate == Some(if old(node).owner_view() == plan.source { Certificate::SourceDone } else { Certificate::DestinationDone })
            && final(node).cleanup_done(plan.generation)
            && old(node).transfer_authorized(*plan,true)
            && storage::complete(final(store).image(plan.range),old(store).image(plan.range),Map::empty(),plan.range),
        storage::preserves_outside(final(store).image(plan.range),old(store).image(plan.range),plan.range),
        storage::prefix(final(store).image(plan.range),old(store).image(plan.range),Map::empty(),plan.range),
        result.certificate.is_none() ==> final(node).same_metadata(*old(node)),
        old(node).envelope_view(*plan) && old(node).transfer_authorized(*plan,true)
            && old(store).available(plan.range) ==>
            result.status == Status::Ok
                && result.certificate == Some(if old(node).owner_view() == plan.source { Certificate::SourceDone } else { Certificate::DestinationDone }),
{
    proof {
        node.metadata_reflexive();
        storage::prefix_at(store.image(plan.range),store.image(plan.range),Map::empty(),plan.range,None);
    }
    let status = node.authorize_transfer(plan,true);
    match status { Status::Ok => {}, _ => return rejected(status) }
    proof { node.pending_cleanup_is_completable(*plan); }
    let owner = node.owner();
    let completed = {
        let mut guarded = GuardedStore { participant: &*node, raw: &mut *store, plan, cleanup: true };
        proof {
            assert(guarded.authorized(*plan,owner,true));
            assert(owner == plan.source || owner == plan.destination);
        }
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
