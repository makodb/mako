//! Same-source request, cursor, page-admission and decoded-order contracts.
//! This module is called by the production ABI adapter, not a test model.
use vstd::prelude::*;
use crate::types::{Status, TxnId, Grant};
use crate::bytes::{compare, copy_bytes};
#[cfg(verus_keep_ghost)]
use crate::bytes::cmp_spec;
use crate::routing_codec::Reader;
#[cfg(verus_keep_ghost)]
use crate::routing_codec_proofs::{blob, word32, word64};
verus! {
pub const REQUEST_CAPACITY: usize = 1024;
pub const PAGE_CAPACITY: usize = 8176;
pub const PAGE_ROWS: usize = 64;
pub const REQUEST_TAG: u32 = 0x3251534d;
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
pub proof fn ordered_push(request: &Request,rows: Seq<WireRow>,row: WireRow)
    requires ordered(request,rows), request.follows_spec(row.key,
        if rows.len() == 0 {opt_bytes(request.cursor)} else {Some(rows.last().key)}),
    ensures ordered(request,rows.push(row)),
{
    hide(cmp_spec); hide(Request::follows_spec);
    let after = rows.push(row);
    assert forall|i: int| 0 <= i < after.len() implies
        request.follows_spec(#[trigger] after[i].key,
            if i == 0 {opt_bytes(request.cursor)} else {Some(after[i-1].key)}) by {
        if i < rows.len() {
            assert(after[i] == rows[i]);
            if i > 0 {assert(after[i-1] == rows[i-1]);}
        } else {
            assert(after[i] == row);
            if i > 0 {assert(after[i-1] == rows.last());}
        }
    }
}
pub proof fn rows_wire_push(rows: Seq<WireRow>,row: WireRow)
    ensures rows_wire(rows.push(row)) == rows_wire(rows)+blob(row.key)+blob(row.value),
{
    hide(blob); hide(word64);
    assert(rows.push(row).drop_last() =~= rows);
    assert(rows.push(row).last() == row);
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
                && bytes.len() <= PAGE_CAPACITY
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
        ensures (status == Status::Ok) == (!old(self).done && count <= PAGE_ROWS && (count > 0 || eof)),
            status == Status::Ok ==> !old(self).done && count <= PAGE_ROWS
                && (count > 0 || eof) && final(self).done == (eof || stopped),
            status != Status::Ok ==> *final(self) == *old(self),
    {
        if self.done || count > PAGE_ROWS || (count == 0 && !eof) { return Status::Invalid; }
        self.done = eof || stopped;
        Status::Ok
    }
}

/// The exact ShardingRequest identity accompanies every bounds/cursor payload.
/// Physical table-handle translation remains a separate foreign ABI adapter.
pub struct ScanIdentity {
    pub transaction: TxnId,
    pub grant: Grant,
    pub table: u64,
    pub fixed_coordinate: bool,
    pub coordinate: Vec<u8>,
}
pub open spec fn same_identity(a: ScanIdentity,b: ScanIdentity) -> bool {
    a.transaction == b.transaction && a.grant == b.grant && a.table == b.table
    && a.fixed_coordinate == b.fixed_coordinate && a.coordinate@ == b.coordinate@
}
impl ScanIdentity {
    pub open spec fn wf(&self) -> bool {
        self.coordinate.len() <= 64 && (self.fixed_coordinate || self.coordinate.len() == 0)
    }
    pub fn matches(&self,transaction: TxnId,grant: Grant,table: u64,fixed: bool,coordinate: &[u8]) -> (yes: bool)
        ensures yes == (self.transaction == transaction && self.grant == grant && self.table == table
            && self.fixed_coordinate == fixed && self.coordinate@ == coordinate@),
    {
        self.transaction.client == transaction.client && self.transaction.sequence == transaction.sequence
        && self.grant.owner == grant.owner && self.grant.epoch == grant.epoch && self.table == table
        && self.fixed_coordinate == fixed && compare(&self.coordinate,coordinate) == 0
    }
}
pub struct BoundRequest { pub identity: ScanIdentity, pub bounds: Request }
pub open spec fn same_bounds(a: Request,b: Request) -> bool {
    a.lo@ == b.lo@ && opt_bytes(a.hi) == opt_bytes(b.hi)
    && opt_bytes(a.cursor) == opt_bytes(b.cursor) && a.reverse == b.reverse
}
pub open spec fn option_payload(a: Option<Vec<u8>>) -> Seq<u8> {
    match a {Some(v) => v@,None => Seq::empty()}
}
pub open spec fn request_flags(r: BoundRequest) -> u32 {
    ((if r.bounds.hi is Some {1u32} else {0u32})
    + (if r.bounds.cursor is Some {2u32} else {0u32})
    + (if r.bounds.reverse {4u32} else {0u32})
    + (if r.identity.fixed_coordinate {8u32} else {0u32})) as u32
}
pub open spec fn request_wire(r: BoundRequest) -> Seq<u8> {
    word32(REQUEST_TAG)+word32(request_flags(r))
    +word64(r.identity.transaction.client)+word64(r.identity.transaction.sequence)
    +word32(r.identity.grant.owner)+word64(r.identity.grant.epoch)+word64(r.identity.table)
    +blob(r.identity.coordinate@)+blob(r.bounds.lo@)
    +blob(option_payload(r.bounds.hi))+blob(option_payload(r.bounds.cursor))
}
pub proof fn wire_sizes(a: u32,b: u64,data: Seq<u8>)
    ensures word32(a).len() == 4, word64(b).len() == 8, blob(data).len() == 8+data.len(),
{}
pub proof fn request_wire_sizes(r: BoundRequest)
    ensures word32(REQUEST_TAG).len() == 4, word32(request_flags(r)).len() == 4,
        word64(r.identity.transaction.client).len() == 8,
        word64(r.identity.transaction.sequence).len() == 8,
        word32(r.identity.grant.owner).len() == 4,
        word64(r.identity.grant.epoch).len() == 8, word64(r.identity.table).len() == 8,
        blob(r.identity.coordinate@).len() == 8+r.identity.coordinate.len(),
        blob(r.bounds.lo@).len() == 8+r.bounds.lo.len(),
        blob(option_payload(r.bounds.hi)).len() == 8+option_payload(r.bounds.hi).len(),
        blob(option_payload(r.bounds.cursor)).len() == 8+option_payload(r.bounds.cursor).len(),
        request_wire(r).len() == 76+r.identity.coordinate.len()+r.bounds.lo.len()
            +option_payload(r.bounds.hi).len()+option_payload(r.bounds.cursor).len(),
{}
pub proof fn request_wire_layout(r: BoundRequest)
    ensures
        crate::routing_codec_proofs::wire_prefix(request_wire(r),0,word32(REQUEST_TAG)),
        crate::routing_codec_proofs::wire_prefix(request_wire(r),4,word32(request_flags(r))),
        crate::routing_codec_proofs::wire_prefix(request_wire(r),8,word64(r.identity.transaction.client)),
        crate::routing_codec_proofs::wire_prefix(request_wire(r),16,word64(r.identity.transaction.sequence)),
        crate::routing_codec_proofs::wire_prefix(request_wire(r),24,word32(r.identity.grant.owner)),
        crate::routing_codec_proofs::wire_prefix(request_wire(r),28,word64(r.identity.grant.epoch)),
        crate::routing_codec_proofs::wire_prefix(request_wire(r),36,word64(r.identity.table)),
        crate::routing_codec_proofs::wire_prefix(request_wire(r),44,blob(r.identity.coordinate@)),
        crate::routing_codec_proofs::wire_prefix(request_wire(r),(52+r.identity.coordinate.len()) as nat,blob(r.bounds.lo@)),
        crate::routing_codec_proofs::wire_prefix(request_wire(r),(60+r.identity.coordinate.len()+r.bounds.lo.len()) as nat,blob(option_payload(r.bounds.hi))),
        crate::routing_codec_proofs::wire_prefix(request_wire(r),(68+r.identity.coordinate.len()+r.bounds.lo.len()+option_payload(r.bounds.hi).len()) as nat,blob(option_payload(r.bounds.cursor))),
{
    hide(word32); hide(word64); hide(blob);
    request_wire_sizes(r);
    let wire = request_wire(r);
    assert(wire.subrange(0,4) =~= word32(REQUEST_TAG));
    assert(wire.subrange(4,8) =~= word32(request_flags(r)));
    assert(wire.subrange(8,16) =~= word64(r.identity.transaction.client));
    assert(wire.subrange(16,24) =~= word64(r.identity.transaction.sequence));
    assert(wire.subrange(24,28) =~= word32(r.identity.grant.owner));
    assert(wire.subrange(28,36) =~= word64(r.identity.grant.epoch));
    assert(wire.subrange(36,44) =~= word64(r.identity.table));
    let lo = 52+r.identity.coordinate.len();
    let hi = lo+8+r.bounds.lo.len();
    let cursor = hi+8+option_payload(r.bounds.hi).len();
    assert(wire.subrange(44,lo) =~= blob(r.identity.coordinate@));
    assert(wire.subrange(lo,hi) =~= blob(r.bounds.lo@));
    assert(wire.subrange(hi,cursor) =~= blob(option_payload(r.bounds.hi)));
    assert(wire.subrange(cursor,wire.len() as int) =~= blob(option_payload(r.bounds.cursor)));
}
pub proof fn request_from_slices(r: BoundRequest,data: Seq<u8>,lo: nat,hi: nat,cursor: nat)
    requires 44 <= lo <= hi <= cursor <= data.len(),
        data.subrange(0,4) == word32(REQUEST_TAG),
        data.subrange(4,8) == word32(request_flags(r)),
        data.subrange(8,16) == word64(r.identity.transaction.client),
        data.subrange(16,24) == word64(r.identity.transaction.sequence),
        data.subrange(24,28) == word32(r.identity.grant.owner),
        data.subrange(28,36) == word64(r.identity.grant.epoch),
        data.subrange(36,44) == word64(r.identity.table),
        data.subrange(44,lo as int) == blob(r.identity.coordinate@),
        data.subrange(lo as int,hi as int) == blob(r.bounds.lo@),
        data.subrange(hi as int,cursor as int) == blob(option_payload(r.bounds.hi)),
        data.subrange(cursor as int,data.len() as int) == blob(option_payload(r.bounds.cursor)),
    ensures data == request_wire(r),
{
    hide(word32); hide(word64); hide(blob);
    crate::routing_codec_proofs::join_wire(data,0,4,8,word32(REQUEST_TAG),word32(request_flags(r)));
    let prefix = word32(REQUEST_TAG)+word32(request_flags(r));
    crate::routing_codec_proofs::join_wire(data,0,8,16,prefix,word64(r.identity.transaction.client));
    let prefix = prefix+word64(r.identity.transaction.client);
    crate::routing_codec_proofs::join_wire(data,0,16,24,prefix,word64(r.identity.transaction.sequence));
    let prefix = prefix+word64(r.identity.transaction.sequence);
    crate::routing_codec_proofs::join_wire(data,0,24,28,prefix,word32(r.identity.grant.owner));
    let prefix = prefix+word32(r.identity.grant.owner);
    crate::routing_codec_proofs::join_wire(data,0,28,36,prefix,word64(r.identity.grant.epoch));
    let prefix = prefix+word64(r.identity.grant.epoch);
    crate::routing_codec_proofs::join_wire(data,0,36,44,prefix,word64(r.identity.table));
    let prefix = prefix+word64(r.identity.table);
    crate::routing_codec_proofs::join_wire(data,0,44,lo,prefix,blob(r.identity.coordinate@));
    let prefix = prefix+blob(r.identity.coordinate@);
    crate::routing_codec_proofs::join_wire(data,0,lo,hi,prefix,blob(r.bounds.lo@));
    let prefix = prefix+blob(r.bounds.lo@);
    crate::routing_codec_proofs::join_wire(data,0,hi,cursor,prefix,blob(option_payload(r.bounds.hi)));
    let prefix = prefix+blob(option_payload(r.bounds.hi));
    crate::routing_codec_proofs::join_wire(data,0,cursor,data.len(),prefix,blob(option_payload(r.bounds.cursor)));
    assert(data.subrange(0,data.len() as int) =~= data);
}
pub proof fn append_blob_payload(prefix: Seq<u8>,data: Seq<u8>,suffix: Seq<u8>)
    ensures (prefix+blob(data)+suffix).subrange(prefix.len() as int+8,prefix.len() as int+8+data.len() as int) == data,
{
    hide(word64);
    wire_sizes(0,data.len() as u64,data);
    assert((prefix+blob(data)+suffix).subrange(prefix.len() as int+8,prefix.len() as int+8+data.len() as int) =~= data);
}

// Allocation-free checked request writer: each primitive advances exactly over
// the bytes it wrote and preserves the remainder of the caller's buffer.
fn copy_into(output: &mut [u8],position: usize,input: &[u8])
    requires position+input.len() <= old(output).len(),
    ensures final(output).len() == old(output).len(),
        final(output)@ == old(output)@.take(position as int)+input@+old(output)@.skip((position+input.len()) as int),
{
    let ghost before = output@;
    let mut i = 0;
    while i < input.len()
        invariant i <= input.len(), position+input.len() <= output.len(), output.len() == before.len(),
            output@ == before.take(position as int)+input@.take(i as int)+before.skip((position+i) as int),
        decreases input.len()-i,
    {
        output[position+i] = input[i];
        i += 1;
        proof { assert(output@ =~= before.take(position as int)+input@.take(i as int)+before.skip((position+i) as int)); }
    }
}
fn put32(output: &mut [u8],position: usize,value: u32)
    requires position+4 <= old(output).len(),
    ensures final(output).len() == old(output).len(),
        final(output)@ == old(output)@.take(position as int)+word32(value)+old(output)@.skip((position+4) as int),
        final(output)@.take((position+4) as int) == old(output)@.take(position as int)+word32(value),
{
    let ghost before = output@;
    let bytes = [value as u8,(value>>8) as u8,(value>>16) as u8,(value>>24) as u8];
    copy_into(output,position,&bytes);
    proof {assert(output@.take((position+4) as int) =~= before.take(position as int)+word32(value));}
}
fn put64(output: &mut [u8],position: usize,value: u64)
    requires position+8 <= old(output).len(),
    ensures final(output).len() == old(output).len(),
        final(output)@ == old(output)@.take(position as int)+word64(value)+old(output)@.skip((position+8) as int),
        final(output)@.take((position+8) as int) == old(output)@.take(position as int)+word64(value),
{
    let ghost before = output@;
    let bytes = [value as u8,(value>>8) as u8,(value>>16) as u8,(value>>24) as u8,
        (value>>32) as u8,(value>>40) as u8,(value>>48) as u8,(value>>56) as u8];
    copy_into(output,position,&bytes);
    proof {assert(output@.take((position+8) as int) =~= before.take(position as int)+word64(value));}
}
fn put_blob(output: &mut [u8],position: usize,value: &[u8]) -> (end: usize)
    requires position+8+value.len() <= old(output).len(),
    ensures end == position+8+value.len(), final(output).len() == old(output).len(),
        final(output)@ == old(output)@.take(position as int)+blob(value@)+old(output)@.skip(end as int),
        final(output)@.take(end as int) == old(output)@.take(position as int)+blob(value@),
{
    let ghost before = output@;
    put64(output,position,value.len() as u64);
    copy_into(output,position+8,value);
    proof {assert(output@.take((position+8+value.len()) as int) =~= before.take(position as int)+blob(value@));}
    position+8+value.len()
}
impl BoundRequest {
    pub fn encode(&self,output: &mut [u8]) -> (out: Result<usize,Status>)
        ensures final(output).len() == old(output).len(),
            match out {Ok(n) => n <= REQUEST_CAPACITY && n <= final(output).len()
                && final(output)@.take(n as int) == request_wire(*self),Err(_) => true},
    {
        hide(cmp_spec); hide(word32); hide(word64); hide(blob);
        proof { request_wire_sizes(*self); }
        let hi = match &self.bounds.hi {Some(v) => v.as_slice(),None => &[]};
        let cursor = match &self.bounds.cursor {Some(v) => v.as_slice(),None => &[]};
        proof {
            assert(hi@ =~= option_payload(self.bounds.hi));
            assert(cursor@ =~= option_payload(self.bounds.cursor));
        }
        if self.identity.coordinate.len() > 64
            || (!self.identity.fixed_coordinate && self.identity.coordinate.len() != 0) {
            return Err(Status::Invalid);
        }
        let mut length = 76usize+self.identity.coordinate.len();
        if self.bounds.lo.len() > REQUEST_CAPACITY-length {return Err(Status::Exhausted);}
        length += self.bounds.lo.len();
        if hi.len() > REQUEST_CAPACITY-length {return Err(Status::Exhausted);}
        length += hi.len();
        if cursor.len() > REQUEST_CAPACITY-length {return Err(Status::Exhausted);}
        length += cursor.len();
        if output.len() < length {return Err(Status::Exhausted);}
        let flags = (if self.bounds.hi.is_some() {1u32} else {0u32})
            +(if self.bounds.cursor.is_some() {2u32} else {0u32})
            +(if self.bounds.reverse {4u32} else {0u32})
            +(if self.identity.fixed_coordinate {8u32} else {0u32});
        put32(output,0,REQUEST_TAG);
        proof {assert(output@.take(4) =~= word32(REQUEST_TAG));}
        put32(output,4,flags);
        put64(output,8,self.identity.transaction.client);
        put64(output,16,self.identity.transaction.sequence);
        put32(output,24,self.identity.grant.owner);
        put64(output,28,self.identity.grant.epoch);
        put64(output,36,self.identity.table);
        let offset = put_blob(output,44,&self.identity.coordinate);
        let offset = put_blob(output,offset,&self.bounds.lo);
        let offset = put_blob(output,offset,hi);
        let offset = put_blob(output,offset,cursor);
        proof { assert(output@.take(offset as int) =~= request_wire(*self)); }
        Ok(offset)
    }
    pub fn decode(bytes: &[u8]) -> (out: Result<Self,Status>)
        ensures match out {Ok(r) => r.bounds.wf() && r.identity.wf()
            && bytes@ == request_wire(r),Err(_) => true},
            (exists|r: BoundRequest| r.bounds.wf() && r.identity.wf()
                && bytes@ == request_wire(r) && bytes.len() <= REQUEST_CAPACITY) ==> out is Ok,
    {
        hide(cmp_spec); hide(word32); hide(word64); hide(blob);
        hide(crate::routing_codec_proofs::wire_prefix);
        hide(request_wire);
        let ghost has_expected = exists|r: BoundRequest| r.bounds.wf() && r.identity.wf()
            && bytes@ == request_wire(r) && bytes.len() <= REQUEST_CAPACITY;
        let ghost expected = if has_expected {
            Some(choose|r: BoundRequest| r.bounds.wf() && r.identity.wf()
                && bytes@ == request_wire(r) && bytes.len() <= REQUEST_CAPACITY)
        } else {None};
        proof { if has_expected {
            let expected = expected.unwrap();
            request_wire_sizes(expected); request_wire_layout(expected);
        } }
        if bytes.len() > REQUEST_CAPACITY {return Err(Status::Invalid);}
        let mut reader = Reader::new(bytes);
        proof { if has_expected {
            assert(crate::routing_codec_proofs::wire_prefix(bytes@,0,word32(REQUEST_TAG)));
        } }
        let tag = match reader.read_u32() {Ok(v) => v,Err(e) => return Err(e)};
        proof {reader.position_bound(); assert(reader.position() == 4);}
        if tag != REQUEST_TAG {return Err(Status::Invalid);}
        proof { if has_expected {
            let expected = expected.unwrap();
            assert(crate::routing_codec_proofs::wire_prefix(bytes@,reader.position(),word32(request_flags(expected))));
        } }
        let flags = match reader.read_u32() {Ok(v) => v,Err(e) => return Err(e)};
        proof {reader.position_bound(); assert(reader.position() == 8);}
        if flags > 15 {return Err(Status::Invalid);}
        proof { if has_expected {
            let expected = expected.unwrap();
            assert(crate::routing_codec_proofs::wire_prefix(bytes@,reader.position(),word64(expected.identity.transaction.client)));
        } }
        let client = match reader.read_u64() {Ok(v) => v,Err(e) => return Err(e)};
        proof {reader.position_bound(); assert(reader.position() == 16);}
        proof { if has_expected {
            let expected = expected.unwrap();
            assert(crate::routing_codec_proofs::wire_prefix(bytes@,reader.position(),word64(expected.identity.transaction.sequence)));
        } }
        let sequence = match reader.read_u64() {Ok(v) => v,Err(e) => return Err(e)};
        proof {reader.position_bound(); assert(reader.position() == 24);}
        proof { if has_expected {
            let expected = expected.unwrap();
            assert(crate::routing_codec_proofs::wire_prefix(bytes@,reader.position(),word32(expected.identity.grant.owner)));
        } }
        let owner = match reader.read_u32() {Ok(v) => v,Err(e) => return Err(e)};
        proof {reader.position_bound(); assert(reader.position() == 28);}
        proof { if has_expected {
            let expected = expected.unwrap();
            assert(crate::routing_codec_proofs::wire_prefix(bytes@,reader.position(),word64(expected.identity.grant.epoch)));
        } }
        let epoch = match reader.read_u64() {Ok(v) => v,Err(e) => return Err(e)};
        proof {reader.position_bound(); assert(reader.position() == 36);}
        proof { if has_expected {
            let expected = expected.unwrap();
            assert(crate::routing_codec_proofs::wire_prefix(bytes@,reader.position(),word64(expected.identity.table)));
        } }
        let table = match reader.read_u64() {Ok(v) => v,Err(e) => return Err(e)};
        proof {reader.position_bound(); assert(reader.position() == 44);}
        proof { if has_expected {
            let expected = expected.unwrap();
            assert(crate::routing_codec_proofs::wire_prefix(bytes@,reader.position(),blob(expected.identity.coordinate@)));
        } }
        let coordinate = match reader.read_bytes() {Ok(v) => v,Err(e) => return Err(e)};
        proof {reader.position_bound();}
        let ghost lo_position = reader.position();
        proof { if has_expected {
            let expected = expected.unwrap();
            assert(coordinate@ == expected.identity.coordinate@);
            assert(lo_position == 52+expected.identity.coordinate.len());
            assert(crate::routing_codec_proofs::wire_prefix(bytes@,reader.position(),blob(expected.bounds.lo@)));
        } }
        let lo = match reader.read_bytes() {Ok(v) => v,Err(e) => return Err(e)};
        proof {reader.position_bound();}
        let ghost hi_position = reader.position();
        proof { if has_expected {
            let expected = expected.unwrap();
            assert(lo@ == expected.bounds.lo@);
            assert(hi_position == 60+expected.identity.coordinate.len()+expected.bounds.lo.len());
            assert(crate::routing_codec_proofs::wire_prefix(bytes@,reader.position(),blob(option_payload(expected.bounds.hi))));
        } }
        let hi = match reader.read_bytes() {Ok(v) => v,Err(e) => return Err(e)};
        proof {reader.position_bound();}
        let ghost cursor_position = reader.position();
        proof { if has_expected {
            let expected = expected.unwrap();
            assert(hi@ == option_payload(expected.bounds.hi));
            assert(cursor_position == 68+expected.identity.coordinate.len()+expected.bounds.lo.len()
                +option_payload(expected.bounds.hi).len());
            assert(crate::routing_codec_proofs::wire_prefix(bytes@,reader.position(),blob(option_payload(expected.bounds.cursor))));
        } }
        let cursor = match reader.read_bytes() {Ok(v) => v,Err(e) => return Err(e)};
        proof {reader.position_bound(); assert(44 <= lo_position <= hi_position <= cursor_position <= reader.position());}
        proof { if has_expected {
            let expected = expected.unwrap();
            assert(cursor@ == option_payload(expected.bounds.cursor));
            assert(reader.position() == request_wire(expected).len());
            assert(reader.position() == bytes@.len());
        } }
        let ghost end = reader.position();
        let end_status = reader.finish();
        match end_status {
            Status::Ok => {},
            _ => return Err(Status::Invalid),
        }
        proof {
            assert(reader.data() == bytes@);
            assert(end == bytes@.len());
        }
        let has_hi = flags % 2 == 1;
        let has_cursor = (flags / 2) % 2 == 1;
        let reverse = (flags / 4) % 2 == 1;
        let fixed = flags / 8 == 1;
        if (!has_hi && hi.len() != 0) || (!has_cursor && cursor.len() != 0)
            || coordinate.len() > 64 || (!fixed && coordinate.len() != 0) {
            return Err(Status::Invalid);
        }
        let ghost hi_payload = hi@;
        let ghost cursor_payload = cursor@;
        let out = Self {
            identity: ScanIdentity {transaction: TxnId {client,sequence},
                grant: Grant {owner,epoch},table,fixed_coordinate: fixed,coordinate},
            bounds: Request {lo,hi: if has_hi {Some(hi)} else {None},
                cursor: if has_cursor {Some(cursor)} else {None},reverse},
        };
        proof {
            assert(hi_payload =~= option_payload(out.bounds.hi));
            assert(cursor_payload =~= option_payload(out.bounds.cursor));
            assert(flags == request_flags(out));
            request_from_slices(out,bytes@,lo_position,hi_position,cursor_position);
            if has_expected { crate::full_scan_proofs::request_roundtrip(out,expected.unwrap()); }
        }
        if !out.bounds.valid() {return Err(Status::Invalid);}
        Ok(out)
    }
}

pub proof fn blob_prefix_unique(a: Seq<u8>,b: Seq<u8>,x: Seq<u8>,y: Seq<u8>)
    requires a.len() <= u64::MAX, b.len() <= u64::MAX, blob(a)+x == blob(b)+y,
    ensures a == b, x == y,
{
    hide(word64);
    wire_sizes(0,a.len() as u64,a);
    wire_sizes(0,b.len() as u64,b);
    assert((blob(a)+x).take(8) =~= word64(a.len() as u64));
    assert((blob(b)+y).take(8) =~= word64(b.len() as u64));
    crate::routing_codec_proofs::word64_injective(a.len() as u64,b.len() as u64);
    assert(a =~= b) by {
        assert(a == (blob(a)+x).subrange(8,8+a.len() as int));
        assert(b == (blob(b)+y).subrange(8,8+b.len() as int));
    }
    assert((blob(a)+x).skip(8+a.len() as int) =~= x);
    assert((blob(b)+y).skip(8+b.len() as int) =~= y);
    assert(x =~= y);
}
pub proof fn rows_head(rows: Seq<WireRow>)
    requires rows.len() > 0,
    ensures rows_wire(rows) == blob(rows[0].key)+blob(rows[0].value)+rows_wire(rows.skip(1)),
    decreases rows.len(),
{
    if rows.len() > 1 {
        rows_head(rows.drop_last());
        assert(rows.drop_last().skip(1) =~= rows.skip(1).drop_last());
        assert(rows.skip(1).last() == rows.last());
    } else {
        assert(rows.drop_last() =~= Seq::<WireRow>::empty());
        assert(rows.skip(1) =~= Seq::<WireRow>::empty());
    }
}
pub proof fn row_fits_wire(rows: Seq<WireRow>,i: int)
    requires 0 <= i < rows.len(),
    ensures rows[i].key.len()+rows[i].value.len()+16 <= rows_wire(rows).len(),
    decreases rows.len(),
{
    if i < rows.len()-1 { row_fits_wire(rows.drop_last(),i); }
}
pub proof fn decoded_row(data: Seq<u8>,begin: nat,middle: nat,end: nat,
    rows: Seq<WireRow>,key: Seq<u8>,value: Seq<u8>)
    requires rows.len() > 0, begin <= middle <= end <= data.len(), data.len() <= PAGE_CAPACITY,
        data.skip(begin as int) == rows_wire(rows),
        data.subrange(begin as int,middle as int) == blob(key),
        data.subrange(middle as int,end as int) == blob(value),
    ensures key == rows[0].key, value == rows[0].value,
        data.skip(end as int) == rows_wire(rows.skip(1)),
{
    hide(word32); hide(word64); hide(blob); hide(rows_wire);
    wire_sizes(0,0,key); wire_sizes(0,0,value);
    rows_head(rows); row_fits_wire(rows,0);
    crate::routing_codec_proofs::join_wire(data,begin,middle,end,blob(key),blob(value));
    assert(data.skip(begin as int) =~= blob(key)+(blob(value)+data.skip(end as int)));
    assert(rows_wire(rows) =~= blob(rows[0].key)+(blob(rows[0].value)+rows_wire(rows.skip(1))));
    blob_prefix_unique(key,rows[0].key,blob(value)+data.skip(end as int),
        blob(rows[0].value)+rows_wire(rows.skip(1)));
    blob_prefix_unique(value,rows[0].value,data.skip(end as int),rows_wire(rows.skip(1)));
}

/// The only object which exposes decoded rows to the callback adapter.
/// `taken` records exactly the slice pairs returned by `next`, including the
/// row whose callback requests stopping. No callback can observe an invalid
/// page: construction validates the entire page before returning this object.
pub struct PageCursor<'a> {
    reader: Reader<'a>,
    count: usize,
    eof: bool,
    last: Option<&'a [u8]>,
    index: usize,
    stopped: bool,
    query: Ghost<Request>,
    expected: Ghost<Seq<WireRow>>,
    taken: Ghost<Seq<WireRow>>,
}
pub struct PageCursorView {
    pub count: usize, pub eof: bool, pub index: usize, pub stopped: bool,
    pub query: Ghost<Request>, pub expected: Ghost<Seq<WireRow>>, pub taken: Ghost<Seq<WireRow>>,
}
impl<'a> View for PageCursor<'a> {
    type V = PageCursorView;
    closed spec fn view(&self) -> PageCursorView {
        PageCursorView {count:self.count,eof:self.eof,index:self.index,stopped:self.stopped,
            query:self.query,expected:self.expected,taken:self.taken}
    }
}
impl<'a> PageCursor<'a> {
    pub closed spec fn wf(&self) -> bool {
        self.reader.wf() && self.count == self.expected@.len() && self.index <= self.count
        && self.reader.data().len() <= PAGE_CAPACITY
        && self.count <= PAGE_ROWS && (self.count == 0 ==> self.eof)
        && self.taken@ == self.expected@.take(self.index as int)
        && self.reader.data().skip(self.reader.position() as int) == rows_wire(self.expected@.skip(self.index as int))
        && ordered(&self.query@,self.expected@)
        && (forall|i: int| 0 <= i < self.expected@.len() ==>
            self.expected@[i].key.len() <= PAGE_CAPACITY && self.expected@[i].value.len() <= PAGE_CAPACITY)
        && match self.last {Some(k) => self.count > 0 && k@ == self.expected@.last().key,
            None => self.count == 0}
    }
    pub open spec fn is_stopped(&self) -> bool { self@.stopped }
    pub open spec fn is_eof(&self) -> bool { self@.eof }
    pub open spec fn consumed(&self) -> nat { self@.index as nat }
    pub fn next(&mut self) -> (out: Result<Option<(&'a [u8],&'a [u8])>,Status>)
        requires old(self).wf(),
        ensures out is Ok ==> final(self).wf(),
            final(self)@.query == old(self)@.query, final(self)@.expected == old(self)@.expected,
            final(self)@.stopped == old(self)@.stopped, final(self)@.eof == old(self)@.eof,
            match out {
                Ok(Some((k,v))) => !old(self)@.stopped && old(self)@.index < old(self)@.count
                    && final(self)@.index == old(self)@.index+1
                    && k@ == old(self)@.expected@[old(self)@.index as int].key
                    && v@ == old(self)@.expected@[old(self)@.index as int].value
                    && final(self)@.taken@ == old(self)@.taken@.push(WireRow {key:k@,value:v@}),
                Ok(None) => *final(self) == *old(self) && (final(self)@.stopped || final(self)@.index == final(self)@.count),
                Err(_) => true,
            },
    {
        hide(cmp_spec); hide(word32); hide(word64); hide(blob);
        hide(rows_wire);
        hide(ordered);
        if self.stopped || self.index == self.count {return Ok(None);}
        let ghost begin = self.reader.position();
        let ghost rest = self.expected@.skip(self.index as int);
        let key = match read_blob(&mut self.reader) {Ok(v) => v,Err(e) => return Err(e)};
        let ghost middle = self.reader.position();
        let value = match read_blob(&mut self.reader) {Ok(v) => v,Err(e) => return Err(e)};
        proof {
            self.reader.position_bound();
            decoded_row(self.reader.data(),begin,middle,self.reader.position(),rest,key@,value@);
            assert(rest[0] == self.expected@[self.index as int]);
            assert(WireRow {key:key@,value:value@} == self.expected@[self.index as int]);
            assert(rest.skip(1) =~= self.expected@.skip(self.index as int+1));
            self.taken@ = self.taken@.push(WireRow {key:key@,value:value@});
        }
        self.index += 1;
        proof { assert(self.taken@ =~= self.expected@.take(self.index as int)); }
        Ok(Some((key,value)))
    }
    pub fn stop(&mut self)
        requires old(self).wf(), old(self).consumed() > 0,
        ensures final(self).wf(), final(self).is_stopped(), final(self)@.taken == old(self)@.taken,
            final(self)@.query == old(self)@.query, final(self)@.expected == old(self)@.expected,
            final(self).is_eof() == old(self).is_eof(), final(self).consumed() == old(self).consumed(),
    { self.stopped = true; }
}

pub struct FullScan {
    pub request: BoundRequest,
    pub completion: Completion,
    pub delivered: Ghost<Seq<WireRow>>,
}
impl FullScan {
    pub fn new(request: BoundRequest) -> (out: Self)
        requires request.bounds.wf(), request.identity.wf(),
        ensures out.request == request, !out.completion.done, out.delivered@ == Seq::<WireRow>::empty(),
    { Self {request,completion: Completion {done:false},delivered: Ghost(Seq::empty())} }
    pub fn begin_page<'a>(&self,bytes: &'a [u8]) -> (out: Result<PageCursor<'a>,Status>)
        requires self.request.bounds.wf(),
        ensures match out {
            Ok(p) => !self.completion.done && p.wf() && !p.is_stopped() && p.consumed() == 0
                && p@.query@ == self.request.bounds && p@.taken@ == Seq::<WireRow>::empty()
                && bytes@ == word32(PAGE_TAG)+word32(p@.expected@.len() as u32)
                    +word32(if p.is_eof() {1u32} else {0u32})+rows_wire(p@.expected@),
            Err(_) => true,
        },
    {
        hide(cmp_spec); hide(word32); hide(word64); hide(blob);
        if self.completion.done {return Err(Status::Invalid);}
        let summary = match self.request.bounds.validate_page(bytes) {Ok(p) => p,Err(e) => return Err(e)};
        let mut reader = Reader::new(bytes);
        match reader.read_slice(12) {Ok(_) => {},Err(e) => return Err(e)}
        proof {
            wire_sizes(PAGE_TAG,0,Seq::empty());
            wire_sizes(summary.count as u32,0,Seq::empty());
            wire_sizes(if summary.eof {1u32} else {0u32},0,Seq::empty());
            assert(reader.data().skip(12) =~= rows_wire(summary.rows@));
            assert(summary.rows@.skip(0) =~= summary.rows@);
            assert(summary.rows@.take(0) =~= Seq::<WireRow>::empty());
            assert forall|i: int| 0 <= i < summary.rows@.len() implies
                summary.rows@[i].key.len() <= PAGE_CAPACITY && summary.rows@[i].value.len() <= PAGE_CAPACITY by {
                row_fits_wire(summary.rows@,i);
            }
        }
        Ok(PageCursor {reader,count:summary.count,eof:summary.eof,last:summary.last,index:0,stopped:false,
            query:Ghost(self.request.bounds),expected:summary.rows,taken:Ghost(Seq::empty())})
    }
    pub fn finish_page(&mut self,page: &PageCursor) -> (out: Result<bool,Status>)
        requires old(self).request.bounds.wf(), page.wf(), page@.query@ == old(self).request.bounds,
        ensures final(self).request.identity == old(self).request.identity,
            crate::full_scan_proofs::consume_effect(*old(self),*final(self),page@.expected@,page@.taken@,
                page.is_eof(),page.is_stopped(),out),
            final(self).request.bounds.lo == old(self).request.bounds.lo,
            final(self).request.bounds.hi == old(self).request.bounds.hi,
            final(self).request.bounds.reverse == old(self).request.bounds.reverse,
            match out {
                Ok(done) => !old(self).completion.done && done == final(self).completion.done
                    && done == (page.is_eof() || page.is_stopped())
                    && (page.is_stopped() || page@.taken@ == page@.expected@)
                    && final(self).delivered@ == old(self).delivered@+page@.taken@
                    && opt_bytes(final(self).request.bounds.cursor) ==
                        if page.is_stopped() || page@.expected@.len() == 0 {opt_bytes(old(self).request.bounds.cursor)}
                        else {Some(page@.expected@.last().key)},
                Err(_) => *final(self) == *old(self),
            },
    {
        hide(cmp_spec); hide(rows_wire); hide(blob); hide(word32); hide(word64);
        let ghost before = *self;
        if self.completion.done || (!page.stopped && page.index != page.count) {return Err(Status::Invalid);}
        if !page.stopped {
            proof {assert(page.expected@.take(page.index as int) =~= page.expected@);}
            if let Some(last) = page.last {
                proof { crate::full_scan_proofs::ordered_all(&self.request.bounds,page.expected@); }
                let status = self.request.bounds.advance(last);
                proof { assert(status == Status::Ok); }
            }
        }
        let status = self.completion.complete(page.count,page.eof,page.stopped);
        proof {
            assert(status == Status::Ok);
            self.delivered@ = self.delivered@+page.taken@;
            assert(crate::full_scan_proofs::consume_effect(before,*self,page.expected@,page.taken@,
                page.eof,page.stopped,Ok(self.completion.done)));
        }
        Ok(self.completion.done)
    }
}

pub open spec fn page_wire(rows: Seq<WireRow>,eof: bool) -> Seq<u8> {
    word32(PAGE_TAG)+word32(rows.len() as u32)+word32(if eof {1u32} else {0u32})+rows_wire(rows)
}
/// Same-source engine callback serializer. Its proof records only callbacks
/// actually supplied by the engine; engine completeness is a separate premise.
pub struct Page {
    pub request: BoundRequest,
    bytes: crate::routing_codec::Writer,
    last_offset: usize,
    last_length: usize,
    pub budget: PageBudget,
    pub error: Option<Status>,
    pub maximum: Maximum,
    rows: Ghost<Seq<WireRow>>,
}
pub struct PageView {
    pub request: BoundRequest, pub budget: PageBudget,
    pub error: Option<Status>, pub rows: Ghost<Seq<WireRow>>,
}
impl View for Page {
    type V = PageView;
    closed spec fn view(&self) -> PageView {
        PageView {request:self.request,budget:self.budget,error:self.error,rows:self.rows}
    }
}
impl Page {
    pub closed spec fn wf(&self) -> bool {
        self.request.bounds.wf() && self.budget.wf() && self.maximum.wf()
        && self.budget.count == self.rows@.len() && ordered(&self.request.bounds,self.rows@)
        && self.bytes.view() == word32(PAGE_TAG)+word32(0)+word32(0)+rows_wire(self.rows@)
        && self.bytes.view().len() == self.budget.used
        && (self.rows@.len() > 0 ==> self.last_offset+self.last_length <= self.bytes.view().len()
            && self.bytes.view().subrange(self.last_offset as int,(self.last_offset+self.last_length) as int)
                == self.rows@.last().key)
    }
    pub proof fn facts(&self)
        requires self.wf(),
        ensures self@.request.bounds.wf(), self@.budget.wf(),
            self@.budget.count == self@.rows@.len(),
            ordered(&self@.request.bounds,self@.rows@),
            rows_wire(self@.rows@).len()+12 == self@.budget.used,
    {}
    pub fn new(request: BoundRequest) -> (out: Result<Self,Status>)
        requires request.bounds.wf(),
        ensures match out {Ok(p) => p.wf() && p@.request == request && p@.rows@ == Seq::<WireRow>::empty()
            && p@.error is None && !p@.budget.more,Err(_) => true},
    {
        let mut bytes = match crate::routing_codec::Writer::with_capacity(PAGE_CAPACITY) {
            Ok(v) => v,Err(e) => return Err(e),
        };
        let a = bytes.write_u32(PAGE_TAG);
        let b = bytes.write_u32(0);
        let c = bytes.write_u32(0);
        proof {assert(a == Status::Ok && b == Status::Ok && c == Status::Ok);}
        Ok(Self {request,bytes,last_offset:0,last_length:0,budget:PageBudget::new(),
            error:None,maximum:Maximum::new(),rows:Ghost(Seq::empty())})
    }
    pub fn add(&mut self,key: &[u8],value: &[u8]) -> (out: Result<bool,Status>)
        requires old(self).wf(),
        ensures final(self).wf(), final(self)@.request == old(self)@.request,
            crate::full_scan_proofs::add_effect(*old(self),*final(self),WireRow {key:key@,value:value@},out),
            match out {
                Ok(true) => final(self)@.rows@ == old(self)@.rows@.push(WireRow {key:key@,value:value@})
                    && final(self)@.error is None && !final(self)@.budget.more,
                Ok(false) => final(self)@.rows == old(self)@.rows && final(self)@.budget.more && final(self)@.error is None,
                Err(_) => final(self)@.rows == old(self)@.rows && final(self)@.error is Some,
            },
    {
        hide(cmp_spec); hide(word32); hide(word64); hide(blob); hide(rows_wire);
        hide(ordered); hide(Request::follows_spec); hide(Request::wf); hide(Maximum::wf);
        proof {wire_sizes(PAGE_TAG,0,key@); wire_sizes(0,0,value@);}
        if self.error.is_some() {return Err(Status::Invalid);}
        let previous = if self.budget.count == 0 {
            match &self.request.bounds.cursor {Some(v) => Some(v.as_slice()),None => None}
        } else {Some(&self.bytes.as_bytes()[self.last_offset..self.last_offset+self.last_length])};
        if !self.request.bounds.follows(key,previous) {
            self.error = Some(Status::Invalid); return Err(Status::Invalid);
        }
        let ghost before = self.rows@;
        match self.budget.reserve(key.len(),value.len()) {
            Status::Ok => {},
            Status::NotFound => return Ok(false),
            error => {self.error = Some(error); return Err(error);},
        }
        proof {
            ordered_push(&self.request.bounds,before,WireRow {key:key@,value:value@});
            rows_wire_push(before,WireRow {key:key@,value:value@});
        }
        self.last_offset = self.bytes.len()+8;
        self.last_length = key.len();
        let ghost old_bytes = self.bytes.view();
        let a = self.bytes.write_bytes(key);
        let b = self.bytes.write_bytes(value);
        proof {
            assert(a == Status::Ok && b == Status::Ok);
            self.rows@ = before.push(WireRow {key:key@,value:value@});
            assert(self.rows@.drop_last() == before);
            assert(self.rows@.last().key == key@ && self.rows@.last().value == value@);
            assert(self.bytes.view() =~= word32(PAGE_TAG)+word32(0)+word32(0)+rows_wire(self.rows@));
            assert(self.bytes.view().len() == self.budget.used);
            append_blob_payload(old_bytes,key@,blob(value@));
            assert(self.bytes.view().subrange(self.last_offset as int,(self.last_offset+self.last_length) as int) =~= key@);
        }
        Ok(true)
    }
    pub fn finish(&self,output: &mut [u8]) -> (out: Result<usize,Status>)
        requires self.wf(),
        ensures final(output).len() == old(output).len(),
            match out {Ok(n) => self@.error is None && n <= PAGE_CAPACITY && n <= final(output).len()
                && final(output)@.take(n as int) == page_wire(self@.rows@,!self@.budget.more),
                Err(_) => true},
    {
        hide(cmp_spec); hide(ordered); hide(word32); hide(word64); hide(blob); hide(rows_wire);
        proof {
            wire_sizes(PAGE_TAG,0,Seq::empty());
            wire_sizes(0,0,Seq::empty());
            wire_sizes(self.budget.count as u32,0,Seq::empty());
            wire_sizes(if self.budget.more {0u32} else {1u32},0,Seq::empty());
        }
        if let Some(error) = self.error {return Err(error);}
        if output.len() < self.bytes.len() {return Err(Status::Exhausted);}
        copy_into(output,0,self.bytes.as_bytes());
        put32(output,4,self.budget.count as u32);
        put32(output,8,if self.budget.more {0u32} else {1u32});
        proof {assert(output@.take(self.bytes.view().len() as int) =~= page_wire(self.rows@,!self.budget.more));}
        Ok(self.bytes.len())
    }
}
}
