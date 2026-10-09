//! Versioned whole-snapshot wire format. Integers are little endian; lengths and
//! counts are u64. Parsing never allocates from an unchecked wire length.
//! Layout: u64 SNAPSHOT_TAG, u64 committed version, u64 table count, then each
//! table's u64 ID, u64 boundary count, and boundaries (u64 start length, bytes,
//! u32 owner, u64 epoch). There is no checksum authority or optional suffix.
//! Decode rejects truncation, trailing bytes, duplicate tables, empty/misordered
//! partitions, noncanonical adjacent full grants, unknown owners and epochs
//! newer than the snapshot. Encode preserves the supplied native table order.
//! Reusable Reader/Writer primitives use exactly these integer/blob conventions.
use vstd::prelude::*;
use crate::types::{Boundary,Grant,Status};
use crate::directory::RouteTable;
use crate::bytes::copy_bytes;
use crate::routing::Snapshot;
#[cfg(verus_keep_ghost)]
use crate::routing::snapshot_valid;
#[cfg(verus_keep_ghost)]
use crate::routing_codec_proofs::*;
verus! {
// Bytes: MAKORNG followed by format version 1.
pub const SNAPSHOT_TAG: u64 = 0x01474e524f4b414d;
pub struct Reader<'a> { data: &'a [u8], position: usize }
impl<'a> Reader<'a> {
    pub closed spec fn data(&self) -> Seq<u8> { self.data@ }
    pub closed spec fn position(&self) -> nat { self.position as nat }
    pub closed spec fn wf(&self) -> bool { self.position <= self.data.len() }
    pub fn new(data: &'a [u8]) -> (r: Self)
        ensures r.wf(), r.data() == data@, r.position() == 0,
    { Self { data,position: 0 } }
    pub fn remaining(&self) -> (n: usize)
        requires self.wf(),
        ensures n == self.data().len()-self.position(),
    { self.data.len()-self.position }
    pub fn consumed(&self) -> (n: usize)
        ensures n == self.position(),
    { self.position }
    pub fn read_slice(&mut self,length: usize) -> (out: Result<&'a [u8],Status>)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).data() == old(self).data(),
            (out is Ok) == (old(self).position()+length <= old(self).data().len()),
            match out {
                Ok(bytes) => final(self).position() == old(self).position()+length
                    && bytes@ == old(self).data().subrange(old(self).position() as int,final(self).position() as int),
                Err(_) => final(self).position() == old(self).position(),
            },
    {
        if length > self.remaining() { return Err(Status::Invalid); }
        let begin = self.position;
        self.position += length;
        Ok(&self.data[begin..self.position])
    }
    pub fn finish(&self) -> (status: Status)
        requires self.wf(),
        ensures (status == Status::Ok) == (self.position() == self.data().len()),
    { if self.position == self.data.len() { Status::Ok } else { Status::Invalid } }
    pub fn read_u64(&mut self) -> (out: Result<u64,Status>)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).data() == old(self).data(),
            forall|v: u64| wire_prefix(old(self).data(),old(self).position(),word64(v)) ==> out == Ok(v),
            match out { Ok(v) => final(self).position() == old(self).position()+8
                && final(self).data().subrange(old(self).position() as int,final(self).position() as int) == word64(v),
                Err(_) => final(self).position() == old(self).position() && old(self).position()+8 > final(self).data().len() },
    {
        if self.remaining() < 8 { return Err(Status::Invalid); }
        let p = self.position;
        let a = self.data[p]; let b = self.data[p+1]; let c = self.data[p+2]; let d = self.data[p+3];
        let e = self.data[p+4]; let f = self.data[p+5]; let g = self.data[p+6]; let h = self.data[p+7];
        let v = (a as u64) | ((b as u64)<<8) | ((c as u64)<<16) | ((d as u64)<<24)
            | ((e as u64)<<32) | ((f as u64)<<40) | ((g as u64)<<48) | ((h as u64)<<56);
        proof { unpack64(a,b,c,d,e,f,g,h); assert(self.data@.subrange(p as int,p as int+8) =~= word64(v)); }
        proof { assert forall|expected: u64| wire_prefix(self.data@,p as nat,word64(expected)) implies v == expected by {
            word64_injective(v,expected);
        } }
        self.position = p+8;
        Ok(v)
    }
    pub fn read_u32(&mut self) -> (out: Result<u32,Status>)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).data() == old(self).data(),
            forall|v: u32| wire_prefix(old(self).data(),old(self).position(),word32(v)) ==> out == Ok(v),
            match out { Ok(v) => final(self).position() == old(self).position()+4
                && final(self).data().subrange(old(self).position() as int,final(self).position() as int) == word32(v),
                Err(_) => final(self).position() == old(self).position() && old(self).position()+4 > final(self).data().len() },
    {
        if self.remaining() < 4 { return Err(Status::Invalid); }
        let p = self.position;
        let a = self.data[p]; let b = self.data[p+1]; let c = self.data[p+2]; let d = self.data[p+3];
        let v = (a as u32) | ((b as u32)<<8) | ((c as u32)<<16) | ((d as u32)<<24);
        proof { unpack32(a,b,c,d); assert(self.data@.subrange(p as int,p as int+4) =~= word32(v)); }
        proof { assert forall|expected: u32| wire_prefix(self.data@,p as nat,word32(expected)) implies v == expected by {
            word32_injective(v,expected);
        } }
        self.position = p+4;
        Ok(v)
    }
    pub fn read_bytes(&mut self) -> (out: Result<Vec<u8>,Status>)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).data() == old(self).data(),
            final(self).position() >= old(self).position(),
            forall|bytes: Seq<u8>| wire_prefix(old(self).data(),old(self).position(),blob(bytes)) ==>
                out is Ok && out->Ok_0@ == bytes,
            match out { Ok(bytes) => final(self).position() == old(self).position()+8+bytes.len()
                && final(self).data().subrange(old(self).position() as int,final(self).position() as int) == blob(bytes@), Err(_) => true },
    {
        let ghost begin = self.position;
        let ghost has_expected = exists|bytes: Seq<u8>| wire_prefix(self.data(),self.position(),blob(bytes));
        let ghost expected = choose|bytes: Seq<u8>| wire_prefix(self.data(),self.position(),blob(bytes));
        proof { if has_expected { prefix_split(self.data(),self.position(),word64(expected.len() as u64),expected); } }
        let length = match self.read_u64() { Ok(n) => n, Err(e) => return Err(e) };
        if length > self.remaining() as u64 { return Err(Status::Invalid); }
        let n = length as usize;
        let end = self.position+n;
        let bytes = copy_bytes(&self.data[self.position..end]);
        self.position = end;
        proof { assert(self.data@.subrange(begin as int,end as int) =~= blob(bytes@)); }
        proof {
            assert forall|expected: Seq<u8>| wire_prefix(self.data(),begin as nat,blob(expected)) implies bytes@ == expected by {
                prefix_split(self.data(),begin as nat,word64(expected.len() as u64),expected);
                word64_injective(length,expected.len() as u64);
                assert(bytes@ =~= expected);
            }
        }
        Ok(bytes)
    }
    fn read_boundary(&mut self) -> (out: Result<Boundary,Status>)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).data() == old(self).data(),
            final(self).position() >= old(self).position(),
            match out { Ok(b) => final(self).data().subrange(old(self).position() as int,final(self).position() as int) == boundary_wire(b), Err(_) => true },
            forall|b: Boundary| wire_prefix(old(self).data(),old(self).position(),boundary_wire(b)) ==>
                out is Ok && boundary_wire(out->Ok_0) == boundary_wire(b),
    {
        let ghost begin = self.position();
        let ghost has_expected = exists|b: Boundary| wire_prefix(self.data(),self.position(),boundary_wire(b));
        let ghost expected = choose|b: Boundary| has_expected ==> wire_prefix(self.data(),self.position(),boundary_wire(b));
        proof { if has_expected {
            assert(boundary_wire(expected) =~= blob(expected.start@)+(word32(expected.grant.owner)+word64(expected.grant.epoch)));
            prefix_split(self.data(),begin,blob(expected.start@),word32(expected.grant.owner)+word64(expected.grant.epoch));
        } }
        let start = match self.read_bytes() { Ok(v) => v, Err(e) => return Err(e) };
        proof { if has_expected {
            prefix_split(self.data(),self.position(),word32(expected.grant.owner),word64(expected.grant.epoch));
        } }
        let owner = match self.read_u32() { Ok(v) => v, Err(e) => return Err(e) };
        let epoch = match self.read_u64() { Ok(v) => v, Err(e) => return Err(e) };
        let b = Boundary { start,grant: Grant { owner,epoch } };
        proof { assert(self.data().subrange(begin as int,self.position() as int) =~= boundary_wire(b)); }
        proof {
            assert forall|expected: Boundary| wire_prefix(self.data(),begin,boundary_wire(expected))
                implies boundary_wire(b) == boundary_wire(expected) by {
                let rest = self.data().skip(begin as int);
                assert(rest == boundary_wire(b)+self.data().skip(self.position() as int));
                assert(rest == boundary_wire(expected)+rest.skip(boundary_wire(expected).len() as int));
                boundary_prefix_unique(b,expected,self.data().skip(self.position() as int),rest.skip(boundary_wire(expected).len() as int));
            }
        }
        Ok(b)
    }
    fn read_table(&mut self) -> (out: Result<RouteTable,Status>)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).data() == old(self).data(),
            final(self).position() >= old(self).position(),
            match out { Ok(t) => final(self).data().subrange(old(self).position() as int,final(self).position() as int) == table_wire(t), Err(_) => true },
            forall|t: RouteTable| t.boundaries.len() > 0 && wire_prefix(old(self).data(),old(self).position(),table_wire(t)) ==>
                out is Ok && table_wire(out->Ok_0) == table_wire(t),
    {
        hide(word64); hide(word32);
        let ghost begin = self.position();
        let ghost has_expected = exists|t: RouteTable| t.boundaries.len() > 0 && wire_prefix(self.data(),begin,table_wire(t));
        let ghost expected = choose|t: RouteTable| has_expected ==> t.boundaries.len() > 0 && wire_prefix(self.data(),begin,table_wire(t));
        proof { if has_expected { table_header(self.data(),begin,expected); } }
        let table = match self.read_u64() { Ok(v) => v, Err(e) => return Err(e) };
        proof { if has_expected { boundaries_size(expected.boundaries@); } }
        let count = match self.read_u64() { Ok(v) => v, Err(e) => return Err(e) };
        // A boundary needs length, owner, epoch even when its start is empty.
        if count == 0 || count > (self.remaining()/20) as u64 { return Err(Status::Invalid); }
        let ghost base = self.position();
        let mut boundaries = Vec::new();
        let mut i = 0u64;
        proof { if has_expected {
            assert(expected.boundaries@.skip(0) == expected.boundaries@);
            assert(expected.boundaries@.take(0) == Seq::<Boundary>::empty());
        } }
        while i < count
            invariant self.wf(), self.data() == old(self).data(), i <= count, boundaries.len() == i,
                begin == old(self).position(),
                has_expected == (exists|t: RouteTable| t.boundaries.len() > 0 && wire_prefix(old(self).data(),begin,table_wire(t))),
                begin+16 == base, base <= self.position(),
                self.data().subrange(begin as int,base as int) == word64(table)+word64(count),
                self.data().subrange(base as int,self.position() as int) == boundaries_wire(boundaries@),
                has_expected ==> count == expected.boundaries.len()
                    && wire_prefix(old(self).data(),begin,table_wire(expected))
                    && wire_prefix(self.data(),self.position(),boundaries_wire(expected.boundaries@.skip(i as int)))
                    && self.position() == base+boundaries_wire(expected.boundaries@.take(i as int)).len(),
            decreases count-i,
        {
            proof { if has_expected {
                boundaries_head(expected.boundaries@.skip(i as int));
                assert(expected.boundaries@.skip(i as int)[0] == expected.boundaries@[i as int]);
                assert(expected.boundaries@.skip(i as int).skip(1) == expected.boundaries@.skip(i as int+1));
                prefix_split(self.data(),self.position(),boundary_wire(expected.boundaries@[i as int]),
                    boundaries_wire(expected.boundaries@.skip(i as int+1)));
            } }
            let ghost middle = self.position();
            let ghost previous = boundaries@;
            let b = match self.read_boundary() { Ok(b) => b, Err(e) => return Err(e) };
            let ghost boundary = b;
            boundaries.push(b); i += 1;
            proof { if has_expected {
                assert(expected.boundaries@.take(i as int).drop_last() == expected.boundaries@.take(i as int-1));
                assert(expected.boundaries@.take(i as int).last() == expected.boundaries@[i as int-1]);
            } }
            proof {
                join_wire(self.data(),base,middle,self.position(),boundaries_wire(previous),boundary_wire(boundary));
                assert(boundaries@.drop_last() == previous); assert(boundaries@.last() == boundary);
            }
        }
        let t = RouteTable { table,boundaries };
        proof { assert(self.data().subrange(begin as int,self.position() as int) =~= table_wire(t)); }
        proof { parsed_table_unique(self.data(),begin,self.position(),t); }
        Ok(t)
    }
}
pub struct Writer { bytes: Vec<u8> }
impl Writer {
    pub closed spec fn view(&self) -> Seq<u8> { self.bytes@ }
    pub fn new() -> (w: Self) ensures w.view() == Seq::<u8>::empty(), { Self { bytes: Vec::new() } }
    pub fn with_capacity(capacity: usize) -> (out: Result<Self,Status>)
        ensures (out is Ok) == (capacity <= isize::MAX as usize),
            out is Ok ==> out->Ok_0.view() == Seq::<u8>::empty(),
    {
        if capacity > isize::MAX as usize { return Err(Status::Exhausted); }
        Ok(Self { bytes: Vec::with_capacity(capacity) })
    }
    pub fn len(&self) -> (n: usize) ensures n == self.view().len(), { self.bytes.len() }
    pub fn as_bytes(&self) -> (bytes: &[u8]) ensures bytes@ == self.view(), { self.bytes.as_slice() }
    pub fn into_bytes(self) -> (out: Vec<u8>) ensures out@ == self.view(), { self.bytes }
    pub fn write_u64(&mut self,value: u64) -> (status: Status)
        ensures status == Status::Ok ==> final(self).view() == old(self).view()+word64(value),
            status != Status::Ok ==> final(self).view() == old(self).view(),
            (status == Status::Ok) == (old(self).view().len()+8 <= isize::MAX as int),
    {
        if self.bytes.len() > (isize::MAX as usize)-8 { return Status::Exhausted; }
        self.bytes.push(value as u8); self.bytes.push((value>>8) as u8);
        self.bytes.push((value>>16) as u8); self.bytes.push((value>>24) as u8);
        self.bytes.push((value>>32) as u8); self.bytes.push((value>>40) as u8);
        self.bytes.push((value>>48) as u8); self.bytes.push((value>>56) as u8);
        proof { assert(self.view() =~= old(self).view()+word64(value)); }
        Status::Ok
    }
    pub fn write_u32(&mut self,value: u32) -> (status: Status)
        ensures status == Status::Ok ==> final(self).view() == old(self).view()+word32(value),
            status != Status::Ok ==> final(self).view() == old(self).view(),
            (status == Status::Ok) == (old(self).view().len()+4 <= isize::MAX as int),
    {
        if self.bytes.len() > (isize::MAX as usize)-4 { return Status::Exhausted; }
        self.bytes.push(value as u8); self.bytes.push((value>>8) as u8);
        self.bytes.push((value>>16) as u8); self.bytes.push((value>>24) as u8);
        proof { assert(self.view() =~= old(self).view()+word32(value)); }
        Status::Ok
    }
    pub fn write_bytes(&mut self,value: &[u8]) -> (result: Status)
        ensures result == Status::Ok ==> final(self).view() == old(self).view()+blob(value@),
            (result == Status::Ok) == (old(self).view().len()+8+value.len() <= isize::MAX as int),
            result != Status::Ok ==> final(self).view() == old(self).view(),
    {
        if value.len() > (isize::MAX as usize)-8 || self.bytes.len() > (isize::MAX as usize)-8-value.len() { return Status::Exhausted; }
        let status = self.write_u64(value.len() as u64);
        if !matches!(status,Status::Ok) { return status; }
        let mut i = 0usize;
        while i < value.len()
            invariant i <= value.len(), self.view() == old(self).view()+word64(value.len() as u64)+value@.take(i as int),
                old(self).view().len()+8+value.len() <= isize::MAX as int,
            decreases value.len()-i,
        {
            self.bytes.push(value[i]); i += 1;
            proof { assert(self.view() =~= old(self).view()+word64(value.len() as u64)+value@.take(i as int)); }
        }
        Status::Ok
    }
    pub fn write_table(&mut self,t: &RouteTable) -> (result: Status)
        ensures result == Status::Ok ==> final(self).view() == old(self).view()+table_wire(*t),
    {
        let status = self.write_u64(t.table); if !matches!(status,Status::Ok) { return status; }
        let status = self.write_u64(t.boundaries.len() as u64); if !matches!(status,Status::Ok) { return status; }
        let mut i = 0usize;
        while i < t.boundaries.len()
            invariant i <= t.boundaries.len(),
                self.view() == old(self).view()+word64(t.table)+word64(t.boundaries.len() as u64)+boundaries_wire(t.boundaries@.take(i as int)),
            decreases t.boundaries.len()-i,
        {
            let b = &t.boundaries[i];
            let status = self.write_bytes(&b.start); if !matches!(status,Status::Ok) { return status; }
            let status = self.write_u32(b.grant.owner); if !matches!(status,Status::Ok) { return status; }
            let status = self.write_u64(b.grant.epoch); if !matches!(status,Status::Ok) { return status; }
            i += 1;
            proof { assert(t.boundaries@.take(i as int).drop_last() == t.boundaries@.take(i as int-1));
                assert(self.view() =~= old(self).view()+word64(t.table)+word64(t.boundaries.len() as u64)+boundaries_wire(t.boundaries@.take(i as int))); }
        }
        proof { assert(t.boundaries@.take(t.boundaries.len() as int) == t.boundaries@); assert(self.view() =~= old(self).view()+table_wire(*t)); }
        Status::Ok
    }
}
pub fn table_encoded_size(table: &RouteTable) -> (out: Result<usize,Status>)
    ensures match out { Ok(n) => n == table_wire(*table).len() && n <= isize::MAX as usize, Err(_) => true },
{
    let mut size = 16usize;
    let mut i = 0usize;
    while i < table.boundaries.len()
        invariant i <= table.boundaries.len(), size <= isize::MAX as usize,
            size == 16+boundaries_wire(table.boundaries@.take(i as int)).len(),
        decreases table.boundaries.len()-i,
    {
        let length = table.boundaries[i].start.len();
        if length > (isize::MAX as usize)-20 || size > (isize::MAX as usize)-20-length { return Err(Status::Exhausted); }
        size += 20+length;
        i += 1;
        proof {
            assert(table.boundaries@.take(i as int).drop_last() == table.boundaries@.take(i as int-1));
            assert(table.boundaries@.take(i as int).last() == table.boundaries@[i as int-1]);
        }
    }
    proof { assert(table.boundaries@.take(i as int) == table.boundaries@); }
    Ok(size)
}
/// Preflight the exact encoded size before allocating the one output buffer.
pub fn encoded_size(snapshot: &Snapshot) -> (out: Result<usize,Status>)
    ensures match out { Ok(n) => n == snapshot_wire(*snapshot).len() && n <= isize::MAX as usize, Err(_) => true },
{
    let mut size = 24usize;
    let mut i = 0usize;
    while i < snapshot.tables.len()
        invariant i <= snapshot.tables.len(), size <= isize::MAX as usize,
            size == 24+tables_wire(snapshot.tables@.take(i as int)).len(),
        decreases snapshot.tables.len()-i,
    {
        let n = match table_encoded_size(&snapshot.tables[i]) { Ok(n) => n, Err(e) => return Err(e) };
        if size > (isize::MAX as usize)-n { return Err(Status::Exhausted); }
        size += n; i += 1;
        proof {
            assert(snapshot.tables@.take(i as int).drop_last() == snapshot.tables@.take(i as int-1));
            assert(snapshot.tables@.take(i as int).last() == snapshot.tables@[i as int-1]);
        }
    }
    proof { assert(snapshot.tables@.take(i as int) == snapshot.tables@); }
    Ok(size)
}
pub fn encode(snapshot: &Snapshot) -> (out: Result<Vec<u8>,Status>)
    ensures match out { Ok(data) => data@ == snapshot_wire(*snapshot), Err(_) => true },
{
    let size = match encoded_size(snapshot) { Ok(n) => n, Err(e) => return Err(e) };
    let mut writer = match Writer::with_capacity(size) { Ok(w) => w, Err(e) => return Err(e) };
    let status = writer.write_u64(SNAPSHOT_TAG); if !matches!(status,Status::Ok) { return Err(status); }
    let status = writer.write_u64(snapshot.version); if !matches!(status,Status::Ok) { return Err(status); }
    let status = writer.write_u64(snapshot.tables.len() as u64); if !matches!(status,Status::Ok) { return Err(status); }
    let mut i = 0usize;
    while i < snapshot.tables.len()
        invariant i <= snapshot.tables.len(), writer.view() == word64(SNAPSHOT_TAG)+word64(snapshot.version)
            +word64(snapshot.tables.len() as u64)+tables_wire(snapshot.tables@.take(i as int)),
        decreases snapshot.tables.len()-i,
    {
        let status = writer.write_table(&snapshot.tables[i]); if !matches!(status,Status::Ok) { return Err(status); }
        i += 1;
        proof { assert(snapshot.tables@.take(i as int).drop_last() == snapshot.tables@.take(i as int-1));
            assert(writer.view() =~= word64(SNAPSHOT_TAG)+word64(snapshot.version)+word64(snapshot.tables.len() as u64)+tables_wire(snapshot.tables@.take(i as int))); }
    }
    proof { assert(snapshot.tables@.take(snapshot.tables.len() as int) == snapshot.tables@); assert(writer.view() =~= snapshot_wire(*snapshot)); }
    Ok(writer.into_bytes())
}
pub fn decode(data: &[u8],nodes: &[u32]) -> (out: Result<Snapshot,Status>)
    ensures match out { Ok(s) => snapshot_valid(s,nodes@) && snapshot_wire(s) == data@, Err(_) => true },
        (exists|s: Snapshot| snapshot_valid(s,nodes@) && snapshot_wire(s) == data@) ==> out is Ok,
{
    hide(word64); hide(word32); hide(snapshot_valid);
    let ghost has_expected = exists|s: Snapshot| snapshot_valid(s,nodes@) && snapshot_wire(s) == data@;
    let ghost expected = choose|s: Snapshot| has_expected ==> snapshot_valid(s,nodes@) && snapshot_wire(s) == data@;
    proof { if has_expected { snapshot_header(expected,nodes@); } }
    let mut reader = Reader::new(data);
    let tag = match reader.read_u64() { Ok(v) => v, Err(e) => return Err(e) };
    if tag != SNAPSHOT_TAG { return Err(Status::Invalid); }
    let version = match reader.read_u64() { Ok(v) => v, Err(e) => return Err(e) };
    let count = match reader.read_u64() { Ok(v) => v, Err(e) => return Err(e) };
    if count > (reader.remaining()/36) as u64 { return Err(Status::Invalid); }
    let mut tables = Vec::new();
    let mut i = 0u64;
    proof { if has_expected {
        assert(expected.tables@.skip(0) == expected.tables@);
        assert(expected.tables@.take(0) == Seq::<RouteTable>::empty());
    } }
    while i < count
        invariant reader.wf(), reader.data() == data@, i <= count, tables.len() == i,
            has_expected == (exists|s: Snapshot| snapshot_valid(s,nodes@) && snapshot_wire(s) == data@),
            24 <= reader.position(), data@.take(24) == word64(SNAPSHOT_TAG)+word64(version)+word64(count),
            data@.subrange(24,reader.position() as int) == tables_wire(tables@),
            has_expected ==> version == expected.version && count == expected.tables.len()
                && snapshot_valid(expected,nodes@) && snapshot_wire(expected) == data@
                && data.len() == 24+tables_wire(expected.tables@).len()
                && (forall|j: int| 0 <= j < expected.tables.len() ==> expected.tables@[j].boundaries.len() > 0)
                && wire_prefix(data@,reader.position(),tables_wire(expected.tables@.skip(i as int)))
                && reader.position() == 24+tables_wire(expected.tables@.take(i as int)).len(),
        decreases count-i,
    {
        proof { if has_expected {
            tables_head(expected.tables@.skip(i as int));
            assert(expected.tables@.skip(i as int)[0] == expected.tables@[i as int]);
            assert(expected.tables@.skip(i as int).skip(1) == expected.tables@.skip(i as int+1));
            assert(expected.tables@[i as int].boundaries.len() > 0);
            prefix_split(data@,reader.position(),table_wire(expected.tables@[i as int]),tables_wire(expected.tables@.skip(i as int+1)));
        } }
        let ghost middle = reader.position();
        let ghost previous = tables@;
        let t = match reader.read_table() { Ok(t) => t, Err(e) => return Err(e) };
        let ghost table = t;
        tables.push(t); i += 1;
        proof { if has_expected {
            assert(expected.tables@.take(i as int).drop_last() == expected.tables@.take(i as int-1));
            assert(expected.tables@.take(i as int).last() == expected.tables@[i as int-1]);
        } }
        proof {
            join_wire(data@,24,middle,reader.position(),tables_wire(previous),table_wire(table));
            assert(tables@.drop_last() == previous); assert(tables@.last() == table);
        }
    }
    proof { if has_expected {
        assert(expected.tables@.take(i as int) == expected.tables@);
        assert(reader.position() == data.len());
    } }
    if !matches!(reader.finish(),Status::Ok) { return Err(Status::Invalid); }
    let snapshot = Snapshot { version,tables };
    proof {
        assert(data@ =~= snapshot_wire(snapshot));
        if has_expected {
            semantic_roundtrip(expected,snapshot);
            crate::routing_proofs::identical_validity(expected,snapshot,nodes@);
        }
    }
    if !snapshot.validate(nodes) { return Err(Status::Invalid); }
    proof { assert(data@ =~= snapshot_wire(snapshot)); }
    Ok(snapshot)
}
} // verus!
