//! Checked, versioned native coordinator journal records. No volatile slot IDs.
use vstd::prelude::*;
use crate::directory::RouteTable;
use crate::migration::Request;
use crate::types::{Certificate, KeyRange, Status, TxnId};
use crate::routing_codec::{Reader, Writer};
use crate::durable_codec::{read_txn, write_txn, read_range, write_range};
#[cfg(verus_keep_ghost)]
use crate::durable_codec::{txn_wire, range_wire};
#[cfg(verus_keep_ghost)]
use crate::routing_codec_proofs::*;

verus! {
// MAKODC, version 1. Bootstrap and operation tags share a versioned envelope.
pub const COORDINATOR_TAG: u64 = 0x0143444f4b414d;
pub enum Operation {
    Begin { nonce: TxnId, source: u32, destination: u32, range: KeyRange },
    Request(Request),
    Receive { generation: u64, owner: u32, certificate: Certificate },
    Reply(TxnId),
}
pub open spec fn request_code(r: Request) -> u32 {
    match r { Request::Freeze => 0, Request::Final => 1, Request::Retire => 2,
        Request::Commit => 3, Request::Abort => 4, Request::Finish => 5 }
}
fn encode_request(r: Request) -> (n: u32) ensures n == request_code(r), {
    match r { Request::Freeze => 0, Request::Final => 1, Request::Retire => 2,
        Request::Commit => 3, Request::Abort => 4, Request::Finish => 5 }
}
fn decode_request(n: u32) -> (out: Result<Request,Status>)
    ensures match out { Ok(r) => request_code(r) == n, Err(_) => n > 5 },
{
    match n { 0 => Ok(Request::Freeze), 1 => Ok(Request::Final), 2 => Ok(Request::Retire),
        3 => Ok(Request::Commit), 4 => Ok(Request::Abort), 5 => Ok(Request::Finish), _ => Err(Status::Invalid) }
}
pub open spec fn certificate_code(c: Certificate) -> u32 {
    match c { Certificate::Drained => 0, Certificate::Ready => 1, Certificate::Retired => 2,
        Certificate::SourceDone => 3, Certificate::DestinationDone => 4 }
}
fn encode_certificate(c: Certificate) -> (n: u32) ensures n == certificate_code(c), {
    match c { Certificate::Drained => 0, Certificate::Ready => 1, Certificate::Retired => 2,
        Certificate::SourceDone => 3, Certificate::DestinationDone => 4 }
}
fn decode_certificate(n: u32) -> (out: Result<Certificate,Status>)
    ensures match out { Ok(c) => certificate_code(c) == n, Err(_) => n > 4 },
{
    match n { 0 => Ok(Certificate::Drained), 1 => Ok(Certificate::Ready), 2 => Ok(Certificate::Retired),
        3 => Ok(Certificate::SourceDone), 4 => Ok(Certificate::DestinationDone), _ => Err(Status::Invalid) }
}
pub open spec fn operation_tag(op: Operation) -> u32 {
    match op { Operation::Begin { .. } => 1, Operation::Request(_) => 2,
        Operation::Receive { .. } => 3, Operation::Reply(_) => 4 }
}
pub open spec fn operation_body(op: Operation) -> Seq<u8> {
    match op {
        Operation::Begin {nonce,source,destination,range} =>
            txn_wire(nonce)+word32(source)+word32(destination)+range_wire(range),
        Operation::Request(r) => word32(request_code(r)),
        Operation::Receive {generation,owner,certificate} =>
            word64(generation)+word32(owner)+word32(certificate_code(certificate)),
        Operation::Reply(nonce) => txn_wire(nonce),
    }
}
pub open spec fn operation_wire(op: Operation) -> Seq<u8> {
    word64(COORDINATOR_TAG)+word32(operation_tag(op))+operation_body(op)
}
pub proof fn integer_sizes()
    ensures forall|n: u64| #[trigger] word64(n).len() == 8,
        forall|n: u32| #[trigger] word32(n).len() == 4,
{}
pub proof fn operation_prefix(op: Operation)
    ensures wire_prefix(operation_wire(op),0,word64(COORDINATOR_TAG)),
        wire_prefix(operation_wire(op),8,word32(operation_tag(op))),
        wire_prefix(operation_wire(op),12,operation_body(op)),
{
    hide(word64); hide(word32); integer_sizes();
    let header = word64(COORDINATOR_TAG)+word32(operation_tag(op));
    assert(operation_wire(op).skip(0) =~= operation_wire(op));
    crate::durable_codec::suffix_prefix(operation_wire(op),0,operation_wire(op));
    assert(wire_prefix(operation_wire(op),0,header+operation_body(op)));
    prefix_split(operation_wire(op),0,header,operation_body(op));
    prefix_split(operation_wire(op),0,word64(COORDINATOR_TAG),word32(operation_tag(op)));
}
pub fn encode_operation(op: &Operation) -> (out: Result<Vec<u8>,Status>)
    ensures match out { Ok(bytes) => bytes@ == operation_wire(*op), Err(_) => true },
{
    hide(word64); hide(word32);
    let mut w = Writer::new();
    let s = w.write_u64(COORDINATOR_TAG); if !matches!(s,Status::Ok) { return Err(s); }
    match op {
        Operation::Begin { nonce, source, destination, range } => {
            let s = w.write_u32(1); if !matches!(s,Status::Ok) { return Err(s); }
            let s = write_txn(&mut w,*nonce); if !matches!(s,Status::Ok) { return Err(s); }
            let s = w.write_u32(*source); if !matches!(s,Status::Ok) { return Err(s); }
            let s = w.write_u32(*destination); if !matches!(s,Status::Ok) { return Err(s); }
            let s = write_range(&mut w,range); if !matches!(s,Status::Ok) { return Err(s); }
        },
        Operation::Request(r) => {
            let s = w.write_u32(2); if !matches!(s,Status::Ok) { return Err(s); }
            let s = w.write_u32(encode_request(*r)); if !matches!(s,Status::Ok) { return Err(s); }
        },
        Operation::Receive { generation, owner, certificate } => {
            let s = w.write_u32(3); if !matches!(s,Status::Ok) { return Err(s); }
            let s = w.write_u64(*generation); if !matches!(s,Status::Ok) { return Err(s); }
            let s = w.write_u32(*owner); if !matches!(s,Status::Ok) { return Err(s); }
            let s = w.write_u32(encode_certificate(*certificate)); if !matches!(s,Status::Ok) { return Err(s); }
        },
        Operation::Reply(nonce) => {
            let s = w.write_u32(4); if !matches!(s,Status::Ok) { return Err(s); }
            let s = write_txn(&mut w,*nonce); if !matches!(s,Status::Ok) { return Err(s); }
        },
    }
    proof { assert(w.view() =~= operation_wire(*op)); }
    Ok(w.into_bytes())
}
fn read_operation_body(r: &mut Reader,kind: u32,Ghost(expected): Ghost<Option<Operation>>)
    -> (out: Result<Operation,Status>)
    requires old(r).wf(), match expected {
        Some(e) => kind == operation_tag(e) && wire_prefix(old(r).data(),old(r).position(),operation_body(e)),
        None => true,
    },
    ensures final(r).wf(), final(r).data() == old(r).data(),
        old(r).position() <= final(r).position() <= final(r).data().len(),
        match out { Ok(op) => operation_tag(op) == kind
            && final(r).data().subrange(old(r).position() as int,final(r).position() as int) == operation_body(op), Err(_) => true },
        match expected { Some(e) => out is Ok
            && final(r).position() == old(r).position()+operation_body(e).len(), None => true },
{
    hide(word64); hide(word32);
    proof { integer_sizes(); r.position_bound(); }
    let ghost start = r.position();
    let ghost data = r.data();
    let op = match kind {
        1 => {
            proof { if let Some(e) = expected {
                assert(e is Begin);
                let rest = word32(e->Begin_source)+word32(e->Begin_destination)+range_wire(e->Begin_range);
                assert(operation_body(e) =~= txn_wire(e->Begin_nonce)+rest);
                prefix_split(data,start,txn_wire(e->Begin_nonce),rest);
            } }
            let nonce = match read_txn(r) { Ok(v) => v, Err(e) => return Err(e) };
            let ghost a = r.position();
            proof { if let Some(e) = expected {
                let rest = word32(e->Begin_destination)+range_wire(e->Begin_range);
                assert(a == start+txn_wire(e->Begin_nonce).len());
                assert(word32(e->Begin_source)+word32(e->Begin_destination)+range_wire(e->Begin_range)
                    =~= word32(e->Begin_source)+rest);
                assert(wire_prefix(data,a,word32(e->Begin_source)+rest));
                prefix_split(data,a,word32(e->Begin_source),rest);
            } }
            let source = match r.read_u32() { Ok(v) => v, Err(e) => return Err(e) };
            let ghost b = r.position();
            proof {
                join_wire(data,start,a,b,txn_wire(nonce),word32(source));
                if let Some(e) = expected {
                    prefix_split(data,b,word32(e->Begin_destination),range_wire(e->Begin_range));
                }
            }
            let destination = match r.read_u32() { Ok(v) => v, Err(e) => return Err(e) };
            let ghost c = r.position();
            proof {
                join_wire(data,start,b,c,txn_wire(nonce)+word32(source),word32(destination));
                if let Some(e) = expected {
                    assert(c == b+4);
                    assert(wire_prefix(data,c,range_wire(e->Begin_range)));
                }
            }
            let ghost expected_range = match expected {
                Some(e) => Some(e->Begin_range), None => None,
            };
            proof { if let Some(er) = expected_range {
                assert(r.data() == data && r.position() == c);
                assert(wire_prefix(r.data(),r.position(),range_wire(er)));
            } }
            let decoded_range = read_range(r);
            proof { if let Some(er) = expected_range {
                assert(decoded_range is Ok);
                assert(range_wire(decoded_range->Ok_0) == range_wire(er));
                assert(r.position() == c+range_wire(er).len());
            } }
            let range = match decoded_range { Ok(v) => v, Err(e) => return Err(e) };
            proof {
                join_wire(data,start,c,r.position(),txn_wire(nonce)+word32(source)+word32(destination),range_wire(range));
                if let Some(e) = expected {
                    assert(range_wire(range) == range_wire(e->Begin_range));
                    assert(r.position() == start+operation_body(e).len());
                }
            }
            Operation::Begin {nonce,source,destination,range}
        },
        2 => {
            proof { if let Some(e) = expected {
                assert(e is Request);
                assert(wire_prefix(data,start,word32(request_code(e->Request_0))));
            } }
            let code = match r.read_u32() { Ok(v) => v, Err(e) => return Err(e) };
            let request = match decode_request(code) { Ok(v) => v, Err(e) => return Err(e) };
            Operation::Request(request)
        },
        3 => {
            proof { if let Some(e) = expected {
                assert(e is Receive);
                let rest = word32(e->Receive_owner)+word32(certificate_code(e->Receive_certificate));
                assert(operation_body(e) =~= word64(e->Receive_generation)+rest);
                prefix_split(data,start,word64(e->Receive_generation),rest);
            } }
            let generation = match r.read_u64() { Ok(v) => v, Err(e) => return Err(e) };
            let ghost a = r.position();
            proof { if let Some(e) = expected {
                prefix_split(data,a,word32(e->Receive_owner),word32(certificate_code(e->Receive_certificate)));
            } }
            let owner = match r.read_u32() { Ok(v) => v, Err(e) => return Err(e) };
            let ghost b = r.position();
            proof { join_wire(data,start,a,b,word64(generation),word32(owner)); }
            let code = match r.read_u32() { Ok(v) => v, Err(e) => return Err(e) };
            let certificate = match decode_certificate(code) { Ok(v) => v, Err(e) => return Err(e) };
            proof { join_wire(data,start,b,r.position(),word64(generation)+word32(owner),word32(code)); }
            Operation::Receive {generation,owner,certificate}
        },
        4 => {
            proof { if let Some(e) = expected {
                assert(e is Reply);
                assert(wire_prefix(data,start,txn_wire(e->Reply_0)));
            } }
            let nonce = match read_txn(r) { Ok(v) => v, Err(e) => return Err(e) };
            Operation::Reply(nonce)
        },
        _ => return Err(Status::Invalid),
    };
    Ok(op)
}
pub fn decode_operation(bytes: &[u8]) -> (out: Result<Operation,Status>)
    ensures match out { Ok(op) => bytes@ == operation_wire(op), Err(_) => true },
        (exists|op: Operation| bytes@ == operation_wire(op)) ==> out is Ok,
{
    hide(word64); hide(word32);
    proof { integer_sizes(); }
    let ghost has_expected = exists|op: Operation| bytes@ == operation_wire(op);
    let ghost expected = if has_expected { Some(choose|op: Operation| bytes@ == operation_wire(op)) } else { None };
    proof { if let Some(e) = expected { operation_prefix(e); } }
    let mut r = Reader::new(bytes);
    let tag = match r.read_u64() { Ok(v) => v, Err(e) => return Err(e) };
    if tag != COORDINATOR_TAG { return Err(Status::Invalid); }
    let kind = match r.read_u32() { Ok(v) => v, Err(e) => return Err(e) };
    let ghost body_start = r.position();
    proof { join_wire(bytes@,0,8,body_start,word64(tag),word32(kind)); }
    let op = match read_operation_body(&mut r,kind,Ghost(expected)) { Ok(op) => op, Err(e) => return Err(e) };
    proof {
        join_wire(bytes@,0,body_start,r.position(),word64(tag)+word32(kind),operation_body(op));
        if let Some(e) = expected { assert(r.position() == bytes@.len()); }
    }
    if !matches!(r.finish(),Status::Ok) { return Err(Status::Invalid); }
    proof { assert(bytes@.subrange(0,bytes@.len() as int) == bytes@); }
    Ok(op)
}
pub open spec fn nodes_wire(nodes: Seq<u32>) -> Seq<u8>
    decreases nodes.len(),
{ if nodes.len() == 0 { Seq::empty() } else { nodes_wire(nodes.drop_last())+word32(nodes.last()) } }
pub open spec fn bootstrap_wire(nodes: Seq<u32>, tables: Seq<RouteTable>) -> Seq<u8> {
    word64(COORDINATOR_TAG)+word32(0)+word64(nodes.len() as u64)+nodes_wire(nodes)
        +word64(tables.len() as u64)+tables_wire(tables)
}
pub fn encode_bootstrap(nodes: &Vec<u32>, tables: &Vec<RouteTable>) -> (out: Result<Vec<u8>,Status>)
    ensures match out { Ok(bytes) => bytes@ == bootstrap_wire(nodes@,tables@), Err(_) => true },
{
    hide(word64); hide(word32);
    let mut w = Writer::new();
    let s = w.write_u64(COORDINATOR_TAG); if !matches!(s,Status::Ok) { return Err(s); }
    let s = w.write_u32(0); if !matches!(s,Status::Ok) { return Err(s); }
    let s = w.write_u64(nodes.len() as u64); if !matches!(s,Status::Ok) { return Err(s); }
    let mut i = 0usize;
    while i < nodes.len()
        invariant i <= nodes.len(), w.view() == word64(COORDINATOR_TAG)+word32(0)
            +word64(nodes.len() as u64)+nodes_wire(nodes@.take(i as int)),
        decreases nodes.len()-i,
    {
        let s = w.write_u32(nodes[i]); if !matches!(s,Status::Ok) { return Err(s); }
        i += 1;
        proof {
            assert(nodes@.take(i as int).drop_last() == nodes@.take(i as int-1));
            assert(nodes@.take(i as int).last() == nodes@[i as int-1]);
            assert(w.view() =~= word64(COORDINATOR_TAG)+word32(0)
                +word64(nodes.len() as u64)+nodes_wire(nodes@.take(i as int)));
        }
    }
    proof { assert(nodes@.take(nodes.len() as int) == nodes@); }
    let s = w.write_u64(tables.len() as u64); if !matches!(s,Status::Ok) { return Err(s); }
    let mut i = 0usize;
    while i < tables.len()
        invariant i <= tables.len(), w.view() == word64(COORDINATOR_TAG)+word32(0)
            +word64(nodes.len() as u64)+nodes_wire(nodes@)+word64(tables.len() as u64)
            +tables_wire(tables@.take(i as int)),
        decreases tables.len()-i,
    {
        let s = w.write_table(&tables[i]); if !matches!(s,Status::Ok) { return Err(s); }
        i += 1;
        proof {
            assert(tables@.take(i as int).drop_last() == tables@.take(i as int-1));
            assert(tables@.take(i as int).last() == tables@[i as int-1]);
            assert(w.view() =~= word64(COORDINATOR_TAG)+word32(0)
                +word64(nodes.len() as u64)+nodes_wire(nodes@)+word64(tables.len() as u64)
                +tables_wire(tables@.take(i as int)));
        }
    }
    proof {
        assert(tables@.take(tables.len() as int) == tables@);
        assert(w.view() =~= bootstrap_wire(nodes@,tables@));
    }
    Ok(w.into_bytes())
}
pub proof fn nodes_head(nodes: Seq<u32>)
    requires nodes.len() > 0,
    ensures nodes_wire(nodes) == word32(nodes[0])+nodes_wire(nodes.skip(1)),
    decreases nodes.len(),
{
    if nodes.len() == 1 {
        assert(nodes.skip(1) == Seq::<u32>::empty());
        assert(nodes.drop_last() == Seq::<u32>::empty());
    } else {
        nodes_head(nodes.drop_last());
        assert(nodes.skip(1).drop_last() == nodes.drop_last().skip(1));
        assert(nodes.skip(1).last() == nodes.last());
    }
}
pub open spec fn bootstrap_encodable(nodes: Seq<u32>,tables: Seq<RouteTable>) -> bool {
    nodes.len() <= usize::MAX && tables.len() <= usize::MAX
        && forall|i: int| 0 <= i < tables.len() ==> tables[i].boundaries.len() > 0
}
fn read_nodes(r: &mut Reader,Ghost(expected): Ghost<Option<Seq<u32>>>) -> (out: Result<Vec<u32>,Status>)
    requires old(r).wf(), match expected { Some(e) => e.len() <= usize::MAX
        && wire_prefix(old(r).data(),old(r).position(),word64(e.len() as u64)+nodes_wire(e)), None => true },
    ensures final(r).wf(), final(r).data() == old(r).data(),
        old(r).position() <= final(r).position() <= final(r).data().len(),
        match out { Ok(nodes) => final(r).data().subrange(old(r).position() as int,final(r).position() as int)
            == word64(nodes.len() as u64)+nodes_wire(nodes@), Err(_) => true },
        match expected { Some(e) => out is Ok && out->Ok_0@ == e
            && final(r).position() == old(r).position()+8+4*e.len(), None => true },
{
    hide(word64); hide(word32);
    proof { integer_sizes(); r.position_bound(); }
    let ghost start = r.position(); let ghost data = r.data();
    proof { if let Some(e) = expected {
        crate::durable_coordinator_proofs::nodes_size(e);
        prefix_split(data,start,word64(e.len() as u64),nodes_wire(e));
    } }
    let count = match r.read_u64() { Ok(v) => v, Err(e) => return Err(e) };
    if count > (r.remaining()/4) as u64 { return Err(Status::Invalid); }
    let mut nodes = Vec::new(); let mut i = 0u64;
    proof { if let Some(e) = expected {
        assert(count == e.len());
        assert(e.take(0) == Seq::<u32>::empty());
        assert(e.skip(0) == e);
        assert(wire_prefix(data,r.position(),nodes_wire(e)));
    } }
    while i < count
        invariant r.wf(), r.data() == data, start <= r.position() <= data.len(),
            data == old(r).data(), start == old(r).position(),
            i <= count, nodes.len() == i, r.position() == start+8+4*i,
            data.subrange(start as int,r.position() as int) == word64(count)+nodes_wire(nodes@),
            match expected { Some(e) => count == e.len() && nodes@ == e.take(i as int)
                && wire_prefix(data,r.position(),nodes_wire(e.skip(i as int))), None => true },
        decreases count-i,
    {
        proof { if let Some(e) = expected {
            nodes_head(e.skip(i as int));
            assert(e.skip(i as int)[0] == e[i as int]);
            assert(e.skip(i as int).skip(1) == e.skip(i as int+1));
            prefix_split(data,r.position(),word32(e[i as int]),nodes_wire(e.skip(i as int+1)));
        } }
        let ghost previous = nodes@; let ghost middle = r.position();
        let node = match r.read_u32() { Ok(v) => v, Err(e) => return Err(e) };
        nodes.push(node); i += 1;
        proof {
            join_wire(data,start,middle,r.position(),word64(count)+nodes_wire(previous),word32(node));
            assert(nodes@.drop_last() == previous); assert(nodes@.last() == node);
            assert(data.subrange(start as int,r.position() as int) =~= word64(count)+nodes_wire(nodes@));
            if let Some(e) = expected { assert(nodes@ =~= e.take(i as int)); }
        }
    }
    proof { if let Some(e) = expected { assert(e.take(i as int) == e); } }
    Ok(nodes)
}
fn read_tables(r: &mut Reader,Ghost(expected): Ghost<Option<Seq<RouteTable>>>) -> (out: Result<Vec<RouteTable>,Status>)
    requires old(r).wf(), match expected { Some(e) => e.len() <= usize::MAX
        && (forall|j: int| 0 <= j < e.len() ==> e[j].boundaries.len() > 0)
        && wire_prefix(old(r).data(),old(r).position(),word64(e.len() as u64)+tables_wire(e)), None => true },
    ensures final(r).wf(), final(r).data() == old(r).data(),
        old(r).position() <= final(r).position() <= final(r).data().len(),
        match out { Ok(tables) => final(r).data().subrange(old(r).position() as int,final(r).position() as int)
            == word64(tables.len() as u64)+tables_wire(tables@), Err(_) => true },
        match expected { Some(e) => out is Ok && out->Ok_0.len() == e.len()
            && tables_wire(out->Ok_0@) == tables_wire(e)
            && final(r).position() == old(r).position()+8+tables_wire(e).len(), None => true },
{
    hide(word64); hide(word32);
    proof { integer_sizes(); r.position_bound(); }
    let ghost start = r.position(); let ghost data = r.data();
    proof { if let Some(e) = expected {
        tables_size(e);
        prefix_split(data,start,word64(e.len() as u64),tables_wire(e));
    } }
    let count = match r.read_u64() { Ok(v) => v, Err(e) => return Err(e) };
    if count > (r.remaining()/36) as u64 { return Err(Status::Invalid); }
    let mut tables = Vec::new(); let mut i = 0u64;
    proof { if let Some(e) = expected {
        assert(count == e.len());
        assert(e.take(0) == Seq::<RouteTable>::empty());
        assert(e.skip(0) == e);
        assert(wire_prefix(data,r.position(),tables_wire(e)));
    } }
    while i < count
        invariant r.wf(), r.data() == data, start+8 <= r.position() <= data.len(),
            data == old(r).data(), start == old(r).position(),
            i <= count, tables.len() == i,
            data.subrange(start as int,r.position() as int) == word64(count)+tables_wire(tables@),
            match expected { Some(e) => count == e.len()
                && (forall|j: int| 0 <= j < e.len() ==> e[j].boundaries.len() > 0)
                && tables_wire(tables@) == tables_wire(e.take(i as int))
                && wire_prefix(data,r.position(),tables_wire(e.skip(i as int)))
                && r.position()+tables_wire(e.skip(i as int)).len() == start+8+tables_wire(e).len(), None => true },
        decreases count-i,
    {
        proof { if let Some(e) = expected {
            tables_head(e.skip(i as int));
            assert(e.skip(i as int)[0] == e[i as int]);
            assert(e.skip(i as int).skip(1) == e.skip(i as int+1));
            prefix_split(data,r.position(),table_wire(e[i as int]),tables_wire(e.skip(i as int+1)));
        } }
        let ghost previous = tables@; let ghost middle = r.position();
        let table = match r.read_table() { Ok(v) => v, Err(e) => return Err(e) };
        let ghost t = table;
        tables.push(table); i += 1;
        proof {
            join_wire(data,start,middle,r.position(),word64(count)+tables_wire(previous),table_wire(t));
            assert(tables@.drop_last() == previous); assert(tables@.last() == t);
            assert(data.subrange(start as int,r.position() as int) =~= word64(count)+tables_wire(tables@));
            if let Some(e) = expected {
                assert(table_wire(t) == table_wire(e[i as int-1]));
                assert(e.take(i as int).drop_last() == e.take(i as int-1));
                assert(e.take(i as int).last() == e[i as int-1]);
                assert(tables_wire(tables@) =~= tables_wire(e.take(i as int)));
            }
        }
    }
    proof { if let Some(e) = expected {
        assert(e.take(i as int) == e);
        assert(e.skip(i as int) == Seq::<RouteTable>::empty());
    } }
    Ok(tables)
}
pub proof fn bootstrap_prefix(nodes: Seq<u32>,tables: Seq<RouteTable>)
    requires nodes.len() <= usize::MAX, tables.len() <= usize::MAX,
    ensures wire_prefix(bootstrap_wire(nodes,tables),0,word64(COORDINATOR_TAG)),
        wire_prefix(bootstrap_wire(nodes,tables),8,word32(0)),
        wire_prefix(bootstrap_wire(nodes,tables),12,word64(nodes.len() as u64)+nodes_wire(nodes)),
        wire_prefix(bootstrap_wire(nodes,tables),20+nodes_wire(nodes).len(),word64(tables.len() as u64)+tables_wire(tables)),
{
    hide(word64); hide(word32); integer_sizes();
    let bytes = bootstrap_wire(nodes,tables);
    let header = word64(COORDINATOR_TAG)+word32(0);
    let nw = word64(nodes.len() as u64)+nodes_wire(nodes);
    let tw = word64(tables.len() as u64)+tables_wire(tables);
    assert(bytes =~= header+(nw+tw));
    assert(bytes.skip(0) =~= bytes);
    crate::durable_codec::suffix_prefix(bytes,0,bytes);
    assert(wire_prefix(bytes,0,header+(nw+tw)));
    prefix_split(bytes,0,header,nw+tw);
    prefix_split(bytes,0,word64(COORDINATOR_TAG),word32(0));
    prefix_split(bytes,12,nw,tw);
}
pub fn decode_bootstrap(bytes: &[u8]) -> (out: Result<(Vec<u32>,Vec<RouteTable>),Status>)
    ensures match out { Ok((nodes,tables)) => bytes@ == bootstrap_wire(nodes@,tables@), Err(_) => true },
        (exists|nodes: Seq<u32>,tables: Seq<RouteTable>| bootstrap_encodable(nodes,tables)
            && bytes@ == bootstrap_wire(nodes,tables)) ==> out is Ok,
{
    hide(word64); hide(word32);
    proof { integer_sizes(); }
    let ghost has_expected = exists|nt: (Seq<u32>,Seq<RouteTable>)| #[trigger] bootstrap_encodable(nt.0,nt.1)
        && bytes@ == bootstrap_wire(nt.0,nt.1);
    proof {
        assert forall|nodes: Seq<u32>,tables: Seq<RouteTable>|
            bootstrap_encodable(nodes,tables) && bytes@ == #[trigger] bootstrap_wire(nodes,tables)
            implies has_expected by {
            let nt = (nodes,tables);
            assert(bootstrap_encodable(nt.0,nt.1) && bytes@ == bootstrap_wire(nt.0,nt.1));
        }
    }
    let ghost expected = if has_expected {
        Some(choose|nt: (Seq<u32>,Seq<RouteTable>)| #[trigger] bootstrap_encodable(nt.0,nt.1)
            && bytes@ == bootstrap_wire(nt.0,nt.1))
    } else { None };
    let ghost en = match expected { Some((nodes,_)) => Some(nodes), None => None };
    let ghost et = match expected { Some((_,tables)) => Some(tables), None => None };
    proof { if let Some((nodes,tables)) = expected {
        bootstrap_prefix(nodes,tables);
        crate::durable_coordinator_proofs::nodes_size(nodes);
    } }
    let mut r = Reader::new(bytes);
    let tag = match r.read_u64() { Ok(v) => v, Err(e) => return Err(e) };
    let kind = match r.read_u32() { Ok(v) => v, Err(e) => return Err(e) };
    if tag != COORDINATOR_TAG || kind != 0 { return Err(Status::Invalid); }
    proof { join_wire(bytes@,0,8,12,word64(tag),word32(kind)); }
    let nodes = match read_nodes(&mut r,Ghost(en)) { Ok(v) => v, Err(e) => return Err(e) };
    let ghost middle = r.position();
    proof { join_wire(bytes@,0,12,middle,word64(tag)+word32(kind),word64(nodes.len() as u64)+nodes_wire(nodes@)); }
    let tables = match read_tables(&mut r,Ghost(et)) { Ok(v) => v, Err(e) => return Err(e) };
    proof {
        join_wire(bytes@,0,middle,r.position(),word64(tag)+word32(kind)+(word64(nodes.len() as u64)+nodes_wire(nodes@)),
            word64(tables.len() as u64)+tables_wire(tables@));
        if let Some((n,t)) = expected { assert(r.position() == bytes@.len()); }
    }
    if !matches!(r.finish(),Status::Ok) { return Err(Status::Invalid); }
    proof {
        assert(bytes@.subrange(0,bytes@.len() as int) =~= bytes@);
        let all = word64(tag)+word32(kind)+(word64(nodes.len() as u64)+nodes_wire(nodes@))
            +(word64(tables.len() as u64)+tables_wire(tables@));
        assert(all =~= bootstrap_wire(nodes@,tables@));
        assert(bytes@ == all);
    }
    Ok((nodes,tables))
}
} // verus!
