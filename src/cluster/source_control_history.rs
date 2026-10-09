//! Coordinator observations, not caller-certified model transitions.
use super::*;
use crate::migration as n;
use crate::types::{Status,TxnId,KeyRange,Outcome,Certificate};
verus! {
pub enum ControlCall {
    Begin { id:TxnId,source:u32,destination:u32,range:KeyRange,result:Result<u64,Status> },
    Transition { request:n::Request,status:Status },
    Reply { id:TxnId,result:Option<Outcome> },
    Receive { generation:u64,owner:u32,certificate:Certificate,status:Status },
}
pub struct ControlRecord { pub before:n::Native,pub after:n::Native,pub call:ControlCall }
impl ControlRecord {
    pub open spec fn observed(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> bool {
        let b=self.before; let z=self.after; let v=s.placement; let o=ctx.observations;
        n::layout(b) && master::master(b,v) && match self.call {
            ControlCall::Begin { id,source,destination,range,result } => {
                n::begin_effect(b,z,id,source,destination,range,result)
                    && (result is Ok ==> master::observes(c,b,o) && master::range_observed(o,range)
                        && p::directory(v)==master::directory(c,b,o)
                        && (forall|owner:u32| b.nodes.contains(owner) ==> c.shards.contains(owner as int))
                        && master::selected(c,o,range)!=Set::<int>::empty()
                        && (forall|k:int| c.keys.contains(k) && c.table[k]==range.table ==>
                            o.slots[k]==z.records.last().slot))
            },
            ControlCall::Transition { request,status } => {
                n::transition_effect(b,z,request,status)
                    && (request is Commit && status==Status::Ok ==> {
                        b.active is Some && master::observes(c,b,o)
                            && master::prepared(b,b.active.unwrap() as int)
                            && b.current[b.records[b.active.unwrap() as int].slot as int]
                                ==b.records[b.active.unwrap() as int].previous
                            && master::range_observed(o,b.records[b.active.unwrap() as int].plan.range)
                            && p::directory(v)==master::directory(c,b,o)
                            && v.plans[v.active.unwrap()].keys==master::selected(c,o,b.records[b.active.unwrap() as int].plan.range)
                            && v.plans[v.active.unwrap()].dst==b.records[b.active.unwrap() as int].plan.destination
                            && (forall|k:int| c.keys.contains(k) ==>
                                (c.table[k]==b.records[b.active.unwrap() as int].plan.range.table)
                                    ==(o.slots[k]==b.records[b.active.unwrap() as int].slot))
                    })
            },
            ControlCall::Reply { id,result } => n::reply_effect(b,z,id,result),
            ControlCall::Receive { generation,owner,certificate,status } => {
                n::receive_effect(b,z,generation,owner,certificate,status)
                    && (status==Status::Ok ==> s.emitted.contains((generation,owner,certificate))
                        && v.certificates.contains((generation as nat,master::certificate(certificate))))
            },
        }
    }
    /// Exact native field projection; neither dispatch nor a model successor is input.
    pub open spec fn image(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> SourceState {
        let v=s.placement;
        let placement=match self.call {
            ControlCall::Begin { result,.. } => if result is Ok {
                log::apply_writes(v,master::begin_writes(c,self.before,self.after,ctx.observations))
            } else { v },
            ControlCall::Transition { request,status } => master::transition_image(self.before,self.after,v,
                request,master::directory(c,self.after,ctx.observations),status),
            ControlCall::Reply { id,result } => if result is Some {
                p::State { replies:v.replies.insert(master::nonce(id),master::outcome(result.unwrap())),..v }
            } else { v },
            ControlCall::Receive { generation,certificate,status,.. } => if status==Status::Ok {
                p::State { received:v.received.insert((generation as nat,master::certificate(certificate))),..v }
            } else { v },
        };
        let plans=match self.call {
            ControlCall::Begin { result:Ok(g),.. } => s.plans.insert(g,self.after.records.last().plan),
            _ => s.plans,
        };
        SourceState { placement,plans,..s }
    }
    pub proof fn certify(&self,c:p::Constants,ctx:SourceContext,s:SourceState) -> (tracked out:ClosedTransfer)
        requires self.observed(c,ctx,s),
        ensures out.valid(c,s.placement),out.after(s.placement)==self.image(c,ctx,s).placement,
    {
        let b=self.before; let z=self.after; let v=s.placement;
        match self.call {
            ControlCall::Begin { id,source,destination,range,result } => match result {
                Ok(g)=>coordinator_begin(c,b,z,ctx.observations,v,id,source,destination,range,g),
                Err(error)=>rejected_begin(c,b,z,v,id,source,destination,range,error),
            },
            ControlCall::Transition { request,status } => coordinator_transition(c,b,z,v,request,status,ctx.observations),
            ControlCall::Reply { id,result } => coordinator_reply(c,b,z,v,id,result),
            ControlCall::Receive { generation,owner,certificate,status } => {
                if status==Status::Ok {
                    // The history fold starts with no emissions and adds them only
                    // at checked native response events. It never accepts a token.
                    let tracked emitted=EmittedCertificate { generation,owner,certificate };
                    reveal(EmittedCertificate::matches);
                    coordinator_receive(c,b,z,v,generation,owner,certificate,&emitted)
                } else { rejected_receive(c,b,z,v,generation,owner,certificate,status) }
            },
        }
    }
}
} // verus!
