//! Same-source request, cursor, page-admission and decoded-order contracts.
//! This module is called by the production ABI adapter, not a test model.
use vstd::prelude::*;
use crate::types::Status;
use crate::bytes::{compare, copy_bytes};
#[cfg(verus_keep_ghost)]
use crate::bytes::cmp_spec;
use crate::routing_codec::Reader;
#[cfg(verus_keep_ghost)]
use crate::routing_codec_proofs::{blob, word32};
verus! {
pub const REQUEST_CAPACITY: usize = 1024;
pub const PAGE_CAPACITY: usize = 8176;
pub const PAGE_ROWS: usize = 64;
pub const REQUEST_TAG: u32 = 0x3151534d;
pub const PAGE_TAG: u32 = 0x3150534d;

pub struct Request {
    pub lo: Vec<u8>,
    pub hi: Option<Vec<u8>>,
    pub cursor: Option<Vec<u8>>,
    pub reverse: bool,
}
pub open spec fn opt_bytes(value: Option<Vec<u8>>) -> Option<Seq<u8>> {
    match value { Some(v) => Some(v@), None => None }
}
pub open spec fn borrowed(value: Option<&[u8]>) -> Option<Seq<u8>> {
    match value { Some(v) => Some(v@), None => None }
}
impl Request {
    pub open spec fn contains_spec(&self, key: Seq<u8>) -> bool {
        cmp_spec(key,self.lo@) >= 0
        && match self.hi { Some(hi) => cmp_spec(key,hi@) < 0, None => true }
    }
    pub open spec fn follows_spec(&self, key: Seq<u8>, previous: Option<Seq<u8>>) -> bool {
        self.contains_spec(key) && match previous {
            Some(last) => if self.reverse { cmp_spec(key,last) < 0 }
                else { cmp_spec(key,last) > 0 }, None => true,
        }
    }
    pub open spec fn wf(&self) -> bool {
        (match self.hi { Some(hi) => cmp_spec(self.lo@,hi@) < 0,
            None => true })
        && match self.cursor { Some(last) => self.contains_spec(last@), None => true }
    }
    pub fn contains(&self, key: &[u8]) -> (yes: bool)
        ensures yes == self.contains_spec(key@),
    {
        if compare(key,&self.lo) < 0 { return false; }
        match &self.hi { Some(hi) => compare(key,hi) < 0, None => true }
    }
    pub fn follows(&self, key: &[u8], previous: Option<&[u8]>) -> (yes: bool)
        ensures yes == self.follows_spec(key@,borrowed(previous)),
    {
        if !self.contains(key) { return false; }
        match previous {
            Some(last) => if self.reverse { compare(key,last) < 0 }
                else { compare(key,last) > 0 }, None => true,
        }
    }
    pub fn valid(&self) -> (yes: bool)
        ensures yes == self.wf(),
    {
        match &self.hi {
            Some(hi) => if compare(&self.lo,hi) >= 0 { return false; },
            None => {},
        }
        match &self.cursor { Some(key) => self.contains(key), None => true }
    }
    pub fn advance(&mut self, key: &[u8]) -> (status: Status)
        requires old(self).wf(),
        ensures final(self).wf(),
            final(self).lo == old(self).lo, final(self).hi == old(self).hi,
            final(self).reverse == old(self).reverse,
            (status == Status::Ok) == old(self).follows_spec(key@,opt_bytes(old(self).cursor)),
            status == Status::Ok ==> opt_bytes(final(self).cursor) == Some(key@),
            status != Status::Ok ==> final(self).cursor == old(self).cursor,
    {
        let previous = match &self.cursor { Some(v) => Some(v.as_slice()), None => None };
        if !self.follows(key,previous) { return Status::Invalid; }
        self.cursor = Some(copy_bytes(key));
        Status::Ok
    }
}

// The native reverse primitive requires a concrete start. For an unbounded
// request, an exhaustive forward pass discovers its actual maximum; no finite
// sentinel can dominate arbitrary byte strings. The same transaction retains
// the range read until the inclusive reverse pass finishes.
pub struct Maximum {
    pub key: Option<Vec<u8>>,
    pub observed: Ghost<Seq<Seq<u8>>>,
}
impl Maximum {
    pub open spec fn wf(&self) -> bool {
        match self.key {
            None => self.observed@.len() == 0,
            Some(key) => self.observed@.len() > 0
                && key@ == self.observed@.last()
                && forall|i: int| 0 <= i < self.observed@.len()
                    ==> cmp_spec(self.observed@[i], key@) <= 0,
        }
    }
    pub fn new() -> (out: Self)
        ensures out.wf(), out.key is None, out.observed@ == Seq::<Seq<u8>>::empty(),
    {
        let ghost observed = Seq::empty();
        Self { key: None, observed: Ghost(observed) }
    }
    pub fn observe(&mut self, request: &Request, key: &[u8]) -> (status: Status)
        requires old(self).wf(),
        ensures final(self).wf(),
            status == Status::Ok ==> final(self).observed@ == old(self).observed@.push(key@)
                && opt_bytes(final(self).key) == Some(key@) && request.contains_spec(key@),
            status != Status::Ok ==> *final(self) == *old(self),
    {
        if !request.reverse || request.hi.is_some() || request.cursor.is_some()
            || !request.contains(key) { return Status::Invalid; }
        if let Some(previous) = &self.key {
            if compare(previous,key) >= 0 { return Status::Invalid; }
        }
        proof {
            if let Some(previous) = &self.key {
                assert forall|i: int| 0 <= i < self.observed@.len()
                    implies cmp_spec(self.observed@[i],key@) <= 0 by {
                    crate::bytes::cmp_trans(self.observed@[i],previous@,key@);
                }
            }
            crate::bytes::cmp_laws(key@,key@);
            self.observed@ = self.observed@.push(key@);
        }
        self.key = Some(copy_bytes(key));
        Status::Ok
    }
}

pub struct WireRow { pub key: Seq<u8>, pub value: Seq<u8> }
pub open spec fn rows_wire(rows: Seq<WireRow>) -> Seq<u8>
    decreases rows.len(),
{
    if rows.len() == 0 { Seq::empty() }
    else { rows_wire(rows.drop_last()) + blob(rows.last().key) + blob(rows.last().value) }
}
pub open spec fn ordered(request: &Request, rows: Seq<WireRow>) -> bool {
    forall|i: int| 0 <= i < rows.len() ==> request.follows_spec(#[trigger] rows[i].key,
        if i == 0 { opt_bytes(request.cursor) } else { Some(rows[i-1].key) })
}
pub struct PageSummary<'a> {
    pub count: usize,
    pub eof: bool,
    pub last: Option<&'a [u8]>,
    pub rows: Ghost<Seq<WireRow>>,
}
pub fn read_blob<'a>(reader: &mut Reader<'a>) -> (out: Result<&'a [u8],Status>)
    requires old(reader).wf(),
    ensures final(reader).wf(), final(reader).data() == old(reader).data(),
        match out {
            Ok(bytes) => final(reader).position() == old(reader).position()+8+bytes.len()
                && final(reader).position() <= final(reader).data().len()
                && final(reader).data().subrange(old(reader).position() as int,final(reader).position() as int) == blob(bytes@),
            Err(_) => true,
        },
{
    let ghost begin = reader.position();
    let length = match reader.read_u64() { Ok(v) => v, Err(e) => return Err(e) };
    if length > reader.remaining() as u64 { return Err(Status::Invalid); }
    let bytes = match reader.read_slice(length as usize) { Ok(v) => v, Err(e) => return Err(e) };
    proof { assert(reader.data().subrange(begin as int,reader.position() as int) =~= blob(bytes@)); }
    Ok(bytes)
}
impl Request {
    pub fn validate_page<'a>(&self, bytes: &'a [u8]) -> (out: Result<PageSummary<'a>,Status>)
        requires self.wf(),
        ensures match out {
            Ok(page) => page.count <= PAGE_ROWS && (page.count == 0 ==> page.eof)
                && page.count == page.rows@.len() && ordered(self,page.rows@)
                && bytes@ == word32(PAGE_TAG)+word32(page.count as u32)
                    +word32(if page.eof {1u32} else {0u32})+rows_wire(page.rows@)
                && match page.last {
                    Some(last) => page.count > 0 && last@ == page.rows@.last().key,
                    None => page.count == 0,
                },
            Err(_) => true,
        },
    {
        hide(word32);
        hide(blob);
        hide(cmp_spec);
        if bytes.len() < 12 || bytes.len() > PAGE_CAPACITY { return Err(Status::Invalid); }
        let mut reader = Reader::new(bytes);
        let tag = match reader.read_u32() { Ok(v) => v, Err(e) => return Err(e) };
        if tag != PAGE_TAG { return Err(Status::Invalid); }
        let count = match reader.read_u32() { Ok(v) => v as usize, Err(e) => return Err(e) };
        let eof = match reader.read_u32() { Ok(v) => v, Err(e) => return Err(e) };
        if count > PAGE_ROWS || eof > 1 || (count == 0 && eof == 0) { return Err(Status::Invalid); }
        let ghost header = word32(PAGE_TAG)+word32(count as u32)+word32(eof);
        proof {
            crate::routing_codec_proofs::join_wire(bytes@,0,4,8,word32(tag),word32(count as u32));
            crate::routing_codec_proofs::join_wire(bytes@,0,8,12,
                word32(tag)+word32(count as u32),word32(eof));
            assert(bytes@.take(12) =~= header);
        }
        let ghost mut rows: Seq<WireRow> = Seq::empty();
        let mut last: Option<&'a [u8]> = None;
        let mut i = 0usize;
        while i < count
            invariant reader.wf(), reader.data() == bytes@,
                i <= count <= PAGE_ROWS, eof <= 1, count == 0 ==> eof == 1,
                header == word32(PAGE_TAG)+word32(count as u32)+word32(eof),
                rows.len() == i, ordered(self,rows), self.wf(),
                reader.position() >= 12,
                bytes@.take(reader.position() as int) == header+rows_wire(rows),
                match last { Some(key) => i > 0 && key@ == rows.last().key, None => i == 0 },
            decreases count-i,
        {
            let ghost begin = reader.position();
            let key = match read_blob(&mut reader) { Ok(v) => v, Err(e) => return Err(e) };
            let ghost middle = reader.position();
            let value = match read_blob(&mut reader) { Ok(v) => v, Err(e) => return Err(e) };
            let previous = match last { Some(key) => Some(key), None => match &self.cursor {
                Some(key) => Some(key.as_slice()), None => None,
            } };
            if !self.follows(key,previous) { return Err(Status::Invalid); }
            proof {
                let old_rows = rows;
                rows = rows.push(WireRow { key: key@,value: value@ });
                assert(rows.drop_last() == old_rows);
                assert(rows.last().key == key@);
                assert forall|j: int| 0 <= j < rows.len() implies self.follows_spec(#[trigger] rows[j].key,
                    if j == 0 {opt_bytes(self.cursor)} else {Some(rows[j-1].key)}) by {
                    if j < old_rows.len() { assert(rows[j] == old_rows[j]); }
                }
                crate::routing_codec_proofs::join_wire(bytes@,0,begin,middle,
                    header+rows_wire(old_rows),blob(key@));
                crate::routing_codec_proofs::join_wire(bytes@,0,middle,reader.position(),
                    header+rows_wire(old_rows)+blob(key@),blob(value@));
                assert(bytes@.take(reader.position() as int) =~= header+rows_wire(rows));
            }
            last = Some(key); i += 1;
        }
        match reader.finish() { Status::Ok => {}, _ => return Err(Status::Invalid) }
        proof {
            assert(reader.position() == reader.data().len());
            assert(reader.data() == bytes@);
            assert(reader.position() == bytes@.len());
            assert(bytes@.take(reader.position() as int) =~= bytes@);
            assert(bytes@ == header+rows_wire(rows));
            assert(eof == if eof != 0 {1u32} else {0u32});
        }
        Ok(PageSummary { count,eof: eof != 0,last,rows: Ghost(rows) })
    }
}

pub struct PageBudget { pub count: usize, pub used: usize, pub more: bool }
impl PageBudget {
    pub open spec fn wf(&self) -> bool {
        self.count <= PAGE_ROWS && 12 <= self.used <= PAGE_CAPACITY
        && (self.more ==> self.count > 0)
    }
    pub fn new() -> (out: Self)
        ensures out.wf(), out.count == 0, out.used == 12, !out.more,
    { Self {count: 0,used: 12,more: false} }
    // NotFound means a real lookahead row did not fit, never end of range.
    pub fn reserve(&mut self,key: usize,value: usize) -> (status: Status)
        requires old(self).wf(),
        ensures final(self).wf(),
            status == Status::Ok ==> final(self).count == old(self).count+1
                && final(self).used == old(self).used+16+key+value && !final(self).more,
            status == Status::NotFound ==> final(self).more
                && final(self).count == old(self).count && final(self).used == old(self).used,
            status != Status::Ok && status != Status::NotFound ==> *final(self) == *old(self),
    {
        if self.more { return Status::Invalid; }
        if key > PAGE_CAPACITY-28 || value > PAGE_CAPACITY-28-key { return Status::Exhausted; }
        let size = 16+key+value;
        if self.count == PAGE_ROWS || size > PAGE_CAPACITY-self.used {
            // The first row is either admitted or rejected as oversized above.
            if self.count == 0 { return Status::Invalid; }
            self.more = true;
            return Status::NotFound;
        }
        self.count += 1; self.used += size;
        Status::Ok
    }
}
pub struct Completion { pub done: bool }
impl Completion {
    pub fn complete(&mut self,count: usize,eof: bool,stopped: bool) -> (status: Status)
        ensures status == Status::Ok ==> !old(self).done && count <= PAGE_ROWS
                && (count > 0 || eof) && final(self).done == (eof || stopped),
            status != Status::Ok ==> *final(self) == *old(self),
    {
        if self.done || count > PAGE_ROWS || (count == 0 && !eof) { return Status::Invalid; }
        self.done = eof || stopped;
        Status::Ok
    }
}
}
