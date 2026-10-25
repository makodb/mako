//! Receipt and engine-completion provenance follows the actual source sequence.
//! These lemmas cannot be satisfied by supplying preconstructed private tokens.
use super::*;
verus! {
impl SourceEvent {
    pub open spec fn captures_private(&self,packet:PrivatePacket) -> bool {
        match *self { Self::CapturePrivate(e)=>e.packet()==packet,_=>false }
    }
    pub open spec fn emits(&self,receipt:(u64,u32,Certificate)) -> bool {
        match *self {
            Self::Seal(e)=>receipt==(e.plan.generation,e.after.owner_view(),Certificate::Ready),
            Self::Drain(e)=>receipt==(e.plan.generation,e.node.owner_view(),Certificate::Drained),
            Self::Immediate(e)=>receipt==(e.plan.generation,e.node.owner_view(),e.certificate()),
            Self::Cleanup(e)=>receipt==(e.plan.generation,e.after.owner_view(),e.certificate()),
            _=>false,
        }
    }
    pub open spec fn settles(&self,id:TxnId,outcome:EngineOutcome) -> bool {
        match *self {
            Self::Engine(EngineRecord::Settle { id:actual,outcome:result,.. })=>id==actual && outcome==result,
            _=>false,
        }
    }
    pub open spec fn completes(&self,id:TxnId,outcome:EngineOutcome) -> bool {
        match *self {
            Self::Engine(EngineRecord::Completion { id:actual,outcome:result })=>
                id==actual && outcome==result && !(result is Unknown),
            _=>false,
        }
    }
}

pub proof fn receipt_origin(c:p::Constants,ctx:SourceContext,states:Seq<SourceState>,events:Seq<SourceEvent>,at:nat,
    receipt:(u64,u32,Certificate))
    requires source_trace(c,ctx,states,events),states[0].emitted==Set::<(u64,u32,Certificate)>::empty(),
        at<states.len(),states[at as int].emitted.contains(receipt),
    ensures exists|j:int| 0<=j<at && events[j].emits(receipt) && events[j].observed(c,ctx,states[j]),
    decreases at,
{
    assert(at>0);
    let previous=(at-1) as nat;
    assert(events[previous as int].observed(c,ctx,states[previous as int]));
    assert(states[at as int]==events[previous as int].image(c,ctx,states[previous as int]));
    if states[previous as int].emitted.contains(receipt) {
        receipt_origin(c,ctx,states,events,previous,receipt);
    } else {
        assert(events[previous as int].emits(receipt));
    }
}

pub proof fn settlement_origin(c:p::Constants,ctx:SourceContext,states:Seq<SourceState>,events:Seq<SourceEvent>,at:nat,
    id:TxnId,outcome:EngineOutcome)
    requires source_trace(c,ctx,states,events),states[0].settled==Map::<TxnId,EngineOutcome>::empty(),
        at<states.len(),states[at as int].settled.contains_key(id),states[at as int].settled[id]==outcome,
    ensures !(outcome is Unknown),exists|j:int| 0<=j<at && events[j].settles(id,outcome)
        && events[j].observed(c,ctx,states[j]),
    decreases at,
{
    assert(at>0);
    let previous=(at-1) as nat;
    assert(events[previous as int].observed(c,ctx,states[previous as int]));
    assert(states[at as int]==events[previous as int].image(c,ctx,states[previous as int]));
    if states[previous as int].settled.contains_key(id) {
        assert(states[previous as int].settled[id]==outcome);
        settlement_origin(c,ctx,states,events,previous,id,outcome);
    } else {
        assert(events[previous as int].settles(id,outcome));
    }
}

pub proof fn completion_origin(c:p::Constants,ctx:SourceContext,states:Seq<SourceState>,events:Seq<SourceEvent>,at:nat,id:TxnId)
    requires source_trace(c,ctx,states,events),states[0].settled==Map::<TxnId,EngineOutcome>::empty(),
        states[0].completed==Set::<TxnId>::empty(),at<states.len(),states[at as int].completed.contains(id),
    ensures exists|settlement:int,callback:int,outcome:EngineOutcome| 0<=settlement<callback<at
        && !(outcome is Unknown) && events[settlement].settles(id,outcome) && events[callback].completes(id,outcome)
        && events[settlement].observed(c,ctx,states[settlement]) && events[callback].observed(c,ctx,states[callback]),
    decreases at,
{
    assert(at>0);
    let previous=(at-1) as nat;
    assert(events[previous as int].observed(c,ctx,states[previous as int]));
    assert(states[at as int]==events[previous as int].image(c,ctx,states[previous as int]));
    if states[previous as int].completed.contains(id) {
        completion_origin(c,ctx,states,events,previous,id);
    } else {
        let outcome=states[previous as int].settled[id];
        assert(events[previous as int].completes(id,outcome));
        settlement_origin(c,ctx,states,events,previous,id,outcome);
    }
}

/// An accepted acknowledgement has an earlier checked native emission. A held
/// transaction is released only after an actual settlement and its known callback.
pub proof fn source_authorization_origins(c:p::Constants,ctx:SourceContext,states:Seq<SourceState>,events:Seq<SourceEvent>,at:nat)
    requires source_trace(c,ctx,states,events),at<events.len(),
        states[0].emitted==Set::<(u64,u32,Certificate)>::empty(),
        states[0].settled==Map::<TxnId,EngineOutcome>::empty(),states[0].completed==Set::<TxnId>::empty(),
    ensures match events[at as int] {
        SourceEvent::Control(e)=>match e.call {
            ControlCall::Receive { generation,owner,certificate,status }=>status==Status::Ok ==>
                exists|j:int| 0<=j<at && events[j].emits((generation,owner,certificate))
                    && events[j].observed(c,ctx,states[j]),
            _=>true,
        },
        SourceEvent::Registry(e)=>e.call is Finish && e.status==Status::Ok ==>
            exists|settlement:int,callback:int,outcome:EngineOutcome| 0<=settlement<callback<at
                && !(outcome is Unknown) && events[settlement].settles(e.id,outcome) && events[callback].completes(e.id,outcome),
        _=>true,
    },
{
    assert(events[at as int].observed(c,ctx,states[at as int]));
    match events[at as int] {
        SourceEvent::Control(e)=>match e.call {
            ControlCall::Receive { generation,owner,certificate,status }=>{
                if status==Status::Ok { receipt_origin(c,ctx,states,events,at,(generation,owner,certificate)); }
            },
            _=>{},
        },
        SourceEvent::Registry(e)=>{
            if e.call is Finish && e.status==Status::Ok { completion_origin(c,ctx,states,events,at,e.id); }
        },
        _=>{},
    }
}

/// Every participant mutation carries the proved native receipt frame. Other
/// owners and read-only calls cannot replace the receiver with a fresh object.
pub proof fn participant_receipt_retained(c:p::Constants,ctx:SourceContext,states:Seq<SourceState>,events:Seq<SourceEvent>,
    start:nat,end:nat,owner:u32,generation:u64)
    requires source_trace(c,ctx,states,events),start<=end<states.len(),
        states[start as int].nodes.contains_key(owner),states[start as int].nodes[owner].has_receipt(generation),
    ensures states[end as int].nodes.contains_key(owner),states[end as int].nodes[owner].has_receipt(generation),
    decreases end-start,
{
    if start<end {
        let previous=(end-1) as nat;
        participant_receipt_retained(c,ctx,states,events,start,previous,owner,generation);
        assert(events[previous as int].observed(c,ctx,states[previous as int]));
        assert(states[end as int]==events[previous as int].image(c,ctx,states[previous as int]));
        events[previous as int].receipt_step(c,ctx,states[previous as int],owner,generation);
    }
}
/// In particular, the successful Final delivery used to issue a mirror job
/// supplies the receipt needed by its later checked native seal callback.
pub proof fn retained_metadata_receipt(c:p::Constants,ctx:SourceContext,states:Seq<SourceState>,events:Seq<SourceEvent>,
    at:nat,end:nat,record:MetadataRecord)
    requires source_trace(c,ctx,states,events),at<events.len(),at<end<states.len(),
        events[at as int]==SourceEvent::Metadata(record),record.result.status==Status::Ok,
    ensures states[end as int].nodes.contains_key(record.before.owner_view()),
        states[end as int].nodes[record.before.owner_view()].has_receipt(record.plan.generation),
{
    let owner=record.before.owner_view();
    assert(events[at as int].observed(c,ctx,states[at as int]));
    assert(states[at as int+1]==events[at as int].image(c,ctx,states[at as int]));
    assert(states[at as int+1].nodes[owner]==record.after);
    assert(record.after.has_receipt(record.plan.generation));
    participant_receipt_retained(c,ctx,states,events,at+1,end,owner,record.plan.generation);
}

pub proof fn private_packet_origin(c:p::Constants,ctx:SourceContext,states:Seq<SourceState>,events:Seq<SourceEvent>,
    at:nat,packet:PrivatePacket)
    requires source_trace(c,ctx,states,events),states[0].private_packets==Set::<PrivatePacket>::empty(),
        at<states.len(),states[at as int].private_packets.contains(packet),
    ensures exists|j:int| 0<=j<at && events[j].captures_private(packet) && events[j].observed(c,ctx,states[j]),
    decreases at,
{
    assert(at>0);
    let previous=(at-1) as nat;
    assert(events[previous as int].observed(c,ctx,states[previous as int]));
    assert(states[at as int]==events[previous as int].image(c,ctx,states[previous as int]));
    if states[previous as int].private_packets.contains(packet) {
        private_packet_origin(c,ctx,states,events,previous,packet);
    } else {
        assert(events[previous as int].captures_private(packet));
    }
}
pub proof fn private_copy_origin(c:p::Constants,ctx:SourceContext,states:Seq<SourceState>,events:Seq<SourceEvent>,at:nat)
    requires source_trace(c,ctx,states,events),states[0].private_packets==Set::<PrivatePacket>::empty(),
        at<events.len(),events[at as int] is CopyPrivate,
    ensures match events[at as int] {
        SourceEvent::CopyPrivate(e)=>exists|j:int| 0<=j<at && events[j].captures_private(e.packet)
            && events[j].observed(c,ctx,states[j]),
        _=>false,
    },
{
    assert(events[at as int].observed(c,ctx,states[at as int]));
    if let SourceEvent::CopyPrivate(e)=events[at as int] {
        private_packet_origin(c,ctx,states,events,at,e.packet);
    }
}

pub proof fn unknown_callback_cannot_complete(ctx:SourceContext,s:SourceState,id:TxnId)
    requires !s.completed.contains(id),
    ensures !(EngineRecord::Completion { id,outcome:EngineOutcome::Unknown }).image(ctx,s).completed.contains(id),
        (EngineRecord::Completion { id,outcome:EngineOutcome::Unknown }).image(ctx,s).registries==s.registries,
{}
} // verus!
