//! Wire grammar and arithmetic inverses used by the actual Reader and Writer.
#![cfg(verus_keep_ghost)]
use vstd::prelude::*;
use crate::types::Boundary;
use crate::directory::RouteTable;
use crate::directory_proofs::equivalent;
use crate::routing::{Snapshot,same_snapshot};
use crate::routing_codec::SNAPSHOT_TAG;
verus! {
pub open spec fn word64(v: u64) -> Seq<u8> {
    seq![v as u8,(v>>8) as u8,(v>>16) as u8,(v>>24) as u8,
        (v>>32) as u8,(v>>40) as u8,(v>>48) as u8,(v>>56) as u8]
}
pub open spec fn word32(v: u32) -> Seq<u8> {
    seq![v as u8,(v>>8) as u8,(v>>16) as u8,(v>>24) as u8]
}
pub proof fn unpack64(a: u8,b: u8,c: u8,d: u8,e: u8,f: u8,g: u8,h: u8)
    ensures word64((a as u64)|((b as u64)<<8)|((c as u64)<<16)|((d as u64)<<24)
        |((e as u64)<<32)|((f as u64)<<40)|((g as u64)<<48)|((h as u64)<<56)) == seq![a,b,c,d,e,f,g,h],
{
    let v = (a as u64)|((b as u64)<<8)|((c as u64)<<16)|((d as u64)<<24)
        |((e as u64)<<32)|((f as u64)<<40)|((g as u64)<<48)|((h as u64)<<56);
    assert(v as u8 == a && (v>>8) as u8 == b && (v>>16) as u8 == c && (v>>24) as u8 == d
        && (v>>32) as u8 == e && (v>>40) as u8 == f && (v>>48) as u8 == g && (v>>56) as u8 == h) by(bit_vector)
        requires v == ((a as u64)|((b as u64)<<8)|((c as u64)<<16)|((d as u64)<<24)
            |((e as u64)<<32)|((f as u64)<<40)|((g as u64)<<48)|((h as u64)<<56));
}
pub proof fn unpack32(a: u8,b: u8,c: u8,d: u8)
    ensures word32((a as u32)|((b as u32)<<8)|((c as u32)<<16)|((d as u32)<<24)) == seq![a,b,c,d],
{
    let v = (a as u32)|((b as u32)<<8)|((c as u32)<<16)|((d as u32)<<24);
    assert(v as u8 == a && (v>>8) as u8 == b && (v>>16) as u8 == c && (v>>24) as u8 == d) by(bit_vector)
        requires v == ((a as u32)|((b as u32)<<8)|((c as u32)<<16)|((d as u32)<<24));
}
pub proof fn word64_injective(a: u64,b: u64)
    requires word64(a) == word64(b), ensures a == b,
{
    assert(a as u8 == b as u8 && (a>>8) as u8 == (b>>8) as u8 && (a>>16) as u8 == (b>>16) as u8
        && (a>>24) as u8 == (b>>24) as u8 && (a>>32) as u8 == (b>>32) as u8 && (a>>40) as u8 == (b>>40) as u8
        && (a>>48) as u8 == (b>>48) as u8 && (a>>56) as u8 == (b>>56) as u8);
    assert(a == b) by(bit_vector)
        requires a as u8 == b as u8, (a>>8) as u8 == (b>>8) as u8, (a>>16) as u8 == (b>>16) as u8,
            (a>>24) as u8 == (b>>24) as u8, (a>>32) as u8 == (b>>32) as u8, (a>>40) as u8 == (b>>40) as u8,
            (a>>48) as u8 == (b>>48) as u8, (a>>56) as u8 == (b>>56) as u8;
}
pub proof fn word32_injective(a: u32,b: u32)
    requires word32(a) == word32(b), ensures a == b,
{
    assert(a as u8 == b as u8 && (a>>8) as u8 == (b>>8) as u8 && (a>>16) as u8 == (b>>16) as u8 && (a>>24) as u8 == (b>>24) as u8);
    assert(a == b) by(bit_vector)
        requires a as u8 == b as u8, (a>>8) as u8 == (b>>8) as u8, (a>>16) as u8 == (b>>16) as u8, (a>>24) as u8 == (b>>24) as u8;
}
pub open spec fn blob(bytes: Seq<u8>) -> Seq<u8> { word64(bytes.len() as u64)+bytes }
pub open spec fn boundary_wire(b: Boundary) -> Seq<u8> { blob(b.start@)+word32(b.grant.owner)+word64(b.grant.epoch) }
pub open spec fn boundaries_wire(p: Seq<Boundary>) -> Seq<u8>
    decreases p.len(),
{ if p.len() == 0 { Seq::empty() } else { boundaries_wire(p.drop_last())+boundary_wire(p.last()) } }
pub open spec fn table_wire(t: RouteTable) -> Seq<u8> {
    word64(t.table)+word64(t.boundaries.len() as u64)+boundaries_wire(t.boundaries@)
}
pub open spec fn tables_wire(t: Seq<RouteTable>) -> Seq<u8>
    decreases t.len(),
{ if t.len() == 0 { Seq::empty() } else { tables_wire(t.drop_last())+table_wire(t.last()) } }
pub open spec fn snapshot_wire(s: Snapshot) -> Seq<u8> {
    word64(SNAPSHOT_TAG)+word64(s.version)+word64(s.tables.len() as u64)+tables_wire(s.tables@)
}
pub proof fn equal_boundaries_wire(a: Seq<Boundary>,b: Seq<Boundary>)
    requires equivalent(a,b),
    ensures boundaries_wire(a) == boundaries_wire(b),
    decreases a.len(),
{
    if a.len() > 0 {
        assert(equivalent(a.drop_last(),b.drop_last()));
        equal_boundaries_wire(a.drop_last(),b.drop_last());
        assert(a.last().start@ == b.last().start@ && a.last().grant == b.last().grant);
    }
}
pub proof fn equal_table_wire(a: RouteTable,b: RouteTable)
    requires a.table == b.table, equivalent(a.boundaries@,b.boundaries@),
    ensures table_wire(a) == table_wire(b),
{ equal_boundaries_wire(a.boundaries@,b.boundaries@); }
pub open spec fn wire_prefix(data: Seq<u8>,position: nat,wire: Seq<u8>) -> bool {
    position+wire.len() <= data.len() && data.subrange(position as int,(position+wire.len()) as int) == wire
}
pub proof fn prefix_split(data: Seq<u8>,position: nat,a: Seq<u8>,b: Seq<u8>)
    requires wire_prefix(data,position,a+b),
    ensures wire_prefix(data,position,a), wire_prefix(data,position+a.len(),b),
{
    assert(data.subrange(position as int,(position+a.len()) as int) =~= a);
    assert forall|i: int| 0 <= i < b.len() implies
        data[(position+a.len()) as int+i] == b[i] by {
        assert(data.subrange(position as int,(position+a.len()+b.len()) as int)[a.len() as int+i] == (a+b)[a.len() as int+i]);
    }
    assert(data.subrange((position+a.len()) as int,(position+a.len()+b.len()) as int) =~= b);
}
pub proof fn join_wire(data: Seq<u8>,start: nat,middle: nat,end: nat,a: Seq<u8>,b: Seq<u8>)
    requires start <= middle <= end <= data.len(),
        data.subrange(start as int,middle as int) == a,
        data.subrange(middle as int,end as int) == b,
    ensures data.subrange(start as int,end as int) == a+b,
{
    assert forall|i: int| 0 <= i < a.len()+b.len() implies
        data[start as int+i] == (a+b)[i] by {
        if i < a.len() { assert(data.subrange(start as int,middle as int)[i] == a[i]); }
        else { assert(data.subrange(middle as int,end as int)[i-a.len() as int] == b[i-a.len() as int]); }
    }
    assert(data.subrange(start as int,end as int) =~= a+b);
}
pub proof fn boundaries_size(p: Seq<Boundary>)
    ensures boundaries_wire(p).len() >= 20*p.len(),
    decreases p.len(),
{ if p.len() > 0 { boundaries_size(p.drop_last()); } }
pub proof fn tables_size(p: Seq<RouteTable>)
    requires forall|i: int| 0 <= i < p.len() ==> #[trigger] p[i].boundaries.len() > 0,
    ensures tables_wire(p).len() >= 36*p.len(),
    decreases p.len(),
{
    if p.len() > 0 {
        tables_size(p.drop_last()); boundaries_size(p.last().boundaries@);
    }
}
pub proof fn boundaries_head(p: Seq<Boundary>)
    requires p.len() > 0,
    ensures boundaries_wire(p) == boundary_wire(p[0])+boundaries_wire(p.skip(1)),
    decreases p.len(),
{
    reveal_with_fuel(boundaries_wire,2);
    if p.len() > 1 {
        boundaries_head(p.drop_last());
        assert(p.drop_last()[0] == p[0]);
        assert(p.drop_last().skip(1) == p.skip(1).drop_last());
        assert(p.skip(1).last() == p.last());
    } else { assert(p.skip(1) == Seq::<Boundary>::empty()); assert(p.drop_last() == Seq::<Boundary>::empty()); assert(p.last() == p[0]); }
    assert(boundaries_wire(p) =~= boundary_wire(p[0])+boundaries_wire(p.skip(1)));
}
pub proof fn tables_head(p: Seq<RouteTable>)
    requires p.len() > 0,
    ensures tables_wire(p) == table_wire(p[0])+tables_wire(p.skip(1)),
    decreases p.len(),
{
    reveal_with_fuel(tables_wire,2);
    if p.len() > 1 {
        tables_head(p.drop_last());
        assert(p.drop_last()[0] == p[0]);
        assert(p.drop_last().skip(1) == p.skip(1).drop_last());
        assert(p.skip(1).last() == p.last());
    } else { assert(p.skip(1) == Seq::<RouteTable>::empty()); assert(p.drop_last() == Seq::<RouteTable>::empty()); assert(p.last() == p[0]); }
    assert(tables_wire(p) =~= table_wire(p[0])+tables_wire(p.skip(1)));
}
pub proof fn boundary_prefix_unique(a: Boundary,b: Boundary,x: Seq<u8>,y: Seq<u8>)
    requires boundary_wire(a)+x == boundary_wire(b)+y,
    ensures a.start@ == b.start@, a.grant == b.grant, x == y,
{
    assert(word64(a.start.len() as u64) == word64(b.start.len() as u64)) by {
        assert((boundary_wire(a)+x).take(8) == word64(a.start.len() as u64));
        assert((boundary_wire(b)+y).take(8) == word64(b.start.len() as u64));
    }
    word64_injective(a.start.len() as u64,b.start.len() as u64);
    let n = a.start.len() as int;
    assert(a.start@ == b.start@) by {
        assert((boundary_wire(a)+x).subrange(8,8+n) == a.start@);
        assert((boundary_wire(b)+y).subrange(8,8+n) == b.start@);
    }
    assert(word32(a.grant.owner) == word32(b.grant.owner)) by {
        assert((boundary_wire(a)+x).subrange(8+n,12+n) == word32(a.grant.owner));
        assert((boundary_wire(b)+y).subrange(8+n,12+n) == word32(b.grant.owner));
    }
    word32_injective(a.grant.owner,b.grant.owner);
    assert(word64(a.grant.epoch) == word64(b.grant.epoch)) by {
        assert((boundary_wire(a)+x).subrange(12+n,20+n) == word64(a.grant.epoch));
        assert((boundary_wire(b)+y).subrange(12+n,20+n) == word64(b.grant.epoch));
    }
    word64_injective(a.grant.epoch,b.grant.epoch);
    assert(x == y) by {
        assert((boundary_wire(a)+x).skip(20+n) == x);
        assert((boundary_wire(b)+y).skip(20+n) == y);
    }
}
pub proof fn boundaries_prefix_unique(a: Seq<Boundary>,b: Seq<Boundary>,x: Seq<u8>,y: Seq<u8>)
    requires a.len() == b.len(), boundaries_wire(a)+x == boundaries_wire(b)+y,
    ensures equivalent(a,b), x == y,
    decreases a.len(),
{
    if a.len() > 0 {
        boundaries_head(a); boundaries_head(b);
        assert(boundary_wire(a[0])+(boundaries_wire(a.skip(1))+x) =~= boundaries_wire(a)+x);
        assert(boundary_wire(b[0])+(boundaries_wire(b.skip(1))+y) =~= boundaries_wire(b)+y);
        boundary_prefix_unique(a[0],b[0],boundaries_wire(a.skip(1))+x,boundaries_wire(b.skip(1))+y);
        boundaries_prefix_unique(a.skip(1),b.skip(1),x,y);
        assert forall|i: int| 0 <= i < a.len() implies a[i].start@ == b[i].start@ && a[i].grant == b[i].grant by {
            if i > 0 { assert(a.skip(1)[i-1] == a[i]); assert(b.skip(1)[i-1] == b[i]); }
        }
    }
}
pub proof fn table_prefix_unique(a: RouteTable,b: RouteTable,x: Seq<u8>,y: Seq<u8>)
    requires table_wire(a)+x == table_wire(b)+y,
    ensures a.table == b.table, equivalent(a.boundaries@,b.boundaries@), x == y,
{
    assert(word64(a.table) == word64(b.table)) by {
        assert((table_wire(a)+x).take(8) == word64(a.table));
        assert((table_wire(b)+y).take(8) == word64(b.table));
    }
    word64_injective(a.table,b.table);
    assert(word64(a.boundaries.len() as u64) == word64(b.boundaries.len() as u64)) by {
        assert((table_wire(a)+x).subrange(8,16) == word64(a.boundaries.len() as u64));
        assert((table_wire(b)+y).subrange(8,16) == word64(b.boundaries.len() as u64));
    }
    word64_injective(a.boundaries.len() as u64,b.boundaries.len() as u64);
    assert(boundaries_wire(a.boundaries@)+x == boundaries_wire(b.boundaries@)+y) by {
        assert((table_wire(a)+x).skip(16) == boundaries_wire(a.boundaries@)+x);
        assert((table_wire(b)+y).skip(16) == boundaries_wire(b.boundaries@)+y);
    }
    boundaries_prefix_unique(a.boundaries@,b.boundaries@,x,y);
}
pub proof fn parsed_table_unique(data: Seq<u8>,begin: nat,end: nat,t: RouteTable)
    requires begin <= end <= data.len(), data.subrange(begin as int,end as int) == table_wire(t),
    ensures forall|expected: RouteTable| wire_prefix(data,begin,table_wire(expected)) ==> table_wire(t) == table_wire(expected),
{
    assert forall|expected: RouteTable| wire_prefix(data,begin,table_wire(expected)) implies table_wire(t) == table_wire(expected) by {
        let rest = data.skip(begin as int);
        assert(rest == table_wire(t)+data.skip(end as int));
        assert(rest == table_wire(expected)+rest.skip(table_wire(expected).len() as int));
        table_prefix_unique(t,expected,data.skip(end as int),rest.skip(table_wire(expected).len() as int));
        equal_table_wire(t,expected);
    }
}
pub proof fn table_header(data: Seq<u8>,begin: nat,t: RouteTable)
    requires wire_prefix(data,begin,table_wire(t)),
    ensures wire_prefix(data,begin,word64(t.table)),
        wire_prefix(data,begin+8,word64(t.boundaries.len() as u64)),
        wire_prefix(data,begin+16,boundaries_wire(t.boundaries@)),
        word64(t.table).len() == 8, word64(t.boundaries.len() as u64).len() == 8,
        table_wire(t).len() == 16+boundaries_wire(t.boundaries@).len(),
{
    assert(table_wire(t) =~= word64(t.table)+(word64(t.boundaries.len() as u64)+boundaries_wire(t.boundaries@)));
    prefix_split(data,begin,word64(t.table),word64(t.boundaries.len() as u64)+boundaries_wire(t.boundaries@));
    prefix_split(data,begin+8,word64(t.boundaries.len() as u64),boundaries_wire(t.boundaries@));
}
pub proof fn snapshot_header_wire(s: Snapshot)
    ensures wire_prefix(snapshot_wire(s),0,word64(SNAPSHOT_TAG)),
        wire_prefix(snapshot_wire(s),8,word64(s.version)),
        wire_prefix(snapshot_wire(s),16,word64(s.tables.len() as u64)),
        wire_prefix(snapshot_wire(s),24,tables_wire(s.tables@)),
        snapshot_wire(s).len() == 24+tables_wire(s.tables@).len(),
{
    let w = snapshot_wire(s);
    let payload = tables_wire(s.tables@);
    assert(w =~= word64(SNAPSHOT_TAG)+(word64(s.version)+(word64(s.tables.len() as u64)+payload)));
    assert(w.subrange(0,w.len() as int) == w);
    assert(wire_prefix(w,0,w));
    prefix_split(w,0,word64(SNAPSHOT_TAG),word64(s.version)+(word64(s.tables.len() as u64)+payload));
    prefix_split(w,8,word64(s.version),word64(s.tables.len() as u64)+payload);
    prefix_split(w,16,word64(s.tables.len() as u64),payload);
}
pub proof fn snapshot_header(s: Snapshot,nodes: Seq<u32>)
    requires crate::routing::snapshot_valid(s,nodes),
    ensures wire_prefix(snapshot_wire(s),0,word64(SNAPSHOT_TAG)),
        wire_prefix(snapshot_wire(s),8,word64(s.version)),
        wire_prefix(snapshot_wire(s),16,word64(s.tables.len() as u64)),
        wire_prefix(snapshot_wire(s),24,tables_wire(s.tables@)),
        snapshot_wire(s).len() == 24+tables_wire(s.tables@).len(),
        tables_wire(s.tables@).len() >= 36*s.tables.len(),
        forall|i: int| 0 <= i < s.tables.len() ==> s.tables@[i].boundaries.len() > 0,
{
    hide(word64); hide(word32);
    snapshot_header_wire(s);
    tables_size(s.tables@);
}
pub proof fn tables_unique(a: Seq<RouteTable>,b: Seq<RouteTable>)
    requires a.len() == b.len(), tables_wire(a) == tables_wire(b),
    ensures forall|i: int| 0 <= i < a.len() ==> a[i].table == b[i].table && equivalent(a[i].boundaries@,b[i].boundaries@),
    decreases a.len(),
{
    if a.len() > 0 {
        tables_head(a); tables_head(b);
        table_prefix_unique(a[0],b[0],tables_wire(a.skip(1)),tables_wire(b.skip(1)));
        tables_unique(a.skip(1),b.skip(1));
        assert forall|i: int| 0 <= i < a.len() implies a[i].table == b[i].table && equivalent(a[i].boundaries@,b[i].boundaries@) by {
            if i > 0 { assert(a.skip(1)[i-1] == a[i]); assert(b.skip(1)[i-1] == b[i]); }
        }
    }
}
pub proof fn semantic_roundtrip(original: Snapshot,decoded: Snapshot)
    requires snapshot_wire(original) == snapshot_wire(decoded),
    ensures same_snapshot(original,decoded),
{
    assert(word64(original.version) == word64(decoded.version)) by {
        assert(snapshot_wire(original).subrange(8,16) == word64(original.version));
        assert(snapshot_wire(decoded).subrange(8,16) == word64(decoded.version));
    }
    word64_injective(original.version,decoded.version);
    assert(word64(original.tables.len() as u64) == word64(decoded.tables.len() as u64)) by {
        assert(snapshot_wire(original).subrange(16,24) == word64(original.tables.len() as u64));
        assert(snapshot_wire(decoded).subrange(16,24) == word64(decoded.tables.len() as u64));
    }
    word64_injective(original.tables.len() as u64,decoded.tables.len() as u64);
    assert(tables_wire(original.tables@) == tables_wire(decoded.tables@)) by {
        assert(snapshot_wire(original).skip(24) == tables_wire(original.tables@));
        assert(snapshot_wire(decoded).skip(24) == tables_wire(decoded.tables@));
    }
    tables_unique(original.tables@,decoded.tables@);
}
pub proof fn roundtrip_routes(original: Snapshot,decoded: Snapshot,nodes: Seq<u32>,table: u64,key: Seq<u8>)
    requires crate::routing::snapshot_valid(original,nodes), snapshot_wire(original) == snapshot_wire(decoded),
    ensures crate::routing::snapshot_valid(decoded,nodes),
        crate::routing::snapshot_lookup(original,table,key) == crate::routing::snapshot_lookup(decoded,table,key),
{
    semantic_roundtrip(original,decoded);
    crate::routing_proofs::identical_validity(original,decoded,nodes);
    crate::routing_proofs::identical_observations(original,decoded,table,key);
}
} // verus!
