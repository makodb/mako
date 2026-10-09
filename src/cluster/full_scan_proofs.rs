//! Sharding composition, not an OCC/MVCC or native-engine correctness proof.
//! The trusted interface is complete raw engine enumeration at an exact
//! transaction/canonical physical address. Sharding page coverage/EOF is derived.
//! All pages of a successful scan refer to ONE transaction snapshot. A caller
//! stopping its callback is not a complete query. Errors abort this certificate.
#![cfg(verus_keep_ghost)]
use vstd::prelude::*;
use crate::bytes::{cmp_spec, cmp_laws, cmp_trans};
use crate::directory::{RouteTable, inside, hi_view, route};
use crate::routing::ScanSegment;
use crate::routing_proofs::{segment_step, reverse_segment_step,
    segment_exact_partition, reverse_segment_exact_partition};
use crate::full_scan_core::{Request, WireRow, PageSummary, ordered, opt_bytes, PAGE_ROWS};
use crate::full_scan_core::{request_wire, request_flags, option_payload, blob_prefix_unique,
    rows_wire, rows_head, row_fits_wire, page_wire};
use crate::routing_codec_proofs::{word32, word64, blob, word32_injective, word64_injective};
use crate::types::{Grant, Status, TxnId};
use crate::leases::SessionView;
use crate::storage::Image;
verus! {

pub type SnapshotRows = vstd::imap::IMap<Seq<u8>, Seq<u8>>;

/// No encoding, sentinel, string conversion, or nonempty-key assumption.
pub open spec fn precedes(reverse: bool, a: Seq<u8>, b: Seq<u8>) -> bool {
    if reverse { cmp_spec(b,a) < 0 } else { cmp_spec(a,b) < 0 }
}
pub open spec fn sorted(reverse: bool, rows: Seq<WireRow>) -> bool {
    forall|i: int,j: int| 0 <= i < j < rows.len() ==>
        precedes(reverse,rows[i].key,rows[j].key)
}
pub open spec fn contains_row(rows: Seq<WireRow>, k: Seq<u8>, v: Seq<u8>) -> bool {
    exists|i: int| 0 <= i < rows.len() && rows[i].key == k && rows[i].value == v
}
pub open spec fn eligible(q: &Request, cursor: Option<Seq<u8>>, k: Seq<u8>) -> bool {
    q.follows_spec(k,cursor)
}
pub open spec fn last_cursor(rows: Seq<WireRow>, old: Option<Seq<u8>>) -> Option<Seq<u8>> {
    if rows.len() == 0 { old } else { Some(rows.last().key) }
}
pub open spec fn exact(q: &Request, cursor: Option<Seq<u8>>, image: SnapshotRows,
    rows: Seq<WireRow>) -> bool {
    sorted(q.reverse,rows)
    && (forall|k: Seq<u8>,v: Seq<u8>| contains_row(rows,k,v) <==>
        image.contains_key(k) && image[k] == v && eligible(q,cursor,k))
}

/// Complete-prefix property consumed by the page induction, NOT a trusted
/// premise of the native theorem: assembled_engine_page derives it from raw
/// engine enumeration and every checked Page::add outcome.
/// Absence is exact, and short non-EOF pages are allowed independently of count.
pub open spec fn page_complete(q: &Request, image: SnapshotRows,
    rows: Seq<WireRow>, eof: bool) -> bool {
    ordered(q,rows) && rows.len() <= PAGE_ROWS && (rows.len() == 0 ==> eof)
    && (forall|k: Seq<u8>,v: Seq<u8>| contains_row(rows,k,v) <==>
        image.contains_key(k) && image[k] == v && eligible(q,opt_bytes(q.cursor),k)
        && (eof || !eligible(q,last_cursor(rows,opt_bytes(q.cursor)),k)))
}

pub struct PageEvent { pub request: Request, pub rows: Seq<WireRow>, pub eof: bool,
    pub status: Status, pub stopped: bool }

/// Transition transcript: only a successful, non-stopped page can consume
/// rows; only its truthful EOF can finish. An error has no successor here.
pub open spec fn pages(q: &Request, image: SnapshotRows, events: Seq<PageEvent>) -> bool
    decreases events.len(),
{
    events.len() > 0 && events[0].request.lo@ == q.lo@
    && opt_bytes(events[0].request.hi) == opt_bytes(q.hi)
    && opt_bytes(events[0].request.cursor) == opt_bytes(q.cursor)
    && events[0].request.reverse == q.reverse
    && events[0].status == Status::Ok && !events[0].stopped
    && page_complete(q,image,events[0].rows,events[0].eof)
    && if events[0].eof { events.len() == 1 } else {
        events.len() > 1
        && events[1].request.lo@ == q.lo@
        && opt_bytes(events[1].request.hi) == opt_bytes(q.hi)
        && events[1].request.reverse == q.reverse
        && opt_bytes(events[1].request.cursor) == Some(events[0].rows.last().key)
        && pages(&events[1].request,image,events.skip(1))
    }
}
pub open spec fn page_rows(events: Seq<PageEvent>) -> Seq<WireRow>
    decreases events.len(),
{
    if events.len() == 0 { Seq::empty() }
    else { events[0].rows + page_rows(events.skip(1)) }
}

pub proof fn cmp_symmetry(a: Seq<u8>,b: Seq<u8>)
    ensures cmp_spec(a,b) == -cmp_spec(b,a), (cmp_spec(a,b) == 0) == (a == b),
{
    hide(cmp_spec); hide(crate::bytes::lex_lt); hide(crate::bytes::prefix);
    cmp_laws(a,b);
}
pub proof fn precedes_trans(reverse: bool,a: Seq<u8>,b: Seq<u8>,c: Seq<u8>)
    requires precedes(reverse,a,b), precedes(reverse,b,c),
    ensures precedes(reverse,a,c),
{
    hide(cmp_spec);
    if reverse { cmp_trans(c,b,a); } else { cmp_trans(a,b,c); }
}
pub proof fn ordered_pair(q: &Request,rows: Seq<WireRow>,i: int,j: int)
    requires ordered(q,rows), 0 <= i < j < rows.len(),
    ensures precedes(q.reverse,rows[i].key,rows[j].key),
    decreases j-i,
{
    hide(cmp_spec);
    cmp_symmetry(rows[j].key,rows[j-1].key);
    assert(q.follows_spec(rows[j].key,Some(rows[j-1].key)));
    if i < j-1 {
        ordered_pair(q,rows,i,j-1);
        precedes_trans(q.reverse,rows[i].key,rows[j-1].key,rows[j].key);
    }
}
pub proof fn ordered_all(q: &Request,rows: Seq<WireRow>)
    requires ordered(q,rows),
    ensures sorted(q.reverse,rows),
        forall|i: int| 0 <= i < rows.len() ==> eligible(q,opt_bytes(q.cursor),rows[i].key),
{
    hide(cmp_spec);
    assert forall|i: int,j: int| 0 <= i < j < rows.len() implies
        precedes(q.reverse,rows[i].key,rows[j].key) by { ordered_pair(q,rows,i,j); }
    assert forall|i: int| 0 <= i < rows.len() implies eligible(q,opt_bytes(q.cursor),rows[i].key) by {
        assert(q.follows_spec(rows[i].key,if i == 0 {opt_bytes(q.cursor)} else {Some(rows[i-1].key)}));
        if i > 0 {
            ordered_pair(q,rows,0,i);
            if opt_bytes(q.cursor) is Some {
                cmp_laws(rows[0].key,opt_bytes(q.cursor).unwrap());
                cmp_laws(rows[i].key,opt_bytes(q.cursor).unwrap());
                precedes_trans(q.reverse,opt_bytes(q.cursor).unwrap(),rows[0].key,rows[i].key);
            }
        }
    }
}
pub proof fn concat_has(a: Seq<WireRow>,b: Seq<WireRow>,k: Seq<u8>,v: Seq<u8>)
    ensures contains_row(a+b,k,v) == (contains_row(a,k,v) || contains_row(b,k,v)),
{
    if contains_row(a+b,k,v) {
        let i = choose|i: int| 0 <= i < (a+b).len() && (a+b)[i].key == k && (a+b)[i].value == v;
        if i < a.len() { assert(a[i] == (a+b)[i]); }
        else { assert(b[i-a.len()] == (a+b)[i]); }
    }
    if contains_row(a,k,v) {
        let i = choose|i: int| 0 <= i < a.len() && a[i].key == k && a[i].value == v;
        assert((a+b)[i] == a[i]);
    }
    if contains_row(b,k,v) {
        let i = choose|i: int| 0 <= i < b.len() && b[i].key == k && b[i].value == v;
        assert((a+b)[a.len()+i] == b[i]);
    }
}
pub proof fn concat_sorted(reverse: bool,a: Seq<WireRow>,b: Seq<WireRow>)
    requires sorted(reverse,a), sorted(reverse,b),
        forall|i: int,j: int| 0 <= i < a.len() && 0 <= j < b.len() ==>
            precedes(reverse,a[i].key,b[j].key),
    ensures sorted(reverse,a+b),
{
    hide(cmp_spec);
    assert forall|i: int,j: int| 0 <= i < j < (a+b).len() implies
        precedes(reverse,(a+b)[i].key,(a+b)[j].key) by {
        if j < a.len() { assert((a+b)[i] == a[i]); assert((a+b)[j] == a[j]); }
        else if i < a.len() { assert((a+b)[i] == a[i]); assert((a+b)[j] == b[j-a.len()]); }
        else { assert((a+b)[i] == b[i-a.len()]); assert((a+b)[j] == b[j-a.len()]); }
    }
}

/// Unbounded induction in page count; no aggregate equality is a premise.
pub proof fn complete_pages(q: &Request,image: SnapshotRows,events: Seq<PageEvent>)
    requires pages(q,image,events),
    ensures exact(q,opt_bytes(q.cursor),image,page_rows(events)),
    decreases events.len(),
{
    hide(cmp_spec);
    let a = events[0].rows;
    if events[0].eof {
        ordered_all(q,a);
        reveal_with_fuel(page_rows,2);
        assert(page_rows(events) =~= a);
    } else {
        let next = &events[1].request;
        complete_pages(next,image,events.skip(1));
        let b = page_rows(events.skip(1));
        complete_page_tail(q,next,image,a,b);
    }
}

pub proof fn complete_page_tail(q: &Request,next: &Request,image: SnapshotRows,
    a: Seq<WireRow>,b: Seq<WireRow>)
    requires page_complete(q,image,a,false), exact(next,opt_bytes(next.cursor),image,b),
        next.lo@ == q.lo@, opt_bytes(next.hi) == opt_bytes(q.hi), next.reverse == q.reverse,
        opt_bytes(next.cursor) == Some(a.last().key),
    ensures exact(q,opt_bytes(q.cursor),image,a+b),
{
    hide(cmp_spec);
    ordered_all(q,a);
    assert(a.len() > 0);
    assert forall|i: int,j: int| 0 <= i < a.len() && 0 <= j < b.len() implies
        precedes(q.reverse,a[i].key,b[j].key) by {
        assert(contains_row(b,b[j].key,b[j].value));
        assert(eligible(next,opt_bytes(next.cursor),b[j].key));
        point_after_page(q,a,b[j].key);
    }
    concat_sorted(q.reverse,a,b);
    assert forall|k: Seq<u8>,v: Seq<u8>| contains_row(a+b,k,v) <==>
        image.contains_key(k) && image[k] == v && eligible(q,opt_bytes(q.cursor),k) by {
        tail_membership(q,next,image,a,b,k,v);
    }
}

pub proof fn point_after_page(q: &Request,a: Seq<WireRow>,k: Seq<u8>)
    requires ordered(q,a), a.len() > 0, eligible(q,Some(a.last().key),k),
    ensures eligible(q,opt_bytes(q.cursor),k),
        forall|i: int| 0 <= i < a.len() ==> precedes(q.reverse,a[i].key,k),
{
    hide(cmp_spec); hide(ordered); hide(eligible);
    ordered_last_eligible(q,a);
    eligible_trans(q,a.last().key,k);
    assert forall|i: int| 0 <= i < a.len() implies precedes(q.reverse,a[i].key,k) by {
        if i < a.len()-1 {
            ordered_pair(q,a,i,a.len() as int-1);
            precedes_trans(q.reverse,a[i].key,a.last().key,k);
        } else {assert(a[i] == a.last());}
    }
}
pub proof fn ordered_last_eligible(q: &Request,a: Seq<WireRow>)
    requires ordered(q,a), a.len() > 0,
    ensures eligible(q,opt_bytes(q.cursor),a.last().key),
{
    hide(cmp_spec); hide(ordered); hide(sorted); hide(eligible);
    ordered_all(q,a);
    assert(eligible(q,opt_bytes(q.cursor),a[a.len() as int-1].key));
}
pub proof fn eligible_trans(q: &Request,previous: Seq<u8>,k: Seq<u8>)
    requires eligible(q,opt_bytes(q.cursor),previous), eligible(q,Some(previous),k),
    ensures eligible(q,opt_bytes(q.cursor),k), precedes(q.reverse,previous,k),
{
    hide(cmp_spec);
    cmp_symmetry(k,previous);
    if opt_bytes(q.cursor) is Some {
        cmp_symmetry(previous,opt_bytes(q.cursor).unwrap());
        cmp_symmetry(k,opt_bytes(q.cursor).unwrap());
        precedes_trans(q.reverse,opt_bytes(q.cursor).unwrap(),previous,k);
    }
}
pub proof fn tail_membership(q: &Request,next: &Request,image: SnapshotRows,
    a: Seq<WireRow>,b: Seq<WireRow>,k: Seq<u8>,v: Seq<u8>)
    requires page_complete(q,image,a,false), exact(next,opt_bytes(next.cursor),image,b),
        next.lo@ == q.lo@, opt_bytes(next.hi) == opt_bytes(q.hi), next.reverse == q.reverse,
        opt_bytes(next.cursor) == Some(a.last().key),
    ensures contains_row(a+b,k,v) == (
        image.contains_key(k) && image[k] == v && eligible(q,opt_bytes(q.cursor),k)),
{
    hide(cmp_spec);
    concat_has(a,b,k,v);
    assert(contains_row(a,k,v) == (image.contains_key(k) && image[k] == v
        && eligible(q,opt_bytes(q.cursor),k) && !eligible(next,opt_bytes(next.cursor),k)));
    assert(contains_row(b,k,v) == (image.contains_key(k) && image[k] == v
        && eligible(next,opt_bytes(next.cursor),k)));
    if eligible(next,opt_bytes(next.cursor),k) {point_after_page(q,a,k);}
}

/// Source connection: these are exactly the successful validate_page outputs.
/// Wire validity supplies ordering and bounded counts, NEVER truthful EOF.
pub open spec fn decoded(q: &Request,bytes: Seq<u8>,page: PageSummary) -> bool {
    page.count <= PAGE_ROWS && (page.count == 0 ==> page.eof)
    && page.count == page.rows@.len() && ordered(q,page.rows@)
    && bytes == crate::routing_codec_proofs::word32(crate::full_scan_core::PAGE_TAG)
        +crate::routing_codec_proofs::word32(page.count as u32)
        +crate::routing_codec_proofs::word32(if page.eof {1u32} else {0u32})
        +crate::full_scan_core::rows_wire(page.rows@)
}
pub proof fn decoded_order(q: &Request,bytes: Seq<u8>,page: PageSummary)
    requires decoded(q,bytes,page),
    ensures sorted(q.reverse,page.rows@),
{ ordered_all(q,page.rows@); }

/// The native Completion::complete postcondition, with errors preserving state.
pub open spec fn completion_effect(before: bool,after: bool,count: usize,eof: bool,
    stopped: bool,status: Status) -> bool {
    if status == Status::Ok { !before && count <= PAGE_ROWS && (count > 0 || eof)
        && after == (eof || stopped) } else { after == before }
}
pub proof fn no_short_page_completion(before: bool,after: bool,count: usize,eof: bool,status: Status)
    requires !before, !eof, completion_effect(before,after,count,eof,false,status),
    ensures !after,
{}
pub proof fn errors_do_not_complete(before: bool,after: bool,count: usize,eof: bool,
    stopped: bool,status: Status)
    requires !before, status != Status::Ok,
        completion_effect(before,after,count,eof,stopped,status),
    ensures !after,
{}
pub proof fn transcript_rejects_error(q: &Request,image: SnapshotRows,events: Seq<PageEvent>,i: int)
    requires pages(q,image,events), 0 <= i < events.len(),
    ensures events[i].status == Status::Ok, !events[i].stopped,
        events[i].eof == (i == events.len()-1),
    decreases events.len(),
{
    if i > 0 { transcript_rejects_error(&events[1].request,image,events.skip(1),i-1); }
}

/// Native forward and reverse iterator-step contracts, accumulated inductively.
/// The cursor is a remaining interval, not a count of rows. Empty segments are
/// consumed only after a successful truthful empty EOF page.
pub open spec fn segments(t: RouteTable,reverse: bool,lo: Seq<u8>,hi: Option<Seq<u8>>,
    ss: Seq<ScanSegment>) -> bool
    decreases ss.len(),
{
    ss.len() > 0 && if reverse {
        reverse_segment_step(t,lo,Some(hi),
            if ss.len() == 1 {None} else {Some(Some(ss[0].lo@))},Some(ss[0]))
        && (ss.len() == 1 || segments(t,reverse,lo,Some(ss[0].lo@),ss.skip(1)))
    } else {
        segment_step(t,Some(lo),hi,
            if ss.len() == 1 {None} else {Some(ss[1].lo@)},Some(ss[0]))
        && (ss.len() == 1 || segments(t,reverse,ss[1].lo@,hi,ss.skip(1)))
    }
}
pub open spec fn rest_lo(reverse: bool,lo: Seq<u8>,ss: Seq<ScanSegment>) -> Seq<u8> {
    if reverse {lo} else {ss[1].lo@}
}
pub open spec fn rest_hi(reverse: bool,hi: Option<Seq<u8>>,ss: Seq<ScanSegment>) -> Option<Seq<u8>> {
    if reverse {Some(ss[0].lo@)} else {hi}
}
pub open spec fn flatten(chunks: Seq<Seq<WireRow>>) -> Seq<WireRow>
    decreases chunks.len(),
{
    if chunks.len() == 0 {Seq::empty()} else {chunks[0]+flatten(chunks.skip(1))}
}
pub open spec fn interval_exact(reverse: bool,lo: Seq<u8>,hi: Option<Seq<u8>>,
    image: SnapshotRows,rows: Seq<WireRow>) -> bool {
    sorted(reverse,rows) && forall|k: Seq<u8>,v: Seq<u8>| contains_row(rows,k,v) <==>
        image.contains_key(k) && image[k] == v && inside(k,lo,hi)
}

pub proof fn segment_partition(t: RouteTable,reverse: bool,lo: Seq<u8>,hi: Option<Seq<u8>>,
    ss: Seq<ScanSegment>,k: Seq<u8>)
    requires segments(t,reverse,lo,hi,ss),
    ensures inside(k,lo,hi) == (inside(k,ss[0].lo@,hi_view(ss[0].hi))
        || ss.len() > 1 && inside(k,rest_lo(reverse,lo,ss),rest_hi(reverse,hi,ss))),
        !(inside(k,ss[0].lo@,hi_view(ss[0].hi)) && ss.len() > 1
            && inside(k,rest_lo(reverse,lo,ss),rest_hi(reverse,hi,ss))),
        inside(k,ss[0].lo@,hi_view(ss[0].hi)) ==> route(t.boundaries@,k) == ss[0].grant,
{
    if reverse {
        reverse_segment_exact_partition(t,lo,Some(hi),
            if ss.len() == 1 {None} else {Some(Some(ss[0].lo@))},ss[0],k);
    } else {
        segment_exact_partition(t,Some(lo),hi,
            if ss.len() == 1 {None} else {Some(ss[1].lo@)},ss[0],k);
    }
}

/// Exact union AND output order are consequences of native segmentation.
pub proof fn merge_segments(t: RouteTable,reverse: bool,lo: Seq<u8>,hi: Option<Seq<u8>>,
    ss: Seq<ScanSegment>,image: SnapshotRows,chunks: Seq<Seq<WireRow>>)
    requires segments(t,reverse,lo,hi,ss), chunks.len() == ss.len(),
        forall|i: int| 0 <= i < ss.len() ==>
            interval_exact(reverse,ss[i].lo@,hi_view(ss[i].hi),image,chunks[i]),
    ensures interval_exact(reverse,lo,hi,image,flatten(chunks)),
    decreases ss.len(),
{
    hide(cmp_spec);
    let a = chunks[0];
    if ss.len() == 1 {
        assert(chunks.skip(1).len() == 0);
        reveal_with_fuel(flatten,2);
        assert(flatten(chunks) =~= a);
        assert forall|k: Seq<u8>,v: Seq<u8>| contains_row(a,k,v) <==>
            image.contains_key(k) && image[k] == v && inside(k,lo,hi) by {
            segment_partition(t,reverse,lo,hi,ss,k);
        }
    } else {
        let l = rest_lo(reverse,lo,ss); let h = rest_hi(reverse,hi,ss);
        assert forall|i: int| 0 <= i < ss.skip(1).len() implies
            interval_exact(reverse,ss.skip(1)[i].lo@,hi_view(ss.skip(1)[i].hi),image,chunks.skip(1)[i]) by {
            assert(ss.skip(1)[i] == ss[i+1]); assert(chunks.skip(1)[i] == chunks[i+1]);
        }
        merge_segments(t,reverse,l,h,ss.skip(1),image,chunks.skip(1));
        let b = flatten(chunks.skip(1));
        assert forall|i: int,j: int| 0 <= i < a.len() && 0 <= j < b.len() implies
            precedes(reverse,a[i].key,b[j].key) by {
            assert(contains_row(a,a[i].key,a[i].value)); assert(contains_row(b,b[j].key,b[j].value));
            if reverse {
                cmp_laws(a[i].key,ss[0].lo@);
                cmp_trans(b[j].key,ss[0].lo@,a[i].key);
            } else {
                cmp_laws(b[j].key,ss[1].lo@);
                cmp_trans(a[i].key,ss[1].lo@,b[j].key);
            }
        }
        concat_sorted(reverse,a,b);
        assert forall|k: Seq<u8>,v: Seq<u8>| contains_row(a+b,k,v) <==>
            image.contains_key(k) && image[k] == v && inside(k,lo,hi) by {
            concat_has(a,b,k,v); segment_partition(t,reverse,lo,hi,ss,k);
        }
    }
}

pub open spec fn segment_request(q: &Request,s: ScanSegment,reverse: bool) -> bool {
    q.lo@ == s.lo@ && opt_bytes(q.hi) == hi_view(s.hi)
    && q.reverse == reverse && q.cursor is None
}

pub proof fn exact_means_no_duplicates(reverse: bool,lo: Seq<u8>,hi: Option<Seq<u8>>,
    image: SnapshotRows,rows: Seq<WireRow>,i: int,j: int)
    requires interval_exact(reverse,lo,hi,image,rows), 0 <= i < j < rows.len(),
    ensures rows[i].key != rows[j].key,
        inside(rows[i].key,lo,hi), image.contains_key(rows[i].key), image[rows[i].key] == rows[i].value,
{
    hide(cmp_spec);
    assert(precedes(reverse,rows[i].key,rows[j].key));
    assert(contains_row(rows,rows[i].key,rows[i].value));
    cmp_laws(rows[i].key,rows[j].key);
}

/// A range's missing rows are not missing routing coordinates. This theorem
/// applies even to an entirely absent range and to the empty byte key.
pub proof fn absence_is_exact(reverse: bool,lo: Seq<u8>,hi: Option<Seq<u8>>,
    image: SnapshotRows,rows: Seq<WireRow>,k: Seq<u8>)
    requires interval_exact(reverse,lo,hi,image,rows), !image.contains_key(k),
    ensures forall|v: Seq<u8>| !contains_row(rows,k,v),
{}

/// Physical row keys and routing coordinates are different for warehouse
/// tables. This projection is precisely storage::Cell = (table,(coord,key)).
/// Fixed-coordinate scans use ONE point admission; they must NOT segment raw
/// key bytes against warehouse-coordinate boundaries.
pub open spec fn coordinate(fixed: Option<Seq<u8>>,key: Seq<u8>) -> Seq<u8> {
    match fixed {Some(c) => c,None => key}
}
pub open spec fn project(image: Image,table: u64,fixed: Option<Seq<u8>>) -> SnapshotRows {
    vstd::imap::IMap::new(|k: Seq<u8>| image.contains_key((table,(coordinate(fixed,k),k))),
        |k: Seq<u8>| image[(table,(coordinate(fixed,k),k))])
}
pub proof fn projected_bytes(image: Image,table: u64,fixed: Option<Seq<u8>>,key: Seq<u8>)
    ensures project(image,table,fixed).contains_key(key) ==
            image.contains_key((table,(coordinate(fixed,key),key))),
        project(image,table,fixed).contains_key(key) ==> project(image,table,fixed)[key] ==
            image[(table,(coordinate(fixed,key),key))],
{}

/// Stability is a consequence of overlapping registrations having one full
/// grant, not an assumption that a cached observation stays current.
pub proof fn held_range_stable(session: SessionView,table: u64,lo: Seq<u8>,hi: Option<Seq<u8>>,
    old: Grant,new: Grant,k: Seq<u8>)
    requires session.wf(), session.has_range(table,lo,hi,old), session.has(table,k,new), inside(k,lo,hi),
    ensures old == new,
{
    let i = choose|i: int| 0 <= i < session.holds.len()
        && session.holds[i].covers_range(table,lo,hi) && session.holds[i].grant == old;
    crate::leases::covered_point(session.holds[i],crate::leases::range_scope(table,lo,hi,old),table,k);
    assert(session.has(table,k,old));
    crate::leases_proofs::unique_grant(session,table,k,old);
    crate::leases_proofs::unique_grant(session,table,k,new);
}

/// Exact successful admission clauses of Participant::acquire_range. A stale
/// unheld incarnation cannot obtain a certificate, even when it returns to
/// the same owner. Failure cannot be interpreted as an empty segment.
pub open spec fn admission(before: crate::participant::Participant,
    after: crate::participant::Participant,id: TxnId,table: u64,lo: Seq<u8>,hi: Option<Seq<u8>>,
    grant: Grant,status: Status) -> bool {
    (status == Status::Ok ==> after.open_range(id,table,lo,hi,grant)
        && grant.owner == before.owner_view())
    && (status == Status::Ok && !before.open_range(id,table,lo,hi,grant) ==>
        forall|k: Seq<u8>| inside(k,lo,hi) ==> before.local_meta(table,k) is Some
            && before.local_meta(table,k).unwrap().role is Serving
            && before.local_meta(table,k).unwrap().epoch == grant.epoch)
    && (status != Status::Ok ==> after.lease_view() == before.lease_view())
}
pub proof fn stale_unheld_rejected(before: crate::participant::Participant,
    after: crate::participant::Participant,id: TxnId,table: u64,lo: Seq<u8>,hi: Option<Seq<u8>>,
    grant: Grant,status: Status,k: Seq<u8>)
    requires admission(before,after,id,table,lo,hi,grant,status),
        !before.open_range(id,table,lo,hi,grant), inside(k,lo,hi),
        grant.owner != before.owner_view() || before.local_meta(table,k) is None
            || !(before.local_meta(table,k).unwrap().role is Serving)
            || before.local_meta(table,k).unwrap().epoch != grant.epoch,
    ensures status != Status::Ok, after.lease_view() == before.lease_view(),
{}

/// Concrete nonvacuous page witness: an empty snapshot has one truthful empty
/// EOF page for ANY byte bounds, direction and cursor. No finite sentinel.
pub proof fn empty_engine_page(q: &Request)
    ensures page_complete(q,vstd::imap::IMap::empty(),Seq::empty(),true),
{}

/// Maximum::wf alone is not exhaustive. A successful discovery pass must
/// additionally cover the snapshot. With that named range-scan premise, the
/// actual observed maximum bounds all keys even for an unbounded upper range.
pub proof fn unbounded_reverse_maximum(q: &Request,maximum: crate::full_scan_core::Maximum,
    image: SnapshotRows)
    requires maximum.wf(), q.reverse, q.hi is None,
        forall|k: Seq<u8>| image.contains_key(k) && q.contains_spec(k) <==>
            maximum.observed@.contains(k),
    ensures maximum.key is None ==> forall|k: Seq<u8>| !image.contains_key(k) || !q.contains_spec(k),
        maximum.key is Some ==> forall|k: Seq<u8>| image.contains_key(k) && q.contains_spec(k) ==>
            cmp_spec(k,maximum.key.unwrap()@) <= 0,
{
    assert forall|k: Seq<u8>| image.contains_key(k) && q.contains_spec(k) implies
        maximum.key is Some && cmp_spec(k,maximum.key.unwrap()@) <= 0 by {
        let i = choose|i: int| 0 <= i < maximum.observed@.len() && maximum.observed@[i] == k;
        assert(cmp_spec(maximum.observed@[i],maximum.key.unwrap()@) <= 0);
    }
}

/// Every consumed segment is routed to the exact owner AND incarnation;
/// keys with no physical row satisfy precisely the same routing theorem.
pub proof fn routed_segment(t: RouteTable,reverse: bool,lo: Seq<u8>,hi: Option<Seq<u8>>,
    ss: Seq<ScanSegment>,i: int,k: Seq<u8>)
    requires segments(t,reverse,lo,hi,ss), 0 <= i < ss.len(),
        inside(k,ss[i].lo@,hi_view(ss[i].hi)),
    ensures inside(k,lo,hi), route(t.boundaries@,k) == ss[i].grant,
    decreases ss.len(),
{
    segment_partition(t,reverse,lo,hi,ss,k);
    if i > 0 {
        assert(ss.skip(1)[i-1] == ss[i]);
        routed_segment(t,reverse,rest_lo(reverse,lo,ss),rest_hi(reverse,hi,ss),ss.skip(1),i-1,k);
    }
}

/// Successful registration can append a hold but cannot mutate any old hold.
/// Metadata-only ownership transitions keep lease_view unchanged; neither
/// form can silently replace a retained incarnation.
pub proof fn registration_preserves_range(before: SessionView,after: SessionView,
    scope: crate::leases::ScopeView,status: Status,table: u64,lo: Seq<u8>,
    hi: Option<Seq<u8>>,grant: Grant)
    requires crate::leases::registration(before,after,scope,status),
        before.has_range(table,lo,hi,grant),
    ensures after.has_range(table,lo,hi,grant),
        before.sequence == after.sequence, before.terminal == after.terminal,
{
    let i = choose|i: int| 0 <= i < before.holds.len()
        && before.holds[i].covers_range(table,lo,hi) && before.holds[i].grant == grant;
    assert(after.holds[i] == before.holds[i]);
}

pub proof fn metadata_change_preserves_range(before: crate::participant::Participant,
    after: crate::participant::Participant,id: TxnId,table: u64,lo: Seq<u8>,
    hi: Option<Seq<u8>>,grant: Grant)
    requires before.lease_view() == after.lease_view(), before.open_range(id,table,lo,hi,grant),
    ensures after.open_range(id,table,lo,hi,grant),
{
    after.lease_frame_open_range(before,id,table,lo,hi,grant);
}

/// The valid zero-row/EOF shape is insufficient to prove exhaustion. In
/// particular, a present empty-byte key cannot be silently omitted.
pub proof fn format_eof_is_not_truthful(q: &Request,image: SnapshotRows,k: Seq<u8>)
    requires image.contains_key(k), eligible(q,opt_bytes(q.cursor),k),
    ensures ordered(q,Seq::empty()), !page_complete(q,image,Seq::empty(),true),
{
    assert(!contains_row(Seq::empty(),k,image[k]));
}

/// A nonempty witness for arbitrary binary key/value bytes, including the
/// empty key when permitted by the request. This is not a bounded proof of
/// composition; complete_pages and merge_segments carry the inductions.
pub proof fn singleton_engine_page(q: &Request,k: Seq<u8>,v: Seq<u8>)
    requires eligible(q,opt_bytes(q.cursor),k),
    ensures page_complete(q,vstd::imap::IMap::empty().insert(k,v),
        seq![WireRow {key:k,value:v}],true),
{
    let rows = seq![WireRow {key:k,value:v}];
    assert forall|key: Seq<u8>,value: Seq<u8>| contains_row(rows,key,value) <==>
        vstd::imap::IMap::empty().insert(k,v).contains_key(key)
        && vstd::imap::IMap::empty().insert(k,v)[key] == value
        && eligible(q,opt_bytes(q.cursor),key) by {
        if contains_row(rows,key,value) {
            let i = choose|i: int| 0 <= i < rows.len() && rows[i].key == key && rows[i].value == value;
            assert(i == 0);
        }
        if key == k && value == v { assert(rows[0].key == key && rows[0].value == value); }
    }
}

/// Injectivity of the ONE production request codec, including every native
/// identity field. A payload cannot be relabeled to another transaction,
/// owner/incarnation, logical table, fixed address, bounds, or cursor.
pub proof fn request_roundtrip(a: crate::full_scan_core::BoundRequest,b: crate::full_scan_core::BoundRequest)
    requires crate::full_scan_core::request_wire(a) == crate::full_scan_core::request_wire(b),
        crate::full_scan_core::request_wire(a).len() <= crate::full_scan_core::REQUEST_CAPACITY,
    ensures crate::full_scan_core::same_identity(a.identity,b.identity),
        crate::full_scan_core::same_bounds(a.bounds,b.bounds),
{
    hide(cmp_spec); hide(word32); hide(word64); hide(blob);
    crate::full_scan_core::request_wire_sizes(a);
    crate::full_scan_core::request_wire_sizes(b);
    assert(request_wire(a).subrange(4,8) =~= word32(request_flags(a)));
    assert(request_wire(b).subrange(4,8) =~= word32(request_flags(b)));
    word32_injective(request_flags(a),request_flags(b));
    assert(request_wire(a).subrange(8,16) =~= word64(a.identity.transaction.client));
    assert(request_wire(b).subrange(8,16) =~= word64(b.identity.transaction.client));
    word64_injective(a.identity.transaction.client,b.identity.transaction.client);
    assert(request_wire(a).subrange(16,24) =~= word64(a.identity.transaction.sequence));
    assert(request_wire(b).subrange(16,24) =~= word64(b.identity.transaction.sequence));
    word64_injective(a.identity.transaction.sequence,b.identity.transaction.sequence);
    assert(request_wire(a).subrange(24,28) =~= word32(a.identity.grant.owner));
    assert(request_wire(b).subrange(24,28) =~= word32(b.identity.grant.owner));
    word32_injective(a.identity.grant.owner,b.identity.grant.owner);
    assert(request_wire(a).subrange(28,36) =~= word64(a.identity.grant.epoch));
    assert(request_wire(b).subrange(28,36) =~= word64(b.identity.grant.epoch));
    word64_injective(a.identity.grant.epoch,b.identity.grant.epoch);
    assert(request_wire(a).subrange(36,44) =~= word64(a.identity.table));
    assert(request_wire(b).subrange(36,44) =~= word64(b.identity.table));
    word64_injective(a.identity.table,b.identity.table);
    let ax = blob(a.bounds.lo@)+blob(option_payload(a.bounds.hi))+blob(option_payload(a.bounds.cursor));
    let bx = blob(b.bounds.lo@)+blob(option_payload(b.bounds.hi))+blob(option_payload(b.bounds.cursor));
    assert(request_wire(a).skip(44) =~= blob(a.identity.coordinate@)+ax);
    assert(request_wire(b).skip(44) =~= blob(b.identity.coordinate@)+bx);
    blob_prefix_unique(a.identity.coordinate@,b.identity.coordinate@,ax,bx);
    assert(ax =~= blob(a.bounds.lo@)+(blob(option_payload(a.bounds.hi))+blob(option_payload(a.bounds.cursor))));
    assert(bx =~= blob(b.bounds.lo@)+(blob(option_payload(b.bounds.hi))+blob(option_payload(b.bounds.cursor))));
    blob_prefix_unique(a.bounds.lo@,b.bounds.lo@,
        blob(option_payload(a.bounds.hi))+blob(option_payload(a.bounds.cursor)),
        blob(option_payload(b.bounds.hi))+blob(option_payload(b.bounds.cursor)));
    blob_prefix_unique(option_payload(a.bounds.hi),option_payload(b.bounds.hi),
        blob(option_payload(a.bounds.cursor)),blob(option_payload(b.bounds.cursor)));
    assert(blob(option_payload(a.bounds.cursor))+Seq::<u8>::empty() =~= blob(option_payload(a.bounds.cursor)));
    assert(blob(option_payload(b.bounds.cursor))+Seq::<u8>::empty() =~= blob(option_payload(b.bounds.cursor)));
    blob_prefix_unique(option_payload(a.bounds.cursor),option_payload(b.bounds.cursor),Seq::empty(),Seq::empty());
}

pub proof fn rows_wire_unique(a: Seq<WireRow>,b: Seq<WireRow>)
    requires a.len() == b.len(), crate::full_scan_core::rows_wire(a) == crate::full_scan_core::rows_wire(b),
        crate::full_scan_core::rows_wire(a).len() <= crate::full_scan_core::PAGE_CAPACITY,
    ensures a == b,
    decreases a.len(),
{
    hide(blob); hide(word32); hide(word64);
    if a.len() > 0 {
        rows_head(a); rows_head(b); row_fits_wire(a,0); row_fits_wire(b,0);
        assert(blob(a[0].key)+(blob(a[0].value)+rows_wire(a.skip(1))) =~= rows_wire(a));
        assert(blob(b[0].key)+(blob(b[0].value)+rows_wire(b.skip(1))) =~= rows_wire(b));
        blob_prefix_unique(a[0].key,b[0].key,blob(a[0].value)+rows_wire(a.skip(1)),blob(b[0].value)+rows_wire(b.skip(1)));
        blob_prefix_unique(a[0].value,b[0].value,rows_wire(a.skip(1)),rows_wire(b.skip(1)));
        rows_wire_unique(a.skip(1),b.skip(1));
        assert(a[0] == b[0]);
        assert forall|i: int| 0 <= i < a.len() implies a[i] == b[i] by {
            if i > 0 {
                assert(a.skip(1)[i-1] == a[i]);
                assert(b.skip(1)[i-1] == b[i]);
            }
        }
        assert(a =~= b);
    } else {assert(a =~= b);}
}
pub proof fn page_wire_unique(a: Seq<WireRow>,aeof: bool,b: Seq<WireRow>,beof: bool)
    requires crate::full_scan_core::page_wire(a,aeof) == crate::full_scan_core::page_wire(b,beof),
        a.len() <= PAGE_ROWS, b.len() <= PAGE_ROWS,
        crate::full_scan_core::page_wire(a,aeof).len() <= crate::full_scan_core::PAGE_CAPACITY,
    ensures a == b, aeof == beof,
{
    assert(page_wire(a,aeof).subrange(4,8) =~= word32(a.len() as u32));
    assert(page_wire(b,beof).subrange(4,8) =~= word32(b.len() as u32));
    word32_injective(a.len() as u32,b.len() as u32);
    assert(page_wire(a,aeof).subrange(8,12) =~= word32(if aeof {1u32} else {0u32}));
    assert(page_wire(b,beof).subrange(8,12) =~= word32(if beof {1u32} else {0u32}));
    word32_injective(if aeof {1u32} else {0u32},if beof {1u32} else {0u32});
    assert(page_wire(a,aeof).skip(12) =~= rows_wire(a));
    assert(page_wire(b,beof).skip(12) =~= rows_wire(b));
    rows_wire_unique(a,b);
}

/// Exact postcondition of checked FullScan::finish_page. The callback adapter
/// receives precisely PageCursor::next's borrowed pairs, once per `taken` row.
pub open spec fn consume_effect(before: crate::full_scan_core::FullScan,after: crate::full_scan_core::FullScan,
    expected: Seq<WireRow>,taken: Seq<WireRow>,eof: bool,stopped: bool,out: Result<bool,Status>) -> bool {
    after.request.identity == before.request.identity
    && after.request.bounds.lo == before.request.bounds.lo && after.request.bounds.hi == before.request.bounds.hi
    && after.request.bounds.reverse == before.request.bounds.reverse
    && match out {
        Ok(done) => !before.completion.done && done == after.completion.done && done == (eof || stopped)
            && (stopped || taken == expected) && after.delivered@ == before.delivered@+taken
            && opt_bytes(after.request.bounds.cursor) == if stopped || expected.len() == 0 {
                opt_bytes(before.request.bounds.cursor)
            } else {Some(expected.last().key)},
        Err(_) => after == before,
    }
}

/// Successful transaction interface key. In raw mode the coordinate is the
/// PHYSICAL ROW key; in warehouse mode all row keys share one fixed address.
/// The source metadata and complete grant remain separate admission evidence.
pub type ScanTarget = (TxnId,u32,u64,Option<Seq<u8>>);
pub type EngineViews = Map<ScanTarget,SnapshotRows>;
pub open spec fn target(identity: crate::full_scan_core::ScanIdentity) -> ScanTarget {
    (identity.transaction,identity.grant.owner,identity.table,
        if identity.fixed_coordinate {Some(identity.coordinate@)} else {None})
}
pub open spec fn snapshot_value(rows: SnapshotRows,k: Seq<u8>) -> Option<Seq<u8>> {
    if rows.contains_key(k) {Some(rows[k])} else {None}
}
/// Trusted engine range enumeration, BEFORE the sharding page builder. `raw`
/// is the complete common-snapshot engine iterator; actual callbacks form its
/// prefix and stop only on the callback's return or an engine error. The
/// contract does not mention Page.rows, PageBudget.more or a sharding EOF.
/// Canonical-address -> physical handle translation and synchronous raw
/// callback forwarding remain foreign ABI obligations, not an invented map.
pub open spec fn trusted_transaction_scan(views: EngineViews,
    request: crate::full_scan_core::BoundRequest,raw: Seq<WireRow>,callbacks: Seq<AddEvent>,exit: EngineExit) -> bool {
    if exit is Error {true} else {
    views.contains_key(target(request.identity))
    && exact(&request.bounds,opt_bytes(request.bounds.cursor),views[target(request.identity)],raw)
    && callbacks.len() <= raw.len()
    && (forall|i: int| 0 <= i < callbacks.len() ==> callbacks[i].row == raw[i])
    && match exit {
        EngineExit::Exhausted => callbacks.len() == raw.len()
            && forall|i: int| 0 <= i < callbacks.len() ==> callbacks[i].outcome == Ok(true),
        EngineExit::CallbackStop => callbacks.len() > 0 && callbacks.last().outcome == Ok(false)
            && forall|i: int| 0 <= i < callbacks.len()-1 ==> callbacks[i].outcome == Ok(true),
        EngineExit::Error => true,
    }
    }
}
pub open spec fn admitted(request: crate::full_scan_core::BoundRequest,leases: Map<u64,SessionView>) -> bool {
    let identity = request.identity;
    leases.contains_key(identity.transaction.client)
    && leases[identity.transaction.client].wf()
    && leases[identity.transaction.client].sequence == identity.transaction.sequence
    && !leases[identity.transaction.client].terminal
    && if identity.fixed_coordinate {
        leases[identity.transaction.client].has(identity.table,identity.coordinate@,identity.grant)
    } else {
        leases[identity.transaction.client].has_range(identity.table,request.bounds.lo@,
            opt_bytes(request.bounds.hi),identity.grant)
    }
}

/// Source-connected exchange evidence. Equality of request/reply bytes and
/// req_nr/status provenance is the named authentic RPC boundary. It does NOT
/// supply row correctness: that comes only from the exact engine call, the
/// checked serializer, and checked validator/consumer below.
pub struct Exchange {
    pub before: crate::full_scan_core::FullScan,
    pub after: crate::full_scan_core::FullScan,
    pub decoded: crate::full_scan_core::BoundRequest,
    pub outer: crate::full_scan_core::ScanIdentity,
    pub destination: u32,
    pub participant: crate::participant::Participant,
    pub leases: Map<u64,SessionView>,
    pub request_bytes: Seq<u8>,
    pub request_number: u32,
    pub reply_number: u32,
    pub reply_status: Status,
    pub producer: crate::full_scan_core::Page,
    pub initial_builder: crate::full_scan_core::Page,
    pub raw_engine_rows: Seq<WireRow>,
    pub callbacks: Seq<AddEvent>,
    pub engine_exit: EngineExit,
    pub reply_bytes: Seq<u8>,
    pub validated: Seq<WireRow>,
    pub consumed: Seq<WireRow>,
    pub eof: bool,
    pub stopped: bool,
    pub outcome: Result<bool,Status>,
}
pub open spec fn exchange(e: Exchange,views: EngineViews) -> bool {
    e.before.request.bounds.wf() && e.before.request.identity.wf()
    && !e.before.completion.done
    && e.request_bytes == crate::full_scan_core::request_wire(e.before.request)
    && e.request_bytes == crate::full_scan_core::request_wire(e.decoded)
    && e.request_bytes.len() <= crate::full_scan_core::REQUEST_CAPACITY
    && crate::full_scan_core::same_identity(e.decoded.identity,e.outer)
    && e.destination == e.outer.grant.owner
    && e.participant.wf() && e.participant.owner_view() == e.destination
    && e.leases == e.participant.lease_view()
    && (if e.decoded.identity.fixed_coordinate {
        e.participant.open_hold(e.decoded.identity.transaction,e.decoded.identity.table,
            e.decoded.identity.coordinate@,e.decoded.identity.grant)
    } else {
        e.participant.open_range(e.decoded.identity.transaction,e.decoded.identity.table,
            e.decoded.bounds.lo@,opt_bytes(e.decoded.bounds.hi),e.decoded.identity.grant)
    })
    && e.reply_number == e.request_number && e.reply_status == Status::Ok
    && e.producer.wf() && e.producer@.request == e.decoded && e.producer@.error is None
    && e.initial_builder.wf() && e.initial_builder@.request == e.decoded
    && e.initial_builder@.rows@ == Seq::<WireRow>::empty()
    && !e.initial_builder@.budget.more && e.initial_builder@.error is None
    && assembly(e.initial_builder,e.callbacks) && e.producer == assembled(e.initial_builder,e.callbacks)
    && !(e.engine_exit is Error)
    && trusted_transaction_scan(views,e.decoded,e.raw_engine_rows,e.callbacks,e.engine_exit)
    && e.reply_bytes == crate::full_scan_core::page_wire(e.producer@.rows@,!e.producer@.budget.more)
    && e.reply_bytes == crate::full_scan_core::page_wire(e.validated,e.eof)
    && e.reply_bytes.len() <= crate::full_scan_core::PAGE_CAPACITY && e.validated.len() <= PAGE_ROWS
    && ordered(&e.before.request.bounds,e.validated)
    && consume_effect(e.before,e.after,e.validated,e.consumed,e.eof,e.stopped,e.outcome)
}
pub proof fn exchange_exact(e: Exchange,views: EngineViews)
    requires exchange(e,views), !e.stopped, e.outcome is Ok,
    ensures crate::full_scan_core::same_identity(e.before.request.identity,e.decoded.identity),
        crate::full_scan_core::same_bounds(e.before.request.bounds,e.decoded.bounds),
        e.destination == e.before.request.identity.grant.owner,
        e.consumed == e.producer@.rows@, e.eof == !e.producer@.budget.more,
        page_complete(&e.before.request.bounds,views[target(e.before.request.identity)],e.consumed,e.eof),
        e.after.request.identity == e.before.request.identity,
        admitted(e.before.request,e.leases),
        e.before.request.bounds.wf() && e.before.request.identity.wf(),
        e.reply_status == Status::Ok, e.consumed == e.validated,
        consume_effect(e.before,e.after,e.validated,e.consumed,e.eof,e.stopped,e.outcome),
        views.contains_key(target(e.before.request.identity)),
{
    request_roundtrip(e.before.request,e.decoded);
    e.producer.facts();
    page_wire_unique(e.producer@.rows@,!e.producer@.budget.more,e.validated,e.eof);
    participant_admission_evidence(e.participant,e.decoded);
    assembled_engine_page(views,e.decoded,e.raw_engine_rows,e.callbacks,e.engine_exit,e.initial_builder);
}
pub open spec fn exchanges(request: crate::full_scan_core::BoundRequest,views: EngineViews,xs: Seq<Exchange>) -> bool
    decreases xs.len(),
{
    xs.len() > 0 && xs[0].before.request == request && exchange(xs[0],views)
    && !xs[0].stopped && xs[0].outcome is Ok
    && if xs[0].after.completion.done {xs.len() == 1} else {
        xs.len() > 1 && xs[1].before == xs[0].after
        && exchanges(xs[0].after.request,views,xs.skip(1))
    }
}
pub open spec fn transcript(xs: Seq<Exchange>) -> Seq<PageEvent> {
    Seq::new(xs.len(),|i: int| PageEvent {request:xs[i].before.request.bounds,
        rows:xs[i].consumed,eof:xs[i].eof,status:xs[i].reply_status,stopped:xs[i].stopped})
}
pub proof fn transcript_complete(request: crate::full_scan_core::BoundRequest,views: EngineViews,xs: Seq<Exchange>)
    requires exchanges(request,views,xs),
    ensures pages(&request.bounds,views[target(request.identity)],transcript(xs)),
        admitted(request,xs[0].leases),
        views.contains_key(target(request.identity)),
        xs.last().after.delivered@ == xs[0].before.delivered@+page_rows(transcript(xs)),
        forall|i: int| 0 <= i < xs.len() ==>
            crate::full_scan_core::same_identity(xs[i].decoded.identity,request.identity)
            && xs[i].destination == request.identity.grant.owner,
    decreases xs.len(),
{
    hide(cmp_spec); hide(exchange); hide(trusted_transaction_scan);
    hide(crate::full_scan_core::request_wire); hide(rows_wire); hide(blob);
    hide(word32); hide(word64);
    exchange_exact(xs[0],views);
    if xs.len() > 1 {
        transcript_complete(xs[0].after.request,views,xs.skip(1));
        assert(transcript(xs).skip(1) =~= transcript(xs.skip(1)));
        assert(xs.skip(1).last() == xs.last());
        assert(xs.skip(1)[0] == xs[1]);
        let suffix = page_rows(transcript(xs.skip(1)));
        assert((xs[0].before.delivered@+xs[0].consumed)+suffix
            =~= xs[0].before.delivered@+(xs[0].consumed+suffix));
        assert(page_rows(transcript(xs)) == xs[0].consumed+suffix);
        assert forall|i: int| 0 <= i < xs.len() implies
            crate::full_scan_core::same_identity(xs[i].decoded.identity,request.identity)
            && xs[i].destination == request.identity.grant.owner by {
            if i > 0 {assert(xs.skip(1)[i-1] == xs[i]);}
        }
    } else {
        assert(xs.last() == xs[0]);
        reveal_with_fuel(page_rows,2);
        assert(page_rows(transcript(xs)) =~= xs[0].consumed);
    }
}

/// Representation of ONE successful transaction's common read view. These
/// premises map independently defined logical/physical model cells to bytes;
/// they DO NOT assume physical replicas equal logical state. That equality is
/// derived from the already proved placement invariant and retained admission.
pub open spec fn read_view_embedding(c: crate::sharding_placement::Constants,s: crate::sharding_placement::State,
    labels: Map<int,crate::storage::Cell>,logical: Image,views: EngineViews,id: TxnId) -> bool {
    labels.dom() == c.keys
    && (forall|k: int| labels.contains_key(k) ==>
        crate::sharding_bytes::value_option(crate::storage::value(logical,labels[k])) == s.logical[k].value)
    && (forall|cell: crate::storage::Cell| logical.contains_key(cell) ==>
        exists|k: int| labels.contains_key(k) && labels[k] == cell)
    && forall|address: ScanTarget| views.contains_key(address) && address.0 == id ==> {
        &&& (forall|k: int| labels.contains_key(k) && labels[k].0 == address.2
            && labels[k].1.0 == coordinate(address.3,labels[k].1.1) ==>
            crate::sharding_bytes::value_option(snapshot_value(views[address],labels[k].1.1))
                == crate::sharding_placement::replica(s,address.1 as int,k).cell.value)
        &&& (forall|key: Seq<u8>| views[address].contains_key(key) ==>
            exists|k: int| labels.contains_key(k)
                && labels[k] == (address.2,(coordinate(address.3,key),key)))
    }
}
/// Native/model hold correspondence is an identity representation relation,
/// as in leases_proofs::expanded, not a value or query-result assumption.
pub open spec fn hold_embedding(s: crate::sharding_placement::State,txn: int,
    labels: Map<int,crate::storage::Cell>,request: crate::full_scan_core::BoundRequest) -> bool {
    s.sessions.contains_key(txn) && !s.sessions[txn].resolved
    && forall|k: int| labels.contains_key(k) && labels[k].0 == request.identity.table
        && labels[k].1.0 == coordinate(target(request.identity).3,labels[k].1.1)
        && inside(labels[k].1.1,request.bounds.lo@,opt_bytes(request.bounds.hi)) ==>
        s.sessions[txn].held.contains_key(k)
        && s.sessions[txn].held[k] == (crate::sharding_placement::Grant {
            owner:request.identity.grant.owner as int,epoch:request.identity.grant.epoch as nat})
}
pub proof fn admitted_snapshot_matches_logical(c: crate::sharding_placement::Constants,s: crate::sharding_placement::State,
    txn: int,labels: Map<int,crate::storage::Cell>,logical: Image,views: EngineViews,
    request: crate::full_scan_core::BoundRequest,key: Seq<u8>)
    requires crate::sharding_placement::proofs::inv(c,s),
        read_view_embedding(c,s,labels,logical,views,request.identity.transaction),
        hold_embedding(s,txn,labels,request), views.contains_key(target(request.identity)),
        inside(key,request.bounds.lo@,opt_bytes(request.bounds.hi)),
    ensures snapshot_value(views[target(request.identity)],key)
        == crate::storage::value(logical,(request.identity.table,(coordinate(target(request.identity).3,key),key))),
{
    let cell = (request.identity.table,(coordinate(target(request.identity).3,key),key));
    if exists|k: int| labels.contains_key(k) && labels[k] == cell {
        let k = choose|k: int| labels.contains_key(k) && labels[k] == cell;
        assert(s.sessions[txn].held.contains_key(k));
        assert(crate::sharding_placement::read(s,txn,k) is Some);
        crate::sharding_placement::proofs::lemma_read_matches_logical(c,s,txn,k);
        crate::sharding_bytes::value_option_injective(snapshot_value(views[target(request.identity)],key),
            crate::storage::value(logical,cell));
    } else {
        assert(!logical.contains_key(cell));
        assert(!views[target(request.identity)].contains_key(key));
    }
}

pub proof fn raw_exchange_scan(c: crate::sharding_placement::Constants,
    s: crate::sharding_placement::State,txn: int,labels: Map<int,crate::storage::Cell>,
    logical: Image,views: EngineViews,xs: Seq<Exchange>)
    requires xs.len() > 0, crate::sharding_placement::proofs::inv(c,s),
        read_view_embedding(c,s,labels,logical,views,xs[0].before.request.identity.transaction),
        exchanges(xs[0].before.request,views,xs),
        xs[0].before.delivered@ == Seq::<WireRow>::empty(),
        !xs[0].before.request.identity.fixed_coordinate, xs[0].before.request.bounds.cursor is None,
        lease_embedding(s,txn,labels,xs[0].before.request.identity.transaction,xs[0].leases),
    ensures interval_exact(xs[0].before.request.bounds.reverse,xs[0].before.request.bounds.lo@,
        opt_bytes(xs[0].before.request.bounds.hi),project(logical,xs[0].before.request.identity.table,None),
        xs.last().after.delivered@),
        xs.last().after.delivered@ == page_rows(transcript(xs)),
{
    hide(cmp_spec); hide(exchange); hide(exchanges); hide(trusted_transaction_scan);
    hide(read_view_embedding); hide(lease_embedding); hide(crate::sharding_placement::proofs::inv);
    hide(request_wire); hide(rows_wire); hide(blob); hide(word32); hide(word64);
    let request = xs[0].before.request;
    let rows = page_rows(transcript(xs));
    transcript_complete(request,views,xs);
    admission_to_model_hold(s,txn,labels,request,xs[0].leases);
    complete_pages(&request.bounds,views[target(request.identity)],transcript(xs));
    assert(xs.last().after.delivered@ =~= rows);
    assert forall|k: Seq<u8>,v: Seq<u8>| contains_row(rows,k,v) <==>
        project(logical,request.identity.table,None).contains_key(k)
        && project(logical,request.identity.table,None)[k] == v
        && inside(k,request.bounds.lo@,opt_bytes(request.bounds.hi)) by {
        cmp_symmetry(request.bounds.lo@,k);
        if inside(k,request.bounds.lo@,opt_bytes(request.bounds.hi)) {
            admitted_snapshot_matches_logical(c,s,txn,labels,logical,views,request,k);
        }
    }
}

/// Main raw-key theorem: native identity/codec, exact admitted engine call,
/// page bytes, actual consumed prefix/cursor/completion, native segmentation,
/// and independent placement coherence all participate in the conclusion.
pub proof fn sharded_scan(t: RouteTable,reverse: bool,lo: Seq<u8>,hi: Option<Seq<u8>>,
    ss: Seq<ScanSegment>,id: TxnId,traces: Seq<Seq<Exchange>>,
    c: crate::sharding_placement::Constants,s: crate::sharding_placement::State,txn: int,
    labels: Map<int,crate::storage::Cell>,logical: Image,views: EngineViews)
    requires segments(t,reverse,lo,hi,ss), traces.len() == ss.len(),
        crate::sharding_placement::proofs::inv(c,s), read_view_embedding(c,s,labels,logical,views,id),
        forall|i: int| 0 <= i < ss.len() ==> traces[i].len() > 0
            && exchanges(traces[i][0].before.request,views,traces[i])
            && traces[i][0].before.delivered@ == Seq::<WireRow>::empty()
            && traces[i][0].before.request.identity.transaction == id
            && traces[i][0].before.request.identity.table == t.table
            && traces[i][0].before.request.identity.grant == ss[i].grant
            && !traces[i][0].before.request.identity.fixed_coordinate
            && segment_request(&traces[i][0].before.request.bounds,ss[i],reverse)
            && lease_embedding(s,txn,labels,id,traces[i][0].leases),
    ensures interval_exact(reverse,lo,hi,project(logical,t.table,None),
        flatten(Seq::new(traces.len(),|i: int| traces[i].last().after.delivered@))),
        forall|i: int| 0 <= i < traces.len() ==> traces[i].last().after.delivered@
            == page_rows(transcript(traces[i])),
{
    hide(cmp_spec); hide(exchange); hide(exchanges); hide(trusted_transaction_scan);
    hide(read_view_embedding); hide(lease_embedding); hide(crate::sharding_placement::proofs::inv);
    hide(crate::full_scan_core::request_wire); hide(rows_wire); hide(blob); hide(word32); hide(word64);
    let chunks = Seq::new(traces.len(),|i: int| traces[i].last().after.delivered@);
    assert forall|i: int| 0 <= i < ss.len() implies
        interval_exact(reverse,ss[i].lo@,hi_view(ss[i].hi),project(logical,t.table,None),chunks[i]) by {
        raw_exchange_scan(c,s,txn,labels,logical,views,traces[i]);
    }
    merge_segments(t,reverse,lo,hi,ss,project(logical,t.table,None),chunks);
    assert forall|i: int| 0 <= i < traces.len() implies traces[i].last().after.delivered@
        == page_rows(transcript(traces[i])) by {
        raw_exchange_scan(c,s,txn,labels,logical,views,traces[i]);
    }
}

pub open spec fn lease_embedding(s: crate::sharding_placement::State,txn: int,
    labels: Map<int,crate::storage::Cell>,id: TxnId,leases: Map<u64,SessionView>) -> bool {
    s.sessions.contains_key(txn) && !s.sessions[txn].resolved
    && leases.contains_key(id.client) && leases[id.client].sequence == id.sequence
    && forall|k: int,g: Grant| labels.contains_key(k) && leases[id.client].has(labels[k].0,labels[k].1.0,g) ==>
        s.sessions[txn].held.contains_key(k) && s.sessions[txn].held[k] ==
            (crate::sharding_placement::Grant {owner:g.owner as int,epoch:g.epoch as nat})
}
pub proof fn admission_to_model_hold(s: crate::sharding_placement::State,txn: int,
    labels: Map<int,crate::storage::Cell>,request: crate::full_scan_core::BoundRequest,leases: Map<u64,SessionView>)
    requires admitted(request,leases), lease_embedding(s,txn,labels,request.identity.transaction,leases),
    ensures hold_embedding(s,txn,labels,request),
{
    let session = leases[request.identity.transaction.client];
    assert forall|k: int| labels.contains_key(k) && labels[k].0 == request.identity.table
        && labels[k].1.0 == coordinate(target(request.identity).3,labels[k].1.1)
        && inside(labels[k].1.1,request.bounds.lo@,opt_bytes(request.bounds.hi)) implies
        s.sessions[txn].held.contains_key(k)
        && s.sessions[txn].held[k] == (crate::sharding_placement::Grant {
            owner:request.identity.grant.owner as int,epoch:request.identity.grant.epoch as nat}) by {
        if !request.identity.fixed_coordinate {
            let i = choose|i: int| 0 <= i < session.holds.len()
                && session.holds[i].covers_range(request.identity.table,request.bounds.lo@,opt_bytes(request.bounds.hi))
                && session.holds[i].grant == request.identity.grant;
            crate::leases::covered_point(session.holds[i],
                crate::leases::range_scope(request.identity.table,request.bounds.lo@,opt_bytes(request.bounds.hi),request.identity.grant),
                labels[k].0,labels[k].1.0);
            assert(session.has(labels[k].0,labels[k].1.0,request.identity.grant));
        }
    }
}

pub proof fn native_fixed_coordinate_scan(t: RouteTable,coordinate_bytes: Seq<u8>,id: TxnId,
    xs: Seq<Exchange>,c: crate::sharding_placement::Constants,s: crate::sharding_placement::State,txn: int,
    labels: Map<int,crate::storage::Cell>,logical: Image,views: EngineViews)
    requires xs.len() > 0, exchanges(xs[0].before.request,views,xs),
        xs[0].before.delivered@ == Seq::<WireRow>::empty(),
        xs[0].before.request.identity.transaction == id,
        xs[0].before.request.identity.table == t.table,
        xs[0].before.request.identity.fixed_coordinate,
        xs[0].before.request.identity.coordinate@ == coordinate_bytes,
        xs[0].before.request.identity.grant == route(t.boundaries@,coordinate_bytes),
        xs[0].before.request.bounds.cursor is None,
        crate::sharding_placement::proofs::inv(c,s), read_view_embedding(c,s,labels,logical,views,id),
        lease_embedding(s,txn,labels,id,xs[0].leases),
    ensures interval_exact(xs[0].before.request.bounds.reverse,xs[0].before.request.bounds.lo@,
        opt_bytes(xs[0].before.request.bounds.hi),project(logical,t.table,Some(coordinate_bytes)),
        xs.last().after.delivered@),
{
    let request = xs[0].before.request;
    transcript_complete(request,views,xs);
    admission_to_model_hold(s,txn,labels,request,xs[0].leases);
    complete_pages(&request.bounds,views[target(request.identity)],transcript(xs));
    assert forall|k: Seq<u8>,v: Seq<u8>| contains_row(page_rows(transcript(xs)),k,v) <==>
        project(logical,t.table,Some(coordinate_bytes)).contains_key(k)
        && project(logical,t.table,Some(coordinate_bytes))[k] == v
        && inside(k,request.bounds.lo@,opt_bytes(request.bounds.hi)) by {
        cmp_laws(request.bounds.lo@,k);
        if inside(k,request.bounds.lo@,opt_bytes(request.bounds.hi)) {
            admitted_snapshot_matches_logical(c,s,txn,labels,logical,views,request,k);
        }
    }
}

/// The admission evidence in Exchange is exactly the checked Participant
/// open-range/open-point capability, including the transaction sequence.
pub proof fn participant_admission_evidence(node: crate::participant::Participant,
    request: crate::full_scan_core::BoundRequest)
    requires node.wf(), node.owner_view() == request.identity.grant.owner,
        if request.identity.fixed_coordinate {
            node.open_hold(request.identity.transaction,request.identity.table,
                request.identity.coordinate@,request.identity.grant)
        } else {
            node.open_range(request.identity.transaction,request.identity.table,
                request.bounds.lo@,opt_bytes(request.bounds.hi),request.identity.grant)
        },
    ensures admitted(request,node.lease_view()),
{
    node.lease_facts();
    if request.identity.fixed_coordinate {
        node.open_hold_lease(request.identity.transaction,request.identity.table,
            request.identity.coordinate@,request.identity.grant);
    } else {
        node.open_range_lease(request.identity.transaction,request.identity.table,
            request.bounds.lo@,opt_bytes(request.bounds.hi),request.identity.grant);
    }
}

pub enum EngineExit { Exhausted, CallbackStop, Error }
pub struct AddEvent {
    pub before: crate::full_scan_core::Page,
    pub after: crate::full_scan_core::Page,
    pub row: WireRow,
    pub outcome: Result<bool,Status>,
}
/// Exact checked Page::add postcondition; no engine completeness is present.
pub open spec fn add_effect(before: crate::full_scan_core::Page,after: crate::full_scan_core::Page,
    row: WireRow,outcome: Result<bool,Status>) -> bool {
    after.wf() && after@.request == before@.request
    && match outcome {
        Ok(true) => after@.rows@ == before@.rows@.push(row) && after@.error is None && !after@.budget.more,
        Ok(false) => after@.rows == before@.rows && after@.budget.more && after@.error is None,
        Err(_) => after@.rows == before@.rows && after@.error is Some,
    }
}
pub open spec fn assembled(initial: crate::full_scan_core::Page,steps: Seq<AddEvent>) -> crate::full_scan_core::Page {
    if steps.len() == 0 {initial} else {steps.last().after}
}
pub open spec fn offered(steps: Seq<AddEvent>) -> Seq<WireRow> {
    Seq::new(steps.len(),|i: int| steps[i].row)
}
pub open spec fn accepted(steps: Seq<AddEvent>) -> nat {
    if steps.len() > 0 && steps.last().outcome == Ok(false) {(steps.len()-1) as nat} else {steps.len()}
}
pub open spec fn assembly(initial: crate::full_scan_core::Page,steps: Seq<AddEvent>) -> bool
    decreases steps.len(),
{
    steps.len() == 0 || {
        assembly(initial,steps.drop_last())
        && steps.last().before == assembled(initial,steps.drop_last())
        && add_effect(steps.last().before,steps.last().after,steps.last().row,steps.last().outcome)
        && (steps.len() == 1 || steps[steps.len()-2].outcome == Ok(true))
    }
}
/// Induction over every actual callback. A rejecting callback is a real
/// lookahead row, is not included, and leaves `more=true`; all prior callbacks
/// were appended exactly once. No row count threshold is interpreted as EOF.
pub proof fn assembly_prefix(initial: crate::full_scan_core::Page,steps: Seq<AddEvent>)
    requires initial.wf(), initial@.rows@ == Seq::<WireRow>::empty(), !initial@.budget.more,
        assembly(initial,steps), forall|i: int| 0 <= i < steps.len() ==> steps[i].outcome is Ok,
    ensures assembled(initial,steps).wf(), assembled(initial,steps)@.request == initial@.request,
        assembled(initial,steps)@.rows@ == offered(steps).take(accepted(steps) as int),
        assembled(initial,steps)@.budget.more == (accepted(steps) < steps.len()),
        accepted(steps) <= steps.len(),
    decreases steps.len(),
{
    hide(cmp_spec); hide(rows_wire); hide(blob); hide(word32); hide(word64);
    if steps.len() > 0 {
        assert forall|i: int| 0 <= i < steps.drop_last().len() implies steps.drop_last()[i].outcome is Ok by {
            assert(steps.drop_last()[i] == steps[i]);
        }
        assembly_prefix(initial,steps.drop_last());
        assert(offered(steps).drop_last() =~= offered(steps.drop_last()));
        assert(offered(steps).last() == steps.last().row);
        assert(accepted(steps.drop_last()) == steps.drop_last().len());
        assert(offered(steps.drop_last()).take(accepted(steps.drop_last()) as int) =~= offered(steps.drop_last()));
        assert(steps.last().before@.rows@ == offered(steps).drop_last());
        if steps.last().outcome == Ok(true) {
            assert(offered(steps).drop_last().push(steps.last().row) =~= offered(steps));
            assert(offered(steps).take(accepted(steps) as int) =~= offered(steps));
        } else {
            assert(steps.last().outcome == Ok(false));
            assert(offered(steps).take(accepted(steps) as int) =~= offered(steps.drop_last()));
        }
    } else {
        assert(offered(steps) =~= Seq::<WireRow>::empty());
        assert(offered(steps).take(0) =~= Seq::<WireRow>::empty());
    }
}
pub proof fn prefix_membership(q: &Request,image: SnapshotRows,raw: Seq<WireRow>,
    n: nat,eof: bool,k: Seq<u8>,v: Seq<u8>)
    requires exact(q,opt_bytes(q.cursor),image,raw), n <= raw.len(),
        eof ==> n == raw.len(), !eof ==> 0 < n < raw.len(),
    ensures contains_row(raw.take(n as int),k,v) == (
        image.contains_key(k) && image[k] == v && eligible(q,opt_bytes(q.cursor),k)
        && (eof || !eligible(q,last_cursor(raw.take(n as int),opt_bytes(q.cursor)),k))),
{
    hide(cmp_spec);
    let rows = raw.take(n as int);
    if contains_row(rows,k,v) {
        let i = choose|i: int| 0 <= i < rows.len() && rows[i].key == k && rows[i].value == v;
        assert(raw[i] == rows[i]); assert(contains_row(raw,k,v));
        if !eof {
            assert(rows.last() == raw[n as int-1]);
            cmp_symmetry(raw[i].key,rows.last().key);
            if i < n-1 {assert(precedes(q.reverse,raw[i].key,raw[n as int-1].key));}
        }
    }
    if image.contains_key(k) && image[k] == v && eligible(q,opt_bytes(q.cursor),k)
        && (eof || !eligible(q,last_cursor(rows,opt_bytes(q.cursor)),k)) {
        assert(contains_row(raw,k,v));
        let i = choose|i: int| 0 <= i < raw.len() && raw[i].key == k && raw[i].value == v;
        if !eof && i >= n {
            assert(rows.last() == raw[n as int-1]);
            assert(precedes(q.reverse,raw[n as int-1].key,raw[i].key));
            cmp_symmetry(k,rows.last().key);
            assert(eligible(q,last_cursor(rows,opt_bytes(q.cursor)),k));
        }
        assert(i < n);
        assert(rows[i] == raw[i]);
        assert(contains_row(rows,k,v));
    }
}
pub proof fn prefix_is_truthful_page(q: &Request,image: SnapshotRows,raw: Seq<WireRow>,n: nat,eof: bool)
    requires exact(q,opt_bytes(q.cursor),image,raw), n <= raw.len(), n <= PAGE_ROWS,
        eof ==> n == raw.len(), !eof ==> 0 < n < raw.len(),
    ensures page_complete(q,image,raw.take(n as int),eof),
{
    hide(cmp_spec);
    let rows = raw.take(n as int);
    assert forall|i: int| 0 <= i < rows.len() implies q.follows_spec(#[trigger] rows[i].key,
        if i == 0 {opt_bytes(q.cursor)} else {Some(rows[i-1].key)}) by {
        assert(rows[i] == raw[i]);
        assert(contains_row(raw,raw[i].key,raw[i].value));
        if i > 0 {
            assert(rows[i-1] == raw[i-1]);
            assert(precedes(q.reverse,raw[i-1].key,raw[i].key));
            cmp_symmetry(raw[i].key,raw[i-1].key);
        }
    }
    assert forall|k: Seq<u8>,v: Seq<u8>| contains_row(rows,k,v) <==>
        image.contains_key(k) && image[k] == v && eligible(q,opt_bytes(q.cursor),k)
        && (eof || !eligible(q,last_cursor(rows,opt_bytes(q.cursor)),k)) by {
        prefix_membership(q,image,raw,n,eof,k,v);
    }
}
/// This derives the former page-completeness/EOF assumption from the raw
/// engine contract and the checked sharding callback accumulator.
pub proof fn assembled_engine_page(views: EngineViews,request: crate::full_scan_core::BoundRequest,
    raw: Seq<WireRow>,steps: Seq<AddEvent>,exit: EngineExit,initial: crate::full_scan_core::Page)
    requires trusted_transaction_scan(views,request,raw,steps,exit), !(exit is Error),
        initial.wf(), initial@.request == request, initial@.rows@ == Seq::<WireRow>::empty(),
        !initial@.budget.more, initial@.error is None, assembly(initial,steps),
    ensures page_complete(&request.bounds,views[target(request.identity)],assembled(initial,steps)@.rows@,
        !assembled(initial,steps)@.budget.more),
        (!assembled(initial,steps)@.budget.more) == (exit is Exhausted),
{
    assert forall|i: int| 0 <= i < steps.len() implies steps[i].outcome is Ok by {
        if exit is CallbackStop && i == steps.len()-1 {assert(steps[i] == steps.last());}
    }
    assembly_prefix(initial,steps);
    assembled(initial,steps).facts();
    assert(offered(steps) =~= raw.take(steps.len() as int));
    assert(offered(steps).take(accepted(steps) as int) =~= raw.take(accepted(steps) as int));
    if exit is CallbackStop {
        assert(assembled(initial,steps)@.budget.more);
        assert(accepted(steps) > 0);
    } else {assert(accepted(steps) == raw.len());}
    prefix_is_truthful_page(&request.bounds,views[target(request.identity)],raw,accepted(steps),
        !assembled(initial,steps)@.budget.more);
}

pub proof fn failed_or_mismatched_reply_is_not_exchange(e: Exchange,views: EngineViews)
    requires e.reply_status != Status::Ok || e.reply_number != e.request_number || e.engine_exit is Error,
    ensures !exchange(e,views),
{}

/// Vacuity check at the raw interface: a successful empty engine iterator
/// yields an actual empty sharding page, not merely a valid EOF header.
pub proof fn empty_raw_scan_enabled(initial: crate::full_scan_core::Page,views: EngineViews)
    requires initial.wf(), initial@.rows@ == Seq::<WireRow>::empty(),
        initial@.error is None, !initial@.budget.more,
        views.contains_key(target(initial@.request.identity)),
        views[target(initial@.request.identity)] == vstd::imap::IMap::<Seq<u8>,Seq<u8>>::empty(),
    ensures trusted_transaction_scan(views,initial@.request,Seq::empty(),Seq::empty(),EngineExit::Exhausted),
        assembly(initial,Seq::empty()),
        page_complete(&initial@.request.bounds,views[target(initial@.request.identity)],initial@.rows@,true),
{
    assert(exact(&initial@.request.bounds,opt_bytes(initial@.request.bounds.cursor),
        views[target(initial@.request.identity)],Seq::empty()));
    assembled_engine_page(views,initial@.request,Seq::empty(),Seq::empty(),EngineExit::Exhausted,initial);
}
} // verus!
