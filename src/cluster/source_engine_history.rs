//! Trusted atomic engine events are distinct from delivery of their outcomes.
//! An Unknown callback neither erases an actual settlement nor releases a lease.
use super::*;
use crate::types::TxnId;
verus! {
pub enum EngineRecord {
    Settle { id:TxnId,outcome:EngineOutcome,before:EngineImages,after:EngineImages,
        writes:Map<int,Option<Seq<u8>>> },
    Completion { id:TxnId,outcome:EngineOutcome },
    Read { id:TxnId,key:int,before:Image,after:Image,returned:Option<Seq<u8>> },
}
impl EngineRecord {
    pub open spec fn observed(&self,ctx:SourceContext,s:SourceState) -> bool {
        let v=s.placement;
        match *self {
            EngineRecord::Settle { id,outcome,before,after,writes } => {
                before==s.images && !s.settled.contains_key(id)
                    && match outcome {
                        EngineOutcome::Committed => p::can_resolve(v,transaction(id),encoded_writes(writes))
                            && atomic_bytes(v,transaction(id),before,after,ctx.labels,writes)
                            && engine_observed(v,before,ctx.labels)
                            && (forall|k:int| writes.contains_key(k) ==> v.logical.contains_key(k)
                                && v.physical.contains_key((v.sessions[transaction(id)].held[k].owner,k))),
                        EngineOutcome::Aborted => before==after && v.sessions.contains_key(transaction(id))
                            && !v.sessions[transaction(id)].resolved && writes==Map::<int,Option<Seq<u8>>>::empty(),
                        EngineOutcome::Unknown => false,
                    }
            },
            EngineRecord::Completion { id,outcome } => outcome is Unknown
                || s.settled.contains_key(id) && s.settled[id]==outcome
                    && v.sessions.contains_key(transaction(id)) && v.sessions[transaction(id)].resolved,
            EngineRecord::Read { id,key,before,after,returned } => ctx.labels.contains_key(key)
                && p::live_lease(v,transaction(id),key)
                && s.images.contains_key(v.sessions[transaction(id)].held[key].owner)
                && before==s.images[v.sessions[transaction(id)].held[key].owner]
                && before==after && returned==crate::storage::value(before,ctx.labels[key])
                && p::replica(v,v.sessions[transaction(id)].held[key].owner,key).cell.value
                    ==crate::sharding_bytes::value_option(crate::storage::value(before,ctx.labels[key])),
        }
    }
    pub open spec fn image(&self,ctx:SourceContext,s:SourceState) -> SourceState {
        let v=s.placement;
        match *self {
            EngineRecord::Settle { id,outcome,after,writes,.. } => {
                let placement=if outcome is Committed {
                    engine::engine_state(v,transaction(id),writes.dom(),Map::new(writes.dom(),|k:int|
                        transfer::observed(after[v.sessions[transaction(id)].held[k].owner],ctx.labels[k],transaction(id))))
                } else { p::State { sessions:v.sessions.insert(transaction(id),
                    p::Session { resolved:true,..v.sessions[transaction(id)] }),..v } };
                SourceState { placement,images:after,settled:s.settled.insert(id,outcome),..s }
            },
            EngineRecord::Completion { id,outcome } => if outcome is Unknown { s }
                else { SourceState { completed:s.completed.insert(id),..s } },
            EngineRecord::Read { .. } => s,
        }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires self.observed(ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(ctx,s).placement,
    {
        let v=s.placement;
        match *self {
            EngineRecord::Settle { id,outcome,before,after,writes } => {
                if outcome is Committed {
                    engine::engine_effect(c,v,transaction(id),outcome,before,after,ctx.labels,writes)
                } else {
                    let tracked (effect,unused_completion)=engine_aborted(c,v,id,outcome,before,after);
                    effect
                }
            },
            EngineRecord::Completion { .. } => empty_segment(c,v),
            EngineRecord::Read { id,key,before,after,returned } =>
                engine_read(c,v,id,key,ctx.labels[key],before,after,returned),
        }
    }
}
} // verus!
