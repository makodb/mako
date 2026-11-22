//! Versioned participant record grammar. Schema values are not completion capabilities.
use vstd::prelude::*;
use crate::types::*;
use crate::directory::RouteTable;
use crate::routing_codec::{Reader,Writer};
use crate::durable_codec::*;
#[cfg(verus_keep_ghost)]
use crate::routing_codec_proofs::{word32,word64,blob,tables_wire,table_wire,join_wire,wire_prefix,prefix_split};
verus! {
pub const TAG:u64=0x015450444f4b414d;
pub enum Operation {
    Bootstrap{owner:u32,tables:Vec<RouteTable>},
    Bind(MigrationPlan),
    Deliver{plan:MigrationPlan,command:Command},
    Begin{id:TxnId,incarnation:u64},Resolve(TxnId),Finish(TxnId),
    Acquire{id:TxnId,range:KeyRange,point:bool,grant:Grant},
    Release{id:TxnId,table:u64,coordinate:Vec<u8>},
    Reserve{plan:MigrationPlan,cleanup:bool},
    Complete{plan:MigrationPlan,cleanup:bool},
    Settled,
}
pub open spec fn bool_wire(b:bool)->Seq<u8>{word32(if b{1}else{0})}
pub open spec fn operation_tag(op:Operation)->u32 {
    match op{
        Operation::Bootstrap{..}=>0,Operation::Bind(_)=>1,Operation::Deliver{..}=>2,
        Operation::Begin{..}=>3,Operation::Resolve(_)=>4,Operation::Finish(_)=>5,Operation::Acquire{..}=>6,
        Operation::Release{..}=>7,Operation::Reserve{..}=>8,Operation::Complete{..}=>9,Operation::Settled=>10,
    }
}
pub open spec fn body(op:Operation)->Seq<u8>{
    match op{
        Operation::Bootstrap{owner,tables}=>word32(owner)+(word64(tables.len() as u64)+tables_wire(tables@)),
        Operation::Bind(p)=>plan_wire(p),
        Operation::Deliver{plan,command}=>plan_wire(plan)+word32(command_code(command)),
        Operation::Begin{id,incarnation}=>txn_wire(id)+word64(incarnation),
        Operation::Resolve(id)|Operation::Finish(id)=>txn_wire(id),
        Operation::Acquire{id,range,point,grant}=>txn_wire(id)+(range_wire(range)+(bool_wire(point)+grant_wire(grant))),
        Operation::Release{id,table,coordinate}=>txn_wire(id)+(word64(table)+blob(coordinate@)),
        Operation::Reserve{plan,cleanup}|Operation::Complete{plan,cleanup}=>plan_wire(plan)+bool_wire(cleanup),
        Operation::Settled=>Seq::empty(),
    }
}
pub open spec fn payload(op:Operation)->Seq<u8>{word32(operation_tag(op))+body(op)}
pub open spec fn wire(op:Operation)->Seq<u8>{word64(TAG)+payload(op)}
/// The only excluded field shape is an empty routing table, which the native
/// bootstrap and immutable-plan admission reject before any journal append.
pub open spec fn encodable(op:Operation)->bool {
    match op{
        Operation::Bootstrap{tables,..}=>forall|i:int|0<=i<tables.len() ==> #[trigger] tables@[i].boundaries.len()>0,
        Operation::Bind(p)|Operation::Deliver{plan:p,..}|Operation::Reserve{plan:p,..}|Operation::Complete{plan:p,..}=>p.old.len()>0,
        _=>true,
    }
}
fn write_bool(w:&mut Writer,b:bool)->(s:Status)
    ensures s==Status::Ok ==> final(w).view()==old(w).view()+bool_wire(b),
{w.write_u32(if b{1}else{0})}
fn read_bool(r:&mut Reader)->(out:Result<bool,Status>)
    requires old(r).wf(),
    ensures final(r).wf(),final(r).data()==old(r).data(),final(r).position()>=old(r).position(),
        final(r).position()<=final(r).data().len(),
        out is Ok ==> final(r).data().subrange(old(r).position() as int,final(r).position() as int)==bool_wire(out->Ok_0),
        forall|expected:bool|wire_prefix(old(r).data(),old(r).position(),bool_wire(expected)) ==>
            out is Ok && out->Ok_0==expected && final(r).position()==old(r).position()+4,
{match r.read_u32(){Ok(0)=>Ok(false),Ok(1)=>Ok(true),_=>Err(Status::Invalid)}}
pub(super) fn encode(op:&Operation)->(out:Result<Vec<u8>,Status>)
    ensures out is Ok ==> out->Ok_0@==wire(*op),
        out is Ok && !(*op is Complete) ==> !super::history::is_completion(out->Ok_0@),
{
    hide(word32);hide(word64);hide(plan_wire);hide(range_wire);hide(txn_wire);hide(grant_wire);
    let mut w=Writer::new();
    let s=w.write_u64(TAG);if !matches!(s,Status::Ok){return Err(s);}
    let tag=match op{Operation::Bootstrap{..}=>0,Operation::Bind(_)=>1,Operation::Deliver{..}=>2,
        Operation::Begin{..}=>3,Operation::Resolve(_)=>4,Operation::Finish(_)=>5,Operation::Acquire{..}=>6,
        Operation::Release{..}=>7,Operation::Reserve{..}=>8,Operation::Complete{..}=>9,Operation::Settled=>10};
    let s=w.write_u32(tag);if !matches!(s,Status::Ok){return Err(s);}
    match op{
        Operation::Bootstrap{owner,tables}=>{
            let s=w.write_u32(*owner);if !matches!(s,Status::Ok){return Err(s);}
            let s=w.write_u64(tables.len() as u64);if !matches!(s,Status::Ok){return Err(s);}
            let ghost prefix=w.view();let mut i=0usize;
            while i<tables.len()
                invariant i<=tables.len(),w.view()==prefix+tables_wire(tables@.take(i as int)),
                decreases tables.len()-i,
            {
                let s=w.write_table(&tables[i]);if !matches!(s,Status::Ok){return Err(s);}i+=1;
                proof{
                    assert(tables@.take(i as int).drop_last()==tables@.take(i as int-1));
                    assert(tables@.take(i as int).last()==tables@[i as int-1]);
                    assert(w.view() =~= prefix+tables_wire(tables@.take(i as int)));
                }
            }
            proof{assert(tables@.take(tables.len() as int)==tables@);assert(w.view() =~= wire(*op));}
        }
        Operation::Bind(p)=>{
            let s=write_plan(&mut w,p);if !matches!(s,Status::Ok){return Err(s);}
            proof{assert(w.view() =~= wire(*op));}
        }
        Operation::Deliver{plan,command}=>{
            let s=write_plan(&mut w,plan);if !matches!(s,Status::Ok){return Err(s);}
            let s=write_command(&mut w,*command);if !matches!(s,Status::Ok){return Err(s);}
            proof{assert(w.view() =~= wire(*op));}
        }
        Operation::Begin{id,incarnation}=>{
            let s=write_txn(&mut w,*id);if !matches!(s,Status::Ok){return Err(s);}
            let s=w.write_u64(*incarnation);if !matches!(s,Status::Ok){return Err(s);}
            proof{assert(w.view() =~= wire(*op));}
        }
        Operation::Resolve(id)|Operation::Finish(id)=>{
            let s=write_txn(&mut w,*id);if !matches!(s,Status::Ok){return Err(s);}
            proof{assert(w.view() =~= wire(*op));}
        }
        Operation::Acquire{id,range,point,grant}=>{
            let s=write_txn(&mut w,*id);if !matches!(s,Status::Ok){return Err(s);}
            let s=write_range(&mut w,range);if !matches!(s,Status::Ok){return Err(s);}
            let s=write_bool(&mut w,*point);if !matches!(s,Status::Ok){return Err(s);}
            let s=write_grant(&mut w,*grant);if !matches!(s,Status::Ok){return Err(s);}
            proof{assert(w.view() =~= wire(*op));}
        }
        Operation::Release{id,table,coordinate}=>{
            let s=write_txn(&mut w,*id);if !matches!(s,Status::Ok){return Err(s);}
            let s=w.write_u64(*table);if !matches!(s,Status::Ok){return Err(s);}
            let s=w.write_bytes(coordinate);if !matches!(s,Status::Ok){return Err(s);}
            proof{assert(w.view() =~= wire(*op));}
        }
        Operation::Complete{plan,cleanup}=>{
            let s=write_plan(&mut w,plan);if !matches!(s,Status::Ok){return Err(s);}
            let s=write_bool(&mut w,*cleanup);if !matches!(s,Status::Ok){return Err(s);}
            proof{assert(w.view() =~= wire(*op));}
        }
        Operation::Reserve{plan,cleanup}=>{
            let s=write_plan(&mut w,plan);if !matches!(s,Status::Ok){return Err(s);}
            let s=write_bool(&mut w,*cleanup);if !matches!(s,Status::Ok){return Err(s);}
            proof{assert(w.view() =~= wire(*op));}
        }
        Operation::Settled=>{},
    }
    proof{if !(*op is Complete){super::history::not_completion(*op);}}
    Ok(w.into_bytes())
}
fn read_bootstrap(r:&mut Reader,Ghost(expected):Ghost<Option<Operation>>)->(out:Result<Operation,Status>)
    requires old(r).wf(),expected is Some ==> expected->Some_0 is Bootstrap && encodable(expected->Some_0)
        && old(r).data().skip(old(r).position() as int)==body(expected->Some_0),
    ensures final(r).wf(),final(r).data()==old(r).data(),old(r).position()<=final(r).position()<=final(r).data().len(),
        out is Ok ==> out->Ok_0 is Bootstrap
            && final(r).data().subrange(old(r).position() as int,final(r).position() as int)==body(out->Ok_0),
        expected is Some ==> out is Ok && same_operation(out->Ok_0,expected->Some_0)
            && final(r).position()==final(r).data().len(),
{
    hide(word32);hide(word64);
    let ghost data=r.data();let ghost begin=r.position();
    proof{r.position_bound();}
    let ghost has_expected=expected.is_some();
    let ghost expected_op=match expected{Some(op)=>op,None=>Operation::Settled};
    let ghost expected_tables=match expected_op{Operation::Bootstrap{tables,..}=>tables@,_=>Seq::empty()};
    let ghost expected_owner=match expected_op{Operation::Bootstrap{owner,..}=>owner,_=>0u32};
    proof{if has_expected{
        assert(expected_tables.len()==expected_op->Bootstrap_tables.len());
        assert(expected_tables.len()<=u64::MAX);
        assert forall|j:int|0<=j<expected_tables.len() implies
            #[trigger] expected_tables[j].boundaries.len()>0 by {
            assert(expected_op->Bootstrap_tables@[j].boundaries.len()>0);
        }
        suffix_prefix(data,begin,body(expected_op));
        prefix_split(data,begin,word32(expected_owner),word64(expected_tables.len() as u64)+tables_wire(expected_tables));
    }}
    let owner=match r.read_u32(){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost count_start=r.position();
    proof{if has_expected{prefix_split(data,r.position(),word64(expected_tables.len() as u64),tables_wire(expected_tables));}}
    let count=match r.read_u64(){Ok(v)=>v,Err(e)=>return Err(e)};
    proof{
        join_wire(data,begin,count_start,r.position(),word32(owner),word64(count));
        if has_expected{
            crate::routing_codec_proofs::tables_size(expected_tables);
            assert(r.position()+tables_wire(expected_tables).len()==data.len());
        }
    }
    if count>(r.remaining()/36) as u64{return Err(Status::Invalid);}
    let ghost start=r.position();let mut tables=Vec::new();let mut i=0u64;
    let ghost header=word32(owner)+word64(count);
    proof{
        assert(tables@==Seq::<RouteTable>::empty());
        if has_expected{
            assert(expected_tables.skip(0) =~= expected_tables);
            assert(wire_prefix(data,start,tables_wire(expected_tables)));
            assert(count==expected_tables.len());
        }
    }
    while i<count
        invariant i<=count,tables.len()==i,r.wf(),r.data()==data,
            data==old(r).data(),begin==old(r).position(),
            begin<=start<=r.position()<=data.len(),
            data.subrange(begin as int,start as int)==header,
            data.subrange(start as int,r.position() as int)==tables_wire(tables@),
            has_expected==expected.is_some(),
            has_expected ==> expected_op==expected->Some_0 && expected_op is Bootstrap && encodable(expected_op)
                && data.skip(begin as int)==body(expected_op)
                && expected_tables==expected_op->Bootstrap_tables@ && expected_owner==expected_op->Bootstrap_owner
                && owner==expected_owner && count==expected_tables.len()
                && (forall|j:int|0<=j<expected_tables.len() ==> #[trigger] expected_tables[j].boundaries.len()>0)
                && wire_prefix(data,r.position(),tables_wire(expected_tables.skip(i as int)))
                && r.position()+tables_wire(expected_tables.skip(i as int)).len()==data.len(),
        decreases count-i,
    {
        let ghost mid=r.position();let ghost before=tables@;
        proof{if has_expected{
            crate::routing_codec_proofs::tables_head(expected_tables.skip(i as int));
            assert(expected_tables.skip(i as int)[0]==expected_tables[i as int]);
            assert(expected_tables.skip(i as int).skip(1)==expected_tables.skip(i as int+1));
            prefix_split(data,r.position(),table_wire(expected_tables[i as int]),tables_wire(expected_tables.skip(i as int+1)));
            assert(expected_tables[i as int].boundaries.len()>0);
        }}
        let t=match r.read_table(){Ok(v)=>v,Err(e)=>return Err(e)};
        let ghost table=t;tables.push(t);i+=1;
        proof{
            join_wire(data,start,mid,r.position(),tables_wire(before),table_wire(table));
            assert(tables@.drop_last()==before);assert(tables@.last()==table);
        }
    }
    let op=Operation::Bootstrap{owner,tables};
    proof{
        join_wire(data,begin,start,r.position(),header,tables_wire(op->Bootstrap_tables@));
        assert(data.subrange(begin as int,r.position() as int) =~= body(op));
        if has_expected{
            assert(r.position()==data.len());
            assert(wire(op)==wire(expected_op));
            operation_unique(op,expected_op);
        }
    }
    Ok(op)
}
// Keep independent operation grammars in separate verification units. Each
// helper consumes exactly its body; only decode owns the version/tag framing.
fn read_planned_body(r:&mut Reader,kind:u32,Ghost(expected):Ghost<Option<Operation>>)->(out:Result<Operation,Status>)
    requires old(r).wf(),kind==1 || kind==2 || kind==8 || kind==9,
        expected is Some ==> encodable(expected->Some_0) && operation_tag(expected->Some_0)==kind
            && old(r).data().skip(old(r).position() as int)==body(expected->Some_0),
    ensures final(r).wf(),final(r).data()==old(r).data(),old(r).position()<=final(r).position()<=final(r).data().len(),
        out is Ok ==> operation_tag(out->Ok_0)==kind
            && final(r).data().subrange(old(r).position() as int,final(r).position() as int)==body(out->Ok_0),
        expected is Some ==> out is Ok && final(r).position()==final(r).data().len(),
{
    hide(word32);hide(word64);hide(plan_wire);hide(range_wire);hide(txn_wire);hide(grant_wire);hide(tables_wire);
    hide(same_operation);hide(wire);hide(payload);
    let ghost data=r.data();let ghost start=r.position();
    proof{r.position_bound();}
    let ghost e=match expected{Some(op)=>op,None=>Operation::Settled};
    proof{if expected is Some{
        suffix_prefix(data,start,body(e));
        match e{
            Operation::Bind(p)=>{assert(wire_prefix(data,start,plan_wire(p)));},
            Operation::Deliver{plan,command}=>{prefix_split(data,start,plan_wire(plan),word32(command_code(command)));},
            Operation::Reserve{plan,cleanup}|Operation::Complete{plan,cleanup}=>{
                prefix_split(data,start,plan_wire(plan),bool_wire(cleanup));
            },
            _=>{assert(false);},
        }
    }}
    let plan=match read_plan(r){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost middle=r.position();
    let op=if kind==1{
        Operation::Bind(plan)
    }else if kind==2{
        let command=match read_command(r){Ok(v)=>v,Err(e)=>return Err(e)};
        proof{join_wire(data,start,middle,r.position(),plan_wire(plan),word32(command_code(command)));}
        Operation::Deliver{plan,command}
    }else{
        let cleanup=match read_bool(r){Ok(v)=>v,Err(e)=>return Err(e)};
        proof{join_wire(data,start,middle,r.position(),plan_wire(plan),bool_wire(cleanup));}
        if kind==8{Operation::Reserve{plan,cleanup}}else{Operation::Complete{plan,cleanup}}
    };
    proof{
        assert(data.subrange(start as int,r.position() as int) =~= body(op));
        if expected is Some{assert(r.position()==data.len());}
    }
    Ok(op)
}
fn read_identity_body(r:&mut Reader,kind:u32,Ghost(expected):Ghost<Option<Operation>>)->(out:Result<Operation,Status>)
    requires old(r).wf(),kind==3 || kind==4 || kind==5,
        expected is Some ==> operation_tag(expected->Some_0)==kind
            && old(r).data().skip(old(r).position() as int)==body(expected->Some_0),
    ensures final(r).wf(),final(r).data()==old(r).data(),old(r).position()<=final(r).position()<=final(r).data().len(),
        out is Ok ==> operation_tag(out->Ok_0)==kind
            && final(r).data().subrange(old(r).position() as int,final(r).position() as int)==body(out->Ok_0),
        expected is Some ==> out is Ok && final(r).position()==final(r).data().len(),
{
    hide(word32);hide(word64);hide(plan_wire);hide(range_wire);hide(txn_wire);hide(grant_wire);hide(tables_wire);
    hide(same_operation);hide(wire);hide(payload);
    let ghost data=r.data();let ghost start=r.position();
    proof{r.position_bound();}
    let ghost e=match expected{Some(op)=>op,None=>Operation::Settled};
    proof{if expected is Some{
        suffix_prefix(data,start,body(e));
        match e{
            Operation::Begin{id,incarnation}=>{prefix_split(data,start,txn_wire(id),word64(incarnation));},
            Operation::Resolve(id)|Operation::Finish(id)=>{assert(wire_prefix(data,start,txn_wire(id)));},
            _=>{assert(false);},
        }
    }}
    let id=match read_txn(r){Ok(v)=>v,Err(e)=>return Err(e)};
    let op=if kind==3{
        let ghost middle=r.position();
        let incarnation=match r.read_u64(){Ok(v)=>v,Err(e)=>return Err(e)};
        proof{join_wire(data,start,middle,r.position(),txn_wire(id),word64(incarnation));}
        Operation::Begin{id,incarnation}
    }else if kind==4{Operation::Resolve(id)}else{Operation::Finish(id)};
    proof{
        assert(data.subrange(start as int,r.position() as int) =~= body(op));
        if expected is Some{assert(r.position()==data.len());}
    }
    Ok(op)
}
fn read_acquire_body(r:&mut Reader,Ghost(expected):Ghost<Option<Operation>>)->(out:Result<Operation,Status>)
    requires old(r).wf(),expected is Some ==> expected->Some_0 is Acquire
        && old(r).data().skip(old(r).position() as int)==body(expected->Some_0),
    ensures final(r).wf(),final(r).data()==old(r).data(),old(r).position()<=final(r).position()<=final(r).data().len(),
        out is Ok ==> out->Ok_0 is Acquire
            && final(r).data().subrange(old(r).position() as int,final(r).position() as int)==body(out->Ok_0),
        expected is Some ==> out is Ok && final(r).position()==final(r).data().len(),
{
    hide(word32);hide(word64);hide(plan_wire);hide(range_wire);hide(txn_wire);hide(grant_wire);hide(tables_wire);
    hide(same_operation);hide(wire);hide(payload);
    let ghost data=r.data();let ghost start=r.position();
    proof{r.position_bound();}
    let ghost e=match expected{Some(op)=>op,None=>Operation::Settled};
    proof{if expected is Some{
        suffix_prefix(data,start,body(e));
        prefix_split(data,start,txn_wire(e->Acquire_id),
            range_wire(e->Acquire_range)+(bool_wire(e->Acquire_point)+grant_wire(e->Acquire_grant)));
    }}
    let id=match read_txn(r){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost range_start=r.position();
    proof{if expected is Some{
        prefix_split(data,range_start,range_wire(e->Acquire_range),bool_wire(e->Acquire_point)+grant_wire(e->Acquire_grant));
        assert(wire_prefix(data,range_start,range_wire(e->Acquire_range)));
    }}
    let range=match read_range(r){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost point_start=r.position();
    proof{
        join_wire(data,start,range_start,point_start,txn_wire(id),range_wire(range));
        if expected is Some{
            assert(point_start==range_start+range_wire(e->Acquire_range).len());
            prefix_split(data,point_start,bool_wire(e->Acquire_point),grant_wire(e->Acquire_grant));
        }
    }
    let point=match read_bool(r){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost grant_start=r.position();
    proof{join_wire(data,start,point_start,grant_start,txn_wire(id)+range_wire(range),bool_wire(point));}
    let grant=match read_grant(r){Ok(v)=>v,Err(e)=>return Err(e)};
    let op=Operation::Acquire{id,range,point,grant};
    proof{
        join_wire(data,start,grant_start,r.position(),txn_wire(id)+range_wire(op->Acquire_range)+bool_wire(point),grant_wire(grant));
        assert(data.subrange(start as int,r.position() as int) =~= body(op));
        if expected is Some{assert(r.position()==data.len());}
    }
    Ok(op)
}
fn read_release_body(r:&mut Reader,Ghost(expected):Ghost<Option<Operation>>)->(out:Result<Operation,Status>)
    requires old(r).wf(),expected is Some ==> expected->Some_0 is Release
        && old(r).data().skip(old(r).position() as int)==body(expected->Some_0),
    ensures final(r).wf(),final(r).data()==old(r).data(),old(r).position()<=final(r).position()<=final(r).data().len(),
        out is Ok ==> out->Ok_0 is Release
            && final(r).data().subrange(old(r).position() as int,final(r).position() as int)==body(out->Ok_0),
        expected is Some ==> out is Ok && final(r).position()==final(r).data().len(),
{
    hide(word32);hide(word64);hide(plan_wire);hide(range_wire);hide(txn_wire);hide(grant_wire);hide(tables_wire);
    hide(same_operation);hide(wire);hide(payload);
    let ghost data=r.data();let ghost start=r.position();
    proof{r.position_bound();}
    let ghost e=match expected{Some(op)=>op,None=>Operation::Settled};
    proof{if expected is Some{
        suffix_prefix(data,start,body(e));
        prefix_split(data,start,txn_wire(e->Release_id),word64(e->Release_table)+blob(e->Release_coordinate@));
    }}
    let id=match read_txn(r){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost table_start=r.position();
    proof{if expected is Some{
        prefix_split(data,table_start,word64(e->Release_table),blob(e->Release_coordinate@));
    }}
    let table=match r.read_u64(){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost coordinate_start=r.position();
    proof{join_wire(data,start,table_start,coordinate_start,txn_wire(id),word64(table));}
    let coordinate=match r.read_bytes(){Ok(v)=>v,Err(e)=>return Err(e)};
    let op=Operation::Release{id,table,coordinate};
    proof{
        join_wire(data,start,coordinate_start,r.position(),txn_wire(id)+word64(table),blob(op->Release_coordinate@));
        assert(data.subrange(start as int,r.position() as int) =~= body(op));
        if expected is Some{assert(r.position()==data.len());}
    }
    Ok(op)
}
pub(crate) fn decode(bytes:&[u8])->(out:Result<Operation,Status>)
    ensures out is Ok ==> wire(out->Ok_0)==bytes@,
        forall|expected:Operation|encodable(expected) && bytes@==wire(expected) ==> out is Ok && same_operation(out->Ok_0,expected),
{
    hide(word32);hide(word64);hide(plan_wire);hide(range_wire);hide(txn_wire);hide(grant_wire);hide(tables_wire);
    hide(body);hide(encodable);hide(same_operation);
    let mut r=Reader::new(bytes);
    let ghost has_expected=exists|expected:Operation|encodable(expected) && bytes@==wire(expected);
    let ghost expected=if has_expected{
        choose|expected:Operation|encodable(expected) && bytes@==wire(expected)
    }else{Operation::Settled};
    proof{if has_expected{
        assert(bytes@.skip(0) =~= bytes@);
        suffix_prefix(bytes@,0,wire(expected));
        prefix_split(bytes@,0,word64(TAG),payload(expected));
    }}
    let tag=match r.read_u64(){Ok(v)=>v,Err(e)=>return Err(e)};
    if tag!=TAG{return Err(Status::Invalid);}
    let ghost payload_start=r.position();
    proof{if has_expected{prefix_split(bytes@,r.position(),word32(operation_tag(expected)),body(expected));}}
    let kind=match r.read_u32(){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost body_start=r.position();
    let ghost body_expected=if has_expected{Some(expected)}else{None};
    proof{if has_expected{
        assert(bytes@.skip(body_start as int) =~= body(expected));
        assert(operation_tag(expected)==kind);
    }}
    let op=match kind{
        0=>match read_bootstrap(&mut r,Ghost(body_expected)){Ok(v)=>v,Err(e)=>return Err(e)},
        1|2|8|9=>match read_planned_body(&mut r,kind,Ghost(body_expected)){Ok(v)=>v,Err(e)=>return Err(e)},
        3|4|5=>match read_identity_body(&mut r,kind,Ghost(body_expected)){Ok(v)=>v,Err(e)=>return Err(e)},
        6=>match read_acquire_body(&mut r,Ghost(body_expected)){Ok(v)=>v,Err(e)=>return Err(e)},
        7=>match read_release_body(&mut r,Ghost(body_expected)){Ok(v)=>v,Err(e)=>return Err(e)},
        10=>{
            proof{
                reveal(body);
                if has_expected{assert(r.position()==bytes@.len());}
                assert(bytes@.subrange(body_start as int,r.position() as int) =~= body(Operation::Settled));
            }
            Operation::Settled
        },
        _=>return Err(Status::Invalid),
    };
    proof{if has_expected{assert(r.position()==bytes@.len());}}
    if !matches!(r.finish(),Status::Ok){return Err(Status::Invalid);}
    proof{
        join_wire(bytes@,payload_start,body_start,r.position(),word32(kind),body(op));
        assert(bytes@.subrange(payload_start as int,r.position() as int) =~= payload(op));
        join_wire(bytes@,0,payload_start,r.position(),word64(TAG),payload(op));
        assert forall|candidate:Operation|encodable(candidate) && bytes@==wire(candidate) implies same_operation(op,candidate) by {
            operation_unique(op,candidate);
        }
    }
    Ok(op)
}
/// Recovery work scanning needs only fixed-width Begin fields, not allocations
/// for the immutable plans carried by unrelated, already-validated records.
pub(super) fn decode_begin(bytes:&[u8])->(out:Result<Option<(TxnId,u64)>,Status>)
    ensures out is Ok ==> match out->Ok_0{
        Some((id,incarnation))=>bytes@==wire(Operation::Begin{id,incarnation}),
        None=>forall|id:TxnId,incarnation:u64|bytes@!=wire(Operation::Begin{id,incarnation}),
    },
{
    hide(word32);hide(word64);hide(txn_wire);
    let mut r=Reader::new(bytes);
    let tag=match r.read_u64(){Ok(v)=>v,Err(e)=>return Err(e)};
    if tag!=TAG{return Err(Status::Invalid);}
    let kind=match r.read_u32(){Ok(v)=>v,Err(e)=>return Err(e)};
    proof{join_wire(bytes@,0,8,r.position(),word64(TAG),word32(kind));}
    if kind>10{return Err(Status::Invalid);}
    if kind!=3{
        proof{
            assert forall|id:TxnId,incarnation:u64|bytes@!=wire(Operation::Begin{id,incarnation}) by{
                if bytes@==wire(Operation::Begin{id,incarnation}){
                    assert(word32(kind)==bytes@.subrange(8,12));
                    assert(bytes@.skip(8)==word32(3)+(txn_wire(id)+word64(incarnation)));
                    assert(bytes@.skip(8)==word32(kind)+bytes@.skip(12));
                    u32_prefix_unique(kind,3,bytes@.skip(12),txn_wire(id)+word64(incarnation));
                }
            }
        }
        return Ok(None);
    }
    let id=match read_txn(&mut r){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost end_txn=r.position();
    proof{join_wire(bytes@,0,12,end_txn,word64(TAG)+word32(3),txn_wire(id));}
    let incarnation=match r.read_u64(){Ok(v)=>v,Err(e)=>return Err(e)};
    if !matches!(r.finish(),Status::Ok){return Err(Status::Invalid);}
    proof{
        join_wire(bytes@,0,end_txn,r.position(),word64(TAG)+word32(3)+txn_wire(id),word64(incarnation));
        assert(bytes@ =~= wire(Operation::Begin{id,incarnation}));
    }
    Ok(Some((id,incarnation)))
}
pub(super) fn encode_completion(plan:&MigrationPlan,cleanup:bool)->(out:Result<Vec<u8>,Status>)
    ensures out is Ok ==> out->Ok_0@==wire(Operation::Complete{plan:*plan,cleanup}),
{
    hide(word32);hide(word64);hide(plan_wire);
    let mut w=Writer::new();
    let s=w.write_u64(TAG);if !matches!(s,Status::Ok){return Err(s);}
    let s=w.write_u32(9);if !matches!(s,Status::Ok){return Err(s);}
    let s=write_plan(&mut w,plan);if !matches!(s,Status::Ok){return Err(s);}
    let s=write_bool(&mut w,cleanup);if !matches!(s,Status::Ok){return Err(s);}
    proof{assert(w.view() =~= wire(Operation::Complete{plan:*plan,cleanup}));}
    Ok(w.into_bytes())
}
pub(super) fn encode_reservation(plan:&MigrationPlan,cleanup:bool)->(out:Result<Vec<u8>,Status>)
    ensures out is Ok ==> out->Ok_0@==wire(Operation::Reserve{plan:*plan,cleanup})
        && !super::history::is_completion(out->Ok_0@),
{
    hide(word32);hide(word64);hide(plan_wire);
    let mut w=Writer::new();
    let s=w.write_u64(TAG);if !matches!(s,Status::Ok){return Err(s);}
    let s=w.write_u32(8);if !matches!(s,Status::Ok){return Err(s);}
    let s=write_plan(&mut w,plan);if !matches!(s,Status::Ok){return Err(s);}
    let s=write_bool(&mut w,cleanup);if !matches!(s,Status::Ok){return Err(s);}
    proof{assert(w.view() =~= wire(Operation::Reserve{plan:*plan,cleanup}));}
    proof{super::history::not_completion(Operation::Reserve{plan:*plan,cleanup});}
    Ok(w.into_bytes())
}
pub(super) fn encode_delivery(plan:&MigrationPlan,command:Command)->(out:Result<Vec<u8>,Status>)
    ensures out is Ok ==> out->Ok_0@==wire(Operation::Deliver{plan:*plan,command})
        && !super::history::is_completion(out->Ok_0@),
{
    hide(word32);hide(word64);hide(plan_wire);
    let mut w=Writer::new();
    let s=w.write_u64(TAG);if !matches!(s,Status::Ok){return Err(s);}
    let s=w.write_u32(2);if !matches!(s,Status::Ok){return Err(s);}
    let s=write_plan(&mut w,plan);if !matches!(s,Status::Ok){return Err(s);}
    let s=write_command(&mut w,command);if !matches!(s,Status::Ok){return Err(s);}
    proof{assert(w.view() =~= wire(Operation::Deliver{plan:*plan,command}));}
    proof{super::history::not_completion(Operation::Deliver{plan:*plan,command});}
    Ok(w.into_bytes())
}
pub open spec fn same_operation(a:Operation,b:Operation)->bool{
    match (a,b){
        (Operation::Bootstrap{owner:x,tables:p},Operation::Bootstrap{owner:y,tables:q})=>x==y && p.len()==q.len()
            && forall|i:int|0<=i<p.len() ==> p@[i].table==q@[i].table && crate::directory_proofs::equivalent(p@[i].boundaries@,q@[i].boundaries@),
        (Operation::Bind(p),Operation::Bind(q))=>crate::storage::same_plan(p,q),
        (Operation::Deliver{plan:p,command:c},Operation::Deliver{plan:q,command:d})=>crate::storage::same_plan(p,q)&&c==d,
        (Operation::Begin{id:x,incarnation:c},Operation::Begin{id:y,incarnation:d})=>x==y&&c==d,
        (Operation::Resolve(x),Operation::Resolve(y))|(Operation::Finish(x),Operation::Finish(y))=>x==y,
        (Operation::Acquire{id:x,range:p,point:c,grant:g},Operation::Acquire{id:y,range:q,point:d,grant:h})=>
            x==y&&range_same(p,q)&&c==d&&g==h,
        (Operation::Release{id:x,table:t,coordinate:p},Operation::Release{id:y,table:u,coordinate:q})=>x==y&&t==u&&p@==q@,
        (Operation::Reserve{plan:p,cleanup:c},Operation::Reserve{plan:q,cleanup:d})=>crate::storage::same_plan(p,q)&&c==d,
        (Operation::Complete{plan:p,cleanup:c},Operation::Complete{plan:q,cleanup:d})=>crate::storage::same_plan(p,q)&&c==d,
        (Operation::Settled,Operation::Settled)=>true,_=>false,
    }
}
pub(crate) proof fn operation_unique(a:Operation,b:Operation)
    requires wire(a)==wire(b),ensures same_operation(a,b),
{
    hide(word32);hide(word64);hide(plan_wire);hide(range_wire);hide(txn_wire);hide(grant_wire);hide(tables_wire);
    u64_prefix_unique(TAG,TAG,payload(a),payload(b));
    u32_prefix_unique(operation_tag(a),operation_tag(b),body(a),body(b));
    match (a,b){
        (Operation::Bootstrap{owner:x,tables:p},Operation::Bootstrap{owner:y,tables:q})=>{
            u32_prefix_unique(x,y,word64(p.len() as u64)+tables_wire(p@),word64(q.len() as u64)+tables_wire(q@));
            u64_prefix_unique(p.len() as u64,q.len() as u64,tables_wire(p@),tables_wire(q@));
            crate::routing_codec_proofs::tables_unique(p@,q@);
        }
        (Operation::Bind(p),Operation::Bind(q))=>{plan_prefix_unique(p,q,Seq::empty(),Seq::empty());}
        (Operation::Deliver{plan:p,command:c},Operation::Deliver{plan:q,command:d})=>{
            plan_prefix_unique(p,q,word32(command_code(c)),word32(command_code(d)));
            crate::routing_codec_proofs::word32_injective(command_code(c),command_code(d));
            match c{Command::Start=>{},Command::Freeze=>{},Command::Final=>{},Command::Retire=>{},Command::Commit=>{},Command::Abort=>{}}
        }
        (Operation::Begin{id:x,incarnation:c},Operation::Begin{id:y,incarnation:d})=>{
            txn_prefix_unique(x,y,word64(c),word64(d));crate::routing_codec_proofs::word64_injective(c,d);
        }
        (Operation::Resolve(x),Operation::Resolve(y))|(Operation::Finish(x),Operation::Finish(y))=>{
            txn_prefix_unique(x,y,Seq::empty(),Seq::empty());
        }
        (Operation::Acquire{id:x,range:p,point:c,grant:g},Operation::Acquire{id:y,range:q,point:d,grant:h})=>{
            assert(body(a) =~= txn_wire(x)+(range_wire(p)+(bool_wire(c)+grant_wire(g))));
            assert(body(b) =~= txn_wire(y)+(range_wire(q)+(bool_wire(d)+grant_wire(h))));
            txn_prefix_unique(x,y,range_wire(p)+(bool_wire(c)+grant_wire(g)),range_wire(q)+(bool_wire(d)+grant_wire(h)));
            range_prefix_unique(p,q,bool_wire(c)+grant_wire(g),bool_wire(d)+grant_wire(h));
            u32_prefix_unique(if c{1}else{0},if d{1}else{0},grant_wire(g),grant_wire(h));
            grant_prefix_unique(g,h,Seq::empty(),Seq::empty());
        }
        (Operation::Release{id:x,table:t,coordinate:p},Operation::Release{id:y,table:u,coordinate:q})=>{
            assert(p@.len()==p.len());assert(q@.len()==q.len());
            txn_prefix_unique(x,y,word64(t)+blob(p@),word64(u)+blob(q@));
            u64_prefix_unique(t,u,blob(p@),blob(q@));blob_prefix_unique(p@,q@,Seq::empty(),Seq::empty());
        }
        (Operation::Reserve{plan:p,cleanup:c},Operation::Reserve{plan:q,cleanup:d})=>{
            plan_prefix_unique(p,q,bool_wire(c),bool_wire(d));
            u32_prefix_unique(if c{1}else{0},if d{1}else{0},Seq::empty(),Seq::empty());
        }
        (Operation::Complete{plan:p,cleanup:c},Operation::Complete{plan:q,cleanup:d})=>{
            plan_prefix_unique(p,q,bool_wire(c),bool_wire(d));
            u32_prefix_unique(if c{1}else{0},if d{1}else{0},Seq::empty(),Seq::empty());
        }
        (Operation::Settled,Operation::Settled)=>{},
        _=>{assert(operation_tag(a)!=operation_tag(b));}
    }
}
} // verus!
