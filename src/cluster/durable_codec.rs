//! Canonical journal fields built on the checked routing byte codec.
use vstd::prelude::*;
use crate::types::*;
use crate::routing_codec::{Reader,Writer};
#[cfg(verus_keep_ghost)]
use crate::routing_codec_proofs::{word32,word64,blob,boundaries_wire,boundary_wire,table_wire,word32_injective,word64_injective,wire_prefix,prefix_split,join_wire};

verus! {
pub open spec fn txn_wire(id:TxnId)->Seq<u8> { word64(id.client)+word64(id.sequence) }
pub open spec fn grant_wire(g:Grant)->Seq<u8> { word32(g.owner)+word64(g.ownership_version) }
pub open spec fn upper_wire(hi:Option<Vec<u8>>)->Seq<u8> {
    match hi { None=>word32(0),Some(h)=>word32(1)+blob(h@) }
}
pub open spec fn range_same(a:KeyRange,b:KeyRange)->bool {
    a.table==b.table && a.lo@==b.lo@ && match (a.hi,b.hi) {
        (None,None)=>true,(Some(x),Some(y))=>x@==y@,_=>false,
    }
}
/// Turn an exact remaining byte sequence into a decoder prefix and its end.
pub proof fn suffix_prefix(data:Seq<u8>,position:nat,wire:Seq<u8>)
    requires position<=data.len(),data.skip(position as int)==wire,
    ensures wire_prefix(data,position,wire),position+wire.len()==data.len(),
{
    assert(data.subrange(position as int,data.len() as int) =~= data.skip(position as int));
}
pub proof fn txn_prefix_unique(a:TxnId,b:TxnId,x:Seq<u8>,y:Seq<u8>)
    requires txn_wire(a)+x==txn_wire(b)+y,
    ensures a==b,x==y,
{
    assert((txn_wire(a)+x).take(8)==word64(a.client));
    assert((txn_wire(b)+y).take(8)==word64(b.client));
    word64_injective(a.client,b.client);
    assert((txn_wire(a)+x).subrange(8,16)==word64(a.sequence));
    assert((txn_wire(b)+y).subrange(8,16)==word64(b.sequence));
    word64_injective(a.sequence,b.sequence);
    assert((txn_wire(a)+x).skip(16)==x);
    assert((txn_wire(b)+y).skip(16)==y);
}
pub proof fn range_prefix_unique(a:KeyRange,b:KeyRange,x:Seq<u8>,y:Seq<u8>)
    requires range_wire(a)+x==range_wire(b)+y,
    ensures range_same(a,b),x==y,
{
    hide(word32);hide(word64);
    assert(a.lo@.len()==a.lo.len());assert(b.lo@.len()==b.lo.len());
    assert(range_wire(a)+x =~= word64(a.table)+(blob(a.lo@)+(upper_wire(a.hi)+x)));
    assert(range_wire(b)+y =~= word64(b.table)+(blob(b.lo@)+(upper_wire(b.hi)+y)));
    u64_prefix_unique(a.table,b.table,blob(a.lo@)+(upper_wire(a.hi)+x),blob(b.lo@)+(upper_wire(b.hi)+y));
    blob_prefix_unique(a.lo@,b.lo@,upper_wire(a.hi)+x,upper_wire(b.hi)+y);
    match (a.hi,b.hi) {
        (None,None)=>{u32_prefix_unique(0,0,x,y);}
        (Some(p),Some(q))=>{
            assert(p@.len()==p.len());assert(q@.len()==q.len());
            assert(upper_wire(a.hi)+x =~= word32(1)+(blob(p@)+x));
            assert(upper_wire(b.hi)+y =~= word32(1)+(blob(q@)+y));
            u32_prefix_unique(1,1,blob(p@)+x,blob(q@)+y);
            blob_prefix_unique(p@,q@,x,y);
        }
        (None,Some(q))=>{
            assert(upper_wire(b.hi)+y =~= word32(1)+(blob(q@)+y));
            u32_prefix_unique(0,1,x,blob(q@)+y);
        }
        (Some(p),None)=>{
            assert(upper_wire(a.hi)+x =~= word32(1)+(blob(p@)+x));
            u32_prefix_unique(1,0,blob(p@)+x,y);
        }
    }
}
pub open spec fn range_wire(r:KeyRange)->Seq<u8> { word64(r.table)+(blob(r.lo@)+upper_wire(r.hi)) }
pub open spec fn plan_wire(p:MigrationPlan)->Seq<u8> {
    word64(p.generation)+(txn_wire(p.nonce)+(word32(p.source)+(word32(p.destination)
        +(range_wire(p.range)+table_wire(crate::directory::RouteTable{table:p.range.table,boundaries:p.old})))))
}
pub open spec fn command_code(c:Command)->u32 {
    match c {Command::Start=>0,Command::Freeze=>1,Command::Final=>2,Command::Retire=>3,Command::Commit=>4,Command::Abort=>5}
}
pub fn write_txn(w:&mut Writer,id:TxnId)->(s:Status)
    ensures s==Status::Ok ==> final(w).view()==old(w).view()+txn_wire(id),
{
    hide(word64);
    let status=w.write_u64(id.client);if !matches!(status,Status::Ok){return status;}
    let status=w.write_u64(id.sequence);
    proof{if status==Status::Ok{assert(w.view() =~= old(w).view()+txn_wire(id));}}
    status
}
pub fn read_txn(r:&mut Reader)->(out:Result<TxnId,Status>)
    requires old(r).wf(),
    ensures final(r).wf(),final(r).data()==old(r).data(),final(r).position()>=old(r).position(),
        final(r).position()<=final(r).data().len(),
        out is Ok ==> final(r).data().subrange(old(r).position() as int,final(r).position() as int)==txn_wire(out->Ok_0),
        forall|expected:TxnId| wire_prefix(old(r).data(),old(r).position(),txn_wire(expected)) ==>
            out is Ok && out->Ok_0==expected && final(r).position()==old(r).position()+txn_wire(expected).len(),
{
    hide(word64);
    let ghost start=r.position();let ghost data=r.data();
    let ghost has_expected=exists|expected:TxnId|wire_prefix(data,start,txn_wire(expected));
    let ghost expected=if has_expected{choose|expected:TxnId|wire_prefix(data,start,txn_wire(expected))}else{TxnId{client:0,sequence:0}};
    proof{if has_expected{prefix_split(data,start,word64(expected.client),word64(expected.sequence));}}
    let client=match r.read_u64(){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost middle=r.position();
    let sequence=match r.read_u64(){Ok(v)=>v,Err(e)=>return Err(e)};
    let id=TxnId{client,sequence};
    proof{join_wire(data,start,middle,r.position(),word64(client),word64(sequence));}
    proof{assert forall|e:TxnId|wire_prefix(old(r).data(),start,txn_wire(e)) implies id==e by {
        let rest=r.data().skip(start as int);
        assert(rest==txn_wire(id)+r.data().skip(r.position() as int));
        assert(rest==txn_wire(e)+rest.skip(txn_wire(e).len() as int));
        txn_prefix_unique(id,e,r.data().skip(r.position() as int),rest.skip(txn_wire(e).len() as int));
    }}
    Ok(id)
}
pub fn write_grant(w:&mut Writer,g:Grant)->(s:Status)
    ensures s==Status::Ok ==> final(w).view()==old(w).view()+grant_wire(g),
{
    hide(word32);hide(word64);
    let status=w.write_u32(g.owner);if !matches!(status,Status::Ok){return status;}
    let status=w.write_u64(g.ownership_version);
    proof{if status==Status::Ok{assert(w.view() =~= old(w).view()+grant_wire(g));}}
    status
}
pub fn read_grant(r:&mut Reader)->(out:Result<Grant,Status>)
    requires old(r).wf(),
    ensures final(r).wf(),final(r).data()==old(r).data(),final(r).position()>=old(r).position(),
        final(r).position()<=final(r).data().len(),
        out is Ok ==> final(r).data().subrange(old(r).position() as int,final(r).position() as int)==grant_wire(out->Ok_0),
        forall|expected:Grant|wire_prefix(old(r).data(),old(r).position(),grant_wire(expected)) ==>
            out is Ok && out->Ok_0==expected && final(r).position()==old(r).position()+grant_wire(expected).len(),
{
    hide(word32);hide(word64);
    let ghost start=r.position();let ghost data=r.data();
    let ghost has_expected=exists|expected:Grant|wire_prefix(data,start,grant_wire(expected));
    let ghost expected=choose|expected:Grant|wire_prefix(data,start,grant_wire(expected));
    proof{if has_expected{prefix_split(data,start,word32(expected.owner),word64(expected.ownership_version));}}
    let owner=match r.read_u32(){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost middle=r.position();
    let ownership_version=match r.read_u64(){Ok(v)=>v,Err(e)=>return Err(e)};
    let g=Grant{owner,ownership_version};
    proof{join_wire(data,start,middle,r.position(),word32(owner),word64(ownership_version));}
    proof{
        assert forall|expected:Grant|wire_prefix(data,start,grant_wire(expected)) implies g==expected by {
            let rest=data.skip(start as int);
            assert(rest==grant_wire(g)+data.skip(r.position() as int));
            assert(rest==grant_wire(expected)+rest.skip(grant_wire(expected).len() as int));
            grant_prefix_unique(g,expected,data.skip(r.position() as int),rest.skip(grant_wire(expected).len() as int));
        }
    }
    Ok(g)
}
pub fn write_range(w:&mut Writer,r:&KeyRange)->(s:Status)
    ensures s==Status::Ok ==> final(w).view()==old(w).view()+range_wire(*r),
{
    hide(word32);hide(word64);
    let status=w.write_u64(r.table);if !matches!(status,Status::Ok){return status;}
    let status=w.write_bytes(&r.lo);if !matches!(status,Status::Ok){return status;}
    let status=match &r.hi {
        None=>w.write_u32(0),
        Some(h)=>{let status=w.write_u32(1);if !matches!(status,Status::Ok){return status;}w.write_bytes(h)}
    };
    proof{if status==Status::Ok{assert(w.view() =~= old(w).view()+range_wire(*r));}}
    status
}
pub fn read_range(r:&mut Reader)->(out:Result<KeyRange,Status>)
    requires old(r).wf(),
    ensures final(r).wf(),final(r).data()==old(r).data(),final(r).position()>=old(r).position(),
        final(r).position()<=final(r).data().len(),
        out is Ok ==> final(r).data().subrange(old(r).position() as int,final(r).position() as int)==range_wire(out->Ok_0),
        forall|expected:KeyRange| #[trigger] wire_prefix(old(r).data(),old(r).position(),range_wire(expected)) ==>
            out is Ok && range_same(out->Ok_0,expected) && range_wire(out->Ok_0)==range_wire(expected)
                && final(r).position()==old(r).position()+range_wire(expected).len(),
{
    hide(word32);hide(word64);
    let ghost start=r.position();let ghost data=r.data();
    let ghost has_expected=exists|expected:KeyRange|wire_prefix(data,start,range_wire(expected));
    let ghost expected=choose|expected:KeyRange|has_expected ==> wire_prefix(data,start,range_wire(expected));
    proof{if has_expected{
        assert(range_wire(expected) =~= word64(expected.table)+(blob(expected.lo@)+upper_wire(expected.hi)));
        prefix_split(data,start,word64(expected.table),blob(expected.lo@)+upper_wire(expected.hi));
    }}
    let table=match r.read_u64(){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost lo_start=r.position();
    proof{if has_expected{prefix_split(data,r.position(),blob(expected.lo@),upper_wire(expected.hi));}}
    let lo=match r.read_bytes(){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost upper_start=r.position();
    proof{join_wire(data,start,lo_start,upper_start,word64(table),blob(lo@));}
    proof{if has_expected{match expected.hi{
        None=>{},
        Some(h)=>{prefix_split(data,r.position(),word32(1),blob(h@));},
    }}}
    let tag=match r.read_u32(){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost body_start=r.position();
    let hi=match tag{
        0=>None,
        1=>{
            let h=match r.read_bytes(){Ok(v)=>v,Err(e)=>return Err(e)};
            proof{join_wire(data,upper_start,body_start,r.position(),word32(1),blob(h@));}
            Some(h)
        },
        _=>return Err(Status::Invalid),
    };
    let range=KeyRange{table,lo,hi};
    proof{join_wire(data,start,upper_start,r.position(),word64(table)+blob(range.lo@),upper_wire(range.hi));}
    proof{assert forall|e:KeyRange|#[trigger] wire_prefix(old(r).data(),start,range_wire(e)) implies range_same(range,e)
        && range_wire(range)==range_wire(e) && r.position()==start+range_wire(e).len() by {
        let rest=r.data().skip(start as int);
        assert(rest==range_wire(range)+r.data().skip(r.position() as int));
        assert(rest==range_wire(e)+rest.skip(range_wire(e).len() as int));
        range_prefix_unique(range,e,r.data().skip(r.position() as int),rest.skip(range_wire(e).len() as int));
        assert(range_wire(range)==range_wire(e)) by{match (range.hi,e.hi){(Some(x),Some(y))=>{},_=>{}}}
    }}
    Ok(range)
}
pub fn write_command(w:&mut Writer,c:Command)->(s:Status)
    ensures s==Status::Ok ==> final(w).view()==old(w).view()+word32(command_code(c)),
{
    w.write_u32(match c {Command::Start=>0,Command::Freeze=>1,Command::Final=>2,Command::Retire=>3,Command::Commit=>4,Command::Abort=>5})
}
pub fn read_command(r:&mut Reader)->(out:Result<Command,Status>)
    requires old(r).wf(),
    ensures final(r).wf(),final(r).data()==old(r).data(),final(r).position()>=old(r).position(),
        final(r).position()<=final(r).data().len(),
        out is Ok ==> final(r).data().subrange(old(r).position() as int,final(r).position() as int)==word32(command_code(out->Ok_0)),
        forall|expected:Command|wire_prefix(old(r).data(),old(r).position(),word32(command_code(expected))) ==>
            out is Ok && out->Ok_0==expected && final(r).position()==old(r).position()+4,
{
    match r.read_u32(){Ok(0)=>Ok(Command::Start),Ok(1)=>Ok(Command::Freeze),Ok(2)=>Ok(Command::Final),
        Ok(3)=>Ok(Command::Retire),Ok(4)=>Ok(Command::Commit),Ok(5)=>Ok(Command::Abort),_=>Err(Status::Invalid)}
}
pub fn write_plan(w:&mut Writer,p:&MigrationPlan)->(s:Status)
    ensures s==Status::Ok ==> final(w).view()==old(w).view()+plan_wire(*p),
{
    hide(word32);hide(word64);
    let status=w.write_u64(p.generation);if !matches!(status,Status::Ok){return status;}
    let status=write_txn(w,p.nonce);if !matches!(status,Status::Ok){return status;}
    let status=w.write_u32(p.source);if !matches!(status,Status::Ok){return status;}
    let status=w.write_u32(p.destination);if !matches!(status,Status::Ok){return status;}
    let status=write_range(w,&p.range);if !matches!(status,Status::Ok){return status;}
    let status=w.write_u64(p.range.table);if !matches!(status,Status::Ok){return status;}
    let status=w.write_u64(p.old.len() as u64);if !matches!(status,Status::Ok){return status;}
    let ghost prefix=w.view();
    let mut i=0usize;
    while i<p.old.len()
        invariant i<=p.old.len(),w.view()==prefix+boundaries_wire(p.old@.take(i as int)),
        decreases p.old.len()-i,
    {
        let status=w.write_bytes(&p.old[i].start);if !matches!(status,Status::Ok){return status;}
        let status=write_grant(w,p.old[i].grant);if !matches!(status,Status::Ok){return status;}
        i+=1;
        proof{
            assert(p.old@.take(i as int).drop_last()==p.old@.take(i as int-1));
            assert(p.old@.take(i as int).last()==p.old@[i as int-1]);
            assert(w.view() =~= prefix+boundaries_wire(p.old@.take(i as int)));
        }
    }
    proof{assert(p.old@.take(p.old.len() as int)==p.old@);assert(w.view() =~= old(w).view()+plan_wire(*p));}
    Status::Ok
}
pub fn read_plan(r:&mut Reader)->(out:Result<MigrationPlan,Status>)
    requires old(r).wf(),
    ensures final(r).wf(),final(r).data()==old(r).data(),final(r).position()>=old(r).position(),
        final(r).position()<=final(r).data().len(),
        out is Ok ==> final(r).data().subrange(old(r).position() as int,final(r).position() as int)==plan_wire(out->Ok_0),
        forall|expected:MigrationPlan|expected.old.len()>0 && #[trigger] wire_prefix(old(r).data(),old(r).position(),plan_wire(expected)) ==>
            out is Ok && crate::storage::same_plan(out->Ok_0,expected) && plan_wire(out->Ok_0)==plan_wire(expected)
                && final(r).position()==old(r).position()+plan_wire(expected).len(),
{
    hide(word32);hide(word64);hide(boundaries_wire);hide(range_wire);hide(txn_wire);
    let ghost start=r.position();let ghost data=r.data();
    let ghost has_expected=exists|expected:MigrationPlan|expected.old.len()>0 && wire_prefix(data,start,plan_wire(expected));
    let ghost expected=if has_expected{
        choose|expected:MigrationPlan|expected.old.len()>0 && wire_prefix(data,start,plan_wire(expected))
    }else{arbitrary()};
    let ghost expected_table=crate::directory::RouteTable{table:expected.range.table,boundaries:expected.old};
    let ghost expected_tail=range_wire(expected.range)+table_wire(expected_table);
    proof{if has_expected{prefix_split(data,start,word64(expected.generation),
        txn_wire(expected.nonce)+(word32(expected.source)+(word32(expected.destination)+expected_tail)));}}
    let generation=match r.read_u64(){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost nonce_start=r.position();
    proof{if has_expected{prefix_split(data,r.position(),txn_wire(expected.nonce),
        word32(expected.source)+(word32(expected.destination)+expected_tail));}}
    let nonce=match read_txn(r){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost source_start=r.position();
    proof{join_wire(data,start,nonce_start,source_start,word64(generation),txn_wire(nonce));}
    proof{if has_expected{prefix_split(data,r.position(),word32(expected.source),word32(expected.destination)+expected_tail);}}
    let source=match r.read_u32(){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost destination_start=r.position();
    proof{join_wire(data,start,source_start,destination_start,word64(generation)+txn_wire(nonce),word32(source));}
    proof{if has_expected{prefix_split(data,r.position(),word32(expected.destination),expected_tail);}}
    let destination=match r.read_u32(){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost range_start=r.position();let ghost header=word64(generation)+txn_wire(nonce)+word32(source)+word32(destination);
    proof{join_wire(data,start,destination_start,range_start,word64(generation)+txn_wire(nonce)+word32(source),word32(destination));}
    proof{if has_expected{
        prefix_split(data,r.position(),range_wire(expected.range),table_wire(expected_table));
        assert(wire_prefix(data,range_start,range_wire(expected.range)));
    }}
    let range=match read_range(r){Ok(v)=>v,Err(e)=>return Err(e)};
    let ghost table_start=r.position();
    proof{if has_expected{
        assert(range_same(range,expected.range));
        assert(range_wire(range)==range_wire(expected.range));
        assert(table_start==range_start+range_wire(expected.range).len());
    }}
    proof{join_wire(data,start,range_start,table_start,header,range_wire(range));}
    proof{if has_expected{
        assert(expected_table.boundaries.len()>0);
        assert(wire_prefix(data,table_start,table_wire(expected_table)));
    }}
    let old=match r.read_table(){Ok(v)=>v,Err(e)=>return Err(e)};
    proof{join_wire(data,start,table_start,r.position(),header+range_wire(range),table_wire(old));}
    proof{if has_expected{
        assert(table_wire(old)==table_wire(expected_table));
        crate::routing_codec_proofs::table_prefix_unique(old,expected_table,Seq::empty(),Seq::empty());
    }}
    if old.table!=range.table{return Err(Status::Invalid);}
    let p=MigrationPlan{generation,nonce,source,destination,range,old:old.boundaries};
    proof{assert(data.subrange(start as int,r.position() as int) =~= plan_wire(p));}
    proof{
        assert forall|expected:MigrationPlan|expected.old.len()>0 && #[trigger] wire_prefix(data,start,plan_wire(expected))
            implies crate::storage::same_plan(p,expected) && plan_wire(p)==plan_wire(expected)
                && r.position()==start+plan_wire(expected).len() by{
            let rest=data.skip(start as int);
            assert(rest==plan_wire(p)+data.skip(r.position() as int));
            assert(rest==plan_wire(expected)+rest.skip(plan_wire(expected).len() as int));
            plan_prefix_unique(p,expected,data.skip(r.position() as int),rest.skip(plan_wire(expected).len() as int));
            assert(plan_wire(p) =~= plan_wire(expected));
        }
    }
    Ok(p)
}
pub proof fn plan_prefix_unique(a:MigrationPlan,b:MigrationPlan,x:Seq<u8>,y:Seq<u8>)
    requires plan_wire(a)+x==plan_wire(b)+y,
    ensures crate::storage::same_plan(a,b),x==y,
{
    hide(word32);hide(word64);hide(boundaries_wire);hide(range_wire);hide(txn_wire);
    let at=crate::directory::RouteTable{table:a.range.table,boundaries:a.old};
    let bt=crate::directory::RouteTable{table:b.range.table,boundaries:b.old};
    let ar=range_wire(a.range)+(table_wire(at)+x);let br=range_wire(b.range)+(table_wire(bt)+y);
    let ad=word32(a.destination)+ar;let bd=word32(b.destination)+br;
    let ast=word32(a.source)+ad;let bst=word32(b.source)+bd;
    let an=txn_wire(a.nonce)+ast;let bn=txn_wire(b.nonce)+bst;
    assert(plan_wire(a)+x =~= word64(a.generation)+an);
    assert(plan_wire(b)+y =~= word64(b.generation)+bn);
    u64_prefix_unique(a.generation,b.generation,an,bn);
    txn_prefix_unique(a.nonce,b.nonce,ast,bst);
    u32_prefix_unique(a.source,b.source,ad,bd);
    u32_prefix_unique(a.destination,b.destination,ar,br);
    range_prefix_unique(a.range,b.range,table_wire(at)+x,table_wire(bt)+y);
    crate::routing_codec_proofs::table_prefix_unique(at,bt,x,y);
}
pub proof fn u64_prefix_unique(a:u64,b:u64,x:Seq<u8>,y:Seq<u8>)
    requires word64(a)+x==word64(b)+y,ensures a==b,x==y,
{
    assert((word64(a)+x).take(8)==word64(a));assert((word64(b)+y).take(8)==word64(b));
    word64_injective(a,b);
    assert((word64(a)+x).skip(8)==x);assert((word64(b)+y).skip(8)==y);
}
pub proof fn u32_prefix_unique(a:u32,b:u32,x:Seq<u8>,y:Seq<u8>)
    requires word32(a)+x==word32(b)+y,ensures a==b,x==y,
{
    assert((word32(a)+x).take(4)==word32(a));assert((word32(b)+y).take(4)==word32(b));
    word32_injective(a,b);
    assert((word32(a)+x).skip(4)==x);assert((word32(b)+y).skip(4)==y);
}
pub proof fn blob_prefix_unique(a:Seq<u8>,b:Seq<u8>,x:Seq<u8>,y:Seq<u8>)
    requires a.len()<=u64::MAX,b.len()<=u64::MAX,blob(a)+x==blob(b)+y,ensures a==b,x==y,
{
    hide(word64);
    assert(blob(a)+x =~= word64(a.len() as u64)+(a+x));
    assert(blob(b)+y =~= word64(b.len() as u64)+(b+y));
    u64_prefix_unique(a.len() as u64,b.len() as u64,a+x,b+y);
    assert((a+x).take(a.len() as int)==a);assert((b+y).take(b.len() as int)==b);
    assert((a+x).skip(a.len() as int)==x);assert((b+y).skip(b.len() as int)==y);
}
pub proof fn grant_prefix_unique(a:Grant,b:Grant,x:Seq<u8>,y:Seq<u8>)
    requires grant_wire(a)+x==grant_wire(b)+y,ensures a==b,x==y,
{
    hide(word32);hide(word64);
    assert(grant_wire(a)+x =~= word32(a.owner)+(word64(a.ownership_version)+x));
    assert(grant_wire(b)+y =~= word32(b.owner)+(word64(b.ownership_version)+y));
    u32_prefix_unique(a.owner,b.owner,word64(a.ownership_version)+x,word64(b.ownership_version)+y);
    u64_prefix_unique(a.ownership_version,b.ownership_version,x,y);
}
} // verus!
