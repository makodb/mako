use vstd::prelude::*;
use super::*;
use crate::bytes::{cmp_laws,cmp_trans};
verus! {
pub proof fn order_laws(a: Identity,b: Identity)
    ensures -1 <= order(a,b) <= 1, (order(a,b) == 0) == (a == b),
        order(a,b) == -order(b,a),
{
    cmp_laws(a.0,b.0); cmp_laws(a.1,b.1);
}
pub proof fn order_trans(a: Identity,b: Identity,c: Identity)
    requires order(a,b) <= 0, order(b,c) <= 0,
    ensures order(a,c) <= 0,
        order(a,b) < 0 || order(b,c) < 0 ==> order(a,c) < 0,
{
    cmp_laws(a.0,b.0); cmp_laws(b.0,c.0);
    cmp_trans(a.0,b.0,c.0);
    if a.0 == b.0 && b.0 == c.0 { cmp_trans(a.1,b.1,c.1); }
}
pub proof fn advance_cached(image: Image,r: KeyRange,after: Option<Identity>,row: Option<Row>,pick: Identity)
    requires first(image,r,after,row), beyond(after,pick),
        row is Some ==> order(pick,identity(row.unwrap())) < 0,
    ensures first(image,r,Some(pick),row),
{
    assert forall|k: Cell| in_range(r,k) && beyond(Some(pick),k.1)
        implies beyond(after,k.1) by {
        if let Some(a) = after { order_trans(a,pick,k.1); }
    }
}
pub proof fn scan_frame(current: Image,initial: Image,source: Image,r: KeyRange,after: Option<Identity>,row: Option<Row>)
    requires mirrored(current,initial,source,r,after), first(current,r,after,row),
    ensures first(initial,r,after,row),
{
    if let Some(x) = row { assert(value(current,row_cell(r.table,x)) == value(initial,row_cell(r.table,x))); }
    assert forall|k: Cell| in_range(r,k) && beyond(after,k.1)
        implies value(current,k) == value(initial,k) by {}
}
pub open spec fn chosen(s: Option<Row>,d: Option<Row>,pick: Row) -> bool {
    (s is Some && identity(pick) == identity(s.unwrap())
        && (d is Some ==> order(identity(pick),identity(d.unwrap())) <= 0))
    || (d is Some && identity(pick) == identity(d.unwrap())
        && (s is Some ==> order(identity(pick),identity(s.unwrap())) <= 0))
}
pub open spec fn effect(current: Image,source: Image,r: KeyRange,pick: Row) -> Image {
    let k = row_cell(r.table,pick);
    if source.dom().contains(k) { current.insert(k,source[k]) } else { current.remove(k) }
}
pub proof fn step(current: Image,initial: Image,source: Image,r: KeyRange,after: Option<Identity>,s: Option<Row>,d: Option<Row>,pick: Row)
    requires mirrored(current,initial,source,r,after), first(source,r,after,s),
        first(initial,r,after,d), chosen(s,d,pick),
    ensures mirrored(effect(current,source,r,pick),initial,source,r,Some(identity(pick))),
        in_range(r,row_cell(r.table,pick)), beyond(after,identity(pick)),
        source.dom().contains(row_cell(r.table,pick)) ==> s is Some && identity(s.unwrap()) == identity(pick),
        source.dom().contains(row_cell(r.table,pick)) || initial.dom().contains(row_cell(r.table,pick)),
{
    let p = identity(pick);
    order_laws(p,p);
    if let Some(x) = s { order_laws(p,identity(x)); }
    if let Some(x) = d { order_laws(p,identity(x)); }
    assert forall|k: Cell| value(effect(current,source,r,pick),k) ==
        if in_range(r,k) && !beyond(Some(p),k.1) { value(source,k) } else { value(initial,k) } by {
        order_laws(p,k.1);
        if let Some(a) = after {
            order_laws(a,k.1); order_laws(a,p);
            if order(p,k.1) < 0 { order_trans(a,p,k.1); }
            if order(a,k.1) >= 0 { order_trans(k.1,a,p); }
        }
        if in_range(r,k) && beyond(after,k.1) && order(p,k.1) > 0 {
            if let Some(x) = s {
                if source.dom().contains(k) { order_trans(p,identity(x),k.1); }
            }
            if let Some(x) = d {
                if initial.dom().contains(k) { order_trans(p,identity(x),k.1); }
            }
            assert(!source.dom().contains(k) && !initial.dom().contains(k));
        }
    }
}
pub proof fn finish(current: Image,initial: Image,source: Image,r: KeyRange,after: Option<Identity>)
    requires mirrored(current,initial,source,r,after), first(source,r,after,None), first(initial,r,after,None),
    ensures complete(current,initial,source,r),
{
    assert forall|k: Cell| value(current,k) == if in_range(r,k) { value(source,k) } else { value(initial,k) } by {}
}
pub open spec fn remaining(initial: Image,source: Image,r: KeyRange,after: Option<Identity>) -> Set<Cell> {
    initial.dom().union(source.dom()).filter(|k: Cell| in_range(r,k) && beyond(after,k.1))
}
pub proof fn progress(initial: Image,source: Image,r: KeyRange,after: Option<Identity>,pick: Row)
    requires beyond(after,identity(pick)),
        in_range(r,row_cell(r.table,pick)),
        initial.dom().contains(row_cell(r.table,pick)) || source.dom().contains(row_cell(r.table,pick)),
    ensures remaining(initial,source,r,Some(identity(pick))).len() < remaining(initial,source,r,after).len(),
{
    let before = remaining(initial,source,r,after);
    let after_set = remaining(initial,source,r,Some(identity(pick)));
    assert forall|k: Cell| after_set.contains(k) implies before.contains(k) by {
        if let Some(a) = after { order_trans(a,identity(pick),k.1); }
    }
    order_laws(identity(pick),identity(pick));
    assert(before.contains(row_cell(r.table,pick)));
    assert(!after_set.contains(row_cell(r.table,pick)));
    assert(after_set.subset_of(before));
    assert(after_set != before);
    after_set.lemma_subset_not_in_lt(before,row_cell(r.table,pick));
}
pub proof fn frame_effect(current: Image,initial: Image,next: Image,r: KeyRange,cell: Cell,write: Option<Seq<u8>>)
    requires preserves_outside(current,initial,r),in_range(r,cell),
        next == current || next == match write { Some(v) => current.insert(cell,v), None => current.remove(cell) },
    ensures preserves_outside(next,initial,r),
{
    assert forall|k: Cell| !in_range(r,k) implies value(next,k) == value(initial,k) by {
        assert(k != cell);
        assert(value(current,k) == value(initial,k));
    }
}
} // verus!
