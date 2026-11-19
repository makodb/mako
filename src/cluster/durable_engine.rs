//! Explicit raw durable-row/transaction boundary, NOT an MBTA implementation.
//! Canonical identity is (table, coordinate, raw key); versions survive deletion.
//! The exclusive native adapter serializes admission checks with application.
//! Raft adapters must perform the same check at application, not only enqueue.
use vstd::prelude::*;
use crate::types::{KeyRange,TxnId,Status,Row};
use crate::transfer::RawStore;
use crate::durable_kv::{Admission,CommitResult};
#[cfg(verus_keep_ghost)]
use crate::storage::{self,Image,Cell,first,identity,in_range,row_cell};
verus! {
/// Atomic byte/version effect accepted by the engine, not a migration command.
#[cfg(verus_keep_ghost)]
pub struct RowEffect {pub cell:Cell,pub value:Option<Seq<u8>>,pub version:u64}
#[cfg(verus_keep_ghost)]
pub struct RowState {pub bytes:Image,pub versions:Map<Cell,u64>}
pub open spec fn row_apply(s:RowState,e:RowEffect)->RowState {
    RowState{bytes:match e.value{Some(v)=>s.bytes.insert(e.cell,v),None=>s.bytes.remove(e.cell)},
        versions:s.versions.insert(e.cell,e.version)}
}
pub open spec fn batch_apply(s:RowState,writes:Seq<RowEffect>)->RowState
    decreases writes.len(),
{if writes.len()==0{s}else{batch_apply(row_apply(s,writes[0]),writes.skip(1))}}
pub open spec fn settled_rows(before:RowState,pending:Option<RowEffect>,after:RowState)->bool {
    after==before || match pending{None=>false,Some(e)=>after==row_apply(before,e)}
}
pub open spec fn next_version(s:RowState,k:Cell)->u64 {if s.versions.contains_key(k){(s.versions[k]+1) as u64}else{1}}
pub open spec fn version_available(s:RowState,k:Cell)->bool {!s.versions.contains_key(k)||s.versions[k]<u64::MAX}
#[derive(Clone,Copy,PartialEq,Eq)]
pub enum TxnDecision{Undecided,Commit,Abort}
#[cfg(verus_keep_ghost)]
pub struct TxnRecord{pub writes:Seq<RowEffect>,pub decision:TxnDecision,pub settled:bool}
pub open spec fn settled(records:Map<TxnId,TxnRecord>,id:TxnId)->bool {
    records.contains_key(id) && records[id].settled
}
pub open spec fn versions_outside(after:Map<Cell,u64>,before:Map<Cell,u64>,range:KeyRange)->bool {
    forall|k:Cell| !in_range(range,k) ==> after.contains_key(k)==before.contains_key(k)
        && (before.contains_key(k) ==> after[k]==before[k])
}
pub open spec fn txn_effect(before:RowState,record:TxnRecord,after:RowState,terminal:TxnRecord)->bool {
    terminal.writes==record.writes && terminal.settled
        && if record.settled{terminal==record && after==before}else{
            (record.decision==TxnDecision::Undecided || terminal.decision==record.decision)
                && match terminal.decision{
                    TxnDecision::Commit=>after==batch_apply(before,record.writes),
                    TxnDecision::Abort=>after==before,TxnDecision::Undecided=>false,
                }
        }
}
/// Implementations supply real engine guarantees. None mentions a migration
/// phase, Ready, generation, copy completion, or participant correctness.
pub trait DurableEngine:Sized {
    spec fn wf(&self)->bool;
    spec fn owner(&self)->u32;
    spec fn durable_state(&self)->RowState;
    spec fn working_state(&self)->RowState;
    spec fn pending_row(&self)->Option<RowEffect>;
    spec fn transactions(&self)->Map<TxnId,TxnRecord>;
    spec fn installed(&self)->Option<Admission>;
    spec fn recovered(&self)->bool;

    fn recover_rows(&mut self)->(s:Status)
        requires old(self).wf(),
        ensures final(self).wf(),final(self).owner()==old(self).owner(),
            settled_rows(old(self).durable_state(),old(self).pending_row(),final(self).durable_state()),
            final(self).transactions()==old(self).transactions(),
            s==Status::Ok ==> final(self).recovered() && final(self).installed()==None && final(self).pending_row()==None
                && final(self).working_state()==final(self).durable_state(),
            s!=Status::Ok ==> final(self).pending_row()==old(self).pending_row();
    fn install(&mut self,a:Admission)->(s:Status)
        requires old(self).wf(),old(self).recovered(),!a.namespace.coordinator,a.namespace.owner==old(self).owner(),
        ensures final(self).wf(),final(self).owner()==old(self).owner(),final(self).recovered(),
            final(self).durable_state()==old(self).durable_state(),final(self).working_state()==old(self).working_state(),
            final(self).pending_row()==old(self).pending_row(),final(self).transactions()==old(self).transactions(),
            s==Status::Ok ==> final(self).installed()==Some(a);
    fn check(&self,a:&Admission)->(yes:bool)
        ensures yes==(self.installed()==Some(*a) && self.pending_row()==None
            && self.owner()==a.namespace.owner && !a.namespace.coordinator),
            yes ==> self.working_state()==self.durable_state();
    fn scan(&self,a:&Admission,r:&KeyRange,after:Option<&Row>)->(out:Result<Option<Row>,Status>)
        requires self.wf(),
        ensures out is Ok ==> self.installed()==Some(*a) && self.pending_row()==None
            && first(self.working_state().bytes,*r,match after{None=>None,Some(v)=>Some(identity(*v))},out->Ok_0);

    /// Committed publishes one atomic durable byte/version effect; NotCommitted
    /// is a true stutter. Unknown may have applied that SAME effect, leaves the
    /// working projection frozen, and retains the accepted effect for recovery.
    fn mutate(&mut self,a:&Admission,r:&KeyRange,row:&Row,delete:bool)->(out:CommitResult)
        requires old(self).wf(),in_range(*r,row_cell(r.table,*row)),
        ensures final(self).wf(),final(self).owner()==old(self).owner(),
            final(self).installed()==old(self).installed(),final(self).transactions()==old(self).transactions(),
            old(self).installed()!=Some(*a) || old(self).pending_row() is Some
                || !version_available(old(self).durable_state(),row_cell(r.table,*row)) ==> out==CommitResult::NotCommitted,
            ({let effect=RowEffect{cell:row_cell(r.table,*row),value:if delete{None}else{Some(row.value@)},
                version:next_version(old(self).durable_state(),row_cell(r.table,*row))};
                match out{
                    CommitResult::Committed=>final(self).durable_state()==row_apply(old(self).durable_state(),effect)
                        && final(self).working_state()==row_apply(old(self).working_state(),effect) && final(self).pending_row()==None,
                    CommitResult::NotCommitted=>final(self).durable_state()==old(self).durable_state()
                        && final(self).working_state()==old(self).working_state() && final(self).pending_row()==old(self).pending_row(),
                    CommitResult::Unknown=>settled_rows(old(self).durable_state(),Some(effect),final(self).durable_state())
                        && final(self).working_state()==old(self).working_state() && final(self).pending_row()==Some(effect),
                }});

    /// Ordering barrier. It cannot invent bytes or repair a partial copy. An
    /// unresolved raw mutation cannot return Committed. Working image and all
    /// transaction records are framed, even on Unknown.
    fn commit_rows(&mut self,a:&Admission,r:&KeyRange)->(out:CommitResult)
        requires old(self).wf(),
        ensures final(self).wf(),final(self).owner()==old(self).owner(),
            final(self).working_state()==old(self).working_state(),final(self).transactions()==old(self).transactions(),
            final(self).durable_state()==old(self).durable_state(),final(self).pending_row()==old(self).pending_row(),
            final(self).installed()==old(self).installed(),
            out==CommitResult::Committed ==> old(self).installed()==Some(*a) && old(self).pending_row()==None
                && final(self).durable_state()==old(self).working_state();

    /// The accepted durable write set and decision belong to the engine's
    /// distributed transaction record. Success is a fence covering surviving
    /// peers and queued RPCs; the exact id can never produce another effect.
    /// Failure can lose a response after the SAME recorded atomic effect, but
    /// cannot modify any unrelated row or another transaction record.
    /// An absent transaction is NOT evidence of settlement: after the same
    /// global old-work fence actually completes, the backend may durably install
    /// an empty-write Abort tombstone for the exact id. This covers a lease
    /// admission whose reply was lost before engine registration. Mere local
    /// absence or timeout must not take this branch.
    fn settle_transaction(&mut self,a:&Admission,id:TxnId)->(s:Status)
        requires old(self).wf(),
        ensures final(self).wf(),final(self).owner()==old(self).owner(),
            final(self).installed()==old(self).installed(),final(self).pending_row()==old(self).pending_row(),
            settlement_effect(old(self).durable_state(),old(self).working_state(),old(self).transactions(),
                final(self).durable_state(),final(self).working_state(),final(self).transactions(),
                old(self).installed()==Some(*a) && old(self).pending_row()==None,id),
            s==Status::Ok ==> old(self).installed()==Some(*a) && old(self).pending_row()==None
                && settled(final(self).transactions(),id);
}
/// First-order row/receipt relation: the primitive trait must not depend on a
/// generic helper that itself requires the same trait.
pub open spec fn settlement_effect(before:RowState,working:RowState,records:Map<TxnId,TxnRecord>,
    after:RowState,after_working:RowState,terminal:Map<TxnId,TxnRecord>,admitted:bool,id:TxnId)->bool {
    (after==before && after_working==working && terminal==records)
        || (admitted && records.contains_key(id) && terminal==records.insert(id,terminal[id])
            && txn_effect(before,records[id],after,terminal[id])
            && txn_effect(working,records[id],after_working,terminal[id]))
        || (admitted && !records.contains_key(id) && after==before && after_working==working
            && terminal==records.insert(id,TxnRecord{writes:Seq::empty(),decision:TxnDecision::Abort,settled:true}))
}
pub open spec fn transaction_settlement<E:DurableEngine>(before:E,after:E,a:Admission,id:TxnId)->bool {
    after.owner()==before.owner() && after.installed()==before.installed() && after.pending_row()==before.pending_row()
        && settlement_effect(before.durable_state(),before.working_state(),before.transactions(),
            after.durable_state(),after.working_state(),after.transactions(),
            before.installed()==Some(a) && before.pending_row()==None,id)
}

/// The actual native transfer loops see this checked adapter. Unknown raw
/// durability freezes the materialized image and returns Io (not NotApplied);
/// the wrapper fences and recover_rows resolves it before further admission.
pub struct DurableRows<'a,E:DurableEngine>{
    engine:&'a mut E,
    admission:Admission,
    window:Ghost<KeyRange>,
    initial:Ghost<E>,
}
impl<'a,E:DurableEngine> RawStore for DurableRows<'a,E>{
    type Context=(Admission,KeyRange,E,E);
    #[verifier::prophetic]
    closed spec fn context(&self)->Self::Context{(self.admission,self.window@,self.initial@,*final(self.engine))}
    closed spec fn image(&self,_r:KeyRange)->Image{self.engine.working_state().bytes}
    closed spec fn scope(&self)->Option<KeyRange>{Some(self.window@)}
    closed spec fn wf(&self)->bool{
        self.engine.wf() && self.engine.owner()==self.admission.namespace.owner
            && self.engine.installed()==Some(self.admission)
            && self.engine.transactions()==self.initial@.transactions()
            && settled_rows(self.engine.working_state(),self.engine.pending_row(),self.engine.durable_state())
            && (self.engine.pending_row() is Some ==> in_range(self.window@,self.engine.pending_row().unwrap().cell))
            && storage::preserves_outside(self.engine.working_state().bytes,self.initial@.working_state().bytes,self.window@)
            && storage::preserves_outside(self.engine.durable_state().bytes,self.initial@.durable_state().bytes,self.window@)
            && versions_outside(self.engine.working_state().versions,self.initial@.working_state().versions,self.window@)
            && versions_outside(self.engine.durable_state().versions,self.initial@.durable_state().versions,self.window@)
    }
    closed spec fn available(&self,_r:KeyRange)->bool{false}
    fn scan(&self,r:&KeyRange,after:Option<&Row>)->(out:Result<Option<Row>,Status>){self.engine.scan(&self.admission,r,after)}
    fn put(&mut self,r:&KeyRange,row:&Row)->(s:Status){
        let result=self.engine.mutate(&self.admission,r,row,false);
        match result{CommitResult::Committed=>Status::Ok,CommitResult::NotCommitted=>Status::Retry,CommitResult::Unknown=>Status::Io}
    }
    fn delete(&mut self,r:&KeyRange,row:&Row)->(s:Status){
        let result=self.engine.mutate(&self.admission,r,row,true);
        match result{CommitResult::Committed=>Status::Ok,CommitResult::NotCommitted=>Status::Retry,CommitResult::Unknown=>Status::Io}
    }
}
pub fn copy_rows<E:DurableEngine,S:crate::storage::Source>(node:&mut crate::participant::Participant,
    engine:&mut E,a:Admission,plan:&crate::types::MigrationPlan,source:&S)->(out:crate::participant::ControlResult)
    requires old(node).wf(),old(engine).wf(),source.wf(),
        old(engine).owner()==a.namespace.owner,old(engine).installed()==Some(a),
        old(engine).pending_row()==None,old(engine).working_state()==old(engine).durable_state(),
    ensures final(node).wf(),final(engine).wf(),
        final(node).lease_view()==old(node).lease_view(),final(node).owner_view()==old(node).owner_view(),
        final(node).table_ids()==old(node).table_ids(),
        out.cleanup==crate::participant::Cleanup::None,
        out.certificate.is_some() ==> final(node).receipt_view()=={
            let r=old(node).receipt_view()[plan.generation];old(node).receipt_view().insert(plan.generation,(r.0,r.1,r.2,true))
        },
        out.certificate.is_some() ==> out.certificate==Some(crate::types::Certificate::Ready)
            && out.status==Status::Ok && old(node).transfer_authorized(*plan,false)
            && final(node).seal_frame(*old(node),*plan)
            && storage::complete(final(engine).working_state().bytes,old(engine).working_state().bytes,source.image(),plan.range),
        storage::preserves_outside(final(engine).working_state().bytes,old(engine).working_state().bytes,plan.range),
        storage::preserves_outside(final(engine).durable_state().bytes,old(engine).durable_state().bytes,plan.range),
        versions_outside(final(engine).working_state().versions,old(engine).working_state().versions,plan.range),
        versions_outside(final(engine).durable_state().versions,old(engine).durable_state().versions,plan.range),
        final(engine).pending_row() is Some ==> in_range(plan.range,final(engine).pending_row().unwrap().cell),
        final(engine).owner()==old(engine).owner(),final(engine).installed()==old(engine).installed(),
        final(engine).transactions()==old(engine).transactions(),
        storage::prefix(final(engine).working_state().bytes,old(engine).working_state().bytes,source.image(),plan.range),
        out.certificate.is_none() ==> final(node).local_unchanged(*old(node)) && final(node).same_metadata(*old(node)),
        final(engine).working_state().bytes!=old(engine).working_state().bytes ==> old(node).transfer_authorized(*plan,false),
{
    let ghost initial=*engine;
    let mut rows=DurableRows{engine:&mut *engine,admission:a,window:Ghost(plan.range),initial:Ghost(initial)};
    let result=crate::transfer::execute_final(node,&mut rows,plan,source);
    let raw=rows.engine;
    proof{assert(raw.wf());}
    result
}
pub fn cleanup_rows<E:DurableEngine>(node:&mut crate::participant::Participant,
    engine:&mut E,a:Admission,plan:&crate::types::MigrationPlan)->(out:crate::participant::ControlResult)
    requires old(node).wf(),old(engine).wf(),old(engine).owner()==a.namespace.owner,old(engine).installed()==Some(a),
        old(engine).pending_row()==None,old(engine).working_state()==old(engine).durable_state(),
    ensures final(node).wf(),final(engine).wf(),
        final(node).lease_view()==old(node).lease_view(),final(node).owner_view()==old(node).owner_view(),
        final(node).table_ids()==old(node).table_ids(),
        out.cleanup==crate::participant::Cleanup::None,
        out.certificate.is_some() ==> final(node).receipt_view()=={
            let r=old(node).receipt_view()[plan.generation];old(node).receipt_view().insert(plan.generation,(r.0,r.1,true,r.3))
        },
        out.certificate.is_some() ==> out.status==Status::Ok && old(node).transfer_authorized(*plan,true)
            && final(node).cleanup_done(plan.generation) && final(node).local_unchanged(*old(node))
            && storage::complete(final(engine).working_state().bytes,old(engine).working_state().bytes,Map::empty(),plan.range),
        storage::preserves_outside(final(engine).working_state().bytes,old(engine).working_state().bytes,plan.range),
        storage::preserves_outside(final(engine).durable_state().bytes,old(engine).durable_state().bytes,plan.range),
        versions_outside(final(engine).working_state().versions,old(engine).working_state().versions,plan.range),
        versions_outside(final(engine).durable_state().versions,old(engine).durable_state().versions,plan.range),
        final(engine).pending_row() is Some ==> in_range(plan.range,final(engine).pending_row().unwrap().cell),
        final(engine).owner()==old(engine).owner(),final(engine).installed()==old(engine).installed(),
        final(engine).transactions()==old(engine).transactions(),
        storage::prefix(final(engine).working_state().bytes,old(engine).working_state().bytes,Map::empty(),plan.range),
        final(node).local_unchanged(*old(node)),
        out.certificate.is_none() ==> final(node).same_metadata(*old(node)),
        out.certificate.is_some() ==> out.certificate==Some(if old(node).owner_view()==plan.source{
            crate::types::Certificate::SourceDone}else{crate::types::Certificate::DestinationDone}),
{
    let ghost initial=*engine;
    let mut rows=DurableRows{engine:&mut *engine,admission:a,window:Ghost(plan.range),initial:Ghost(initial)};
    let result=crate::transfer::execute_cleanup(node,&mut rows,plan);
    let raw=rows.engine;
    proof{assert(raw.wf());}
    result
}
} // verus!
