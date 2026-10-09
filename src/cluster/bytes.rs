//! Allocation-free lexicographic routing coordinates.
use vstd::prelude::*;
use crate::types::KeyRange;
verus! {
pub open spec fn prefix(a: Seq<u8>, b: Seq<u8>, n: int) -> bool {
    0 <= n <= a.len() && n <= b.len()
        && forall|j: int| 0 <= j < n ==> a[j] == b[j]
}
pub open spec fn lex_lt(a: Seq<u8>, b: Seq<u8>) -> bool {
    (a.len() < b.len() && prefix(a,b,a.len() as int))
    || exists|i: int| 0 <= i < a.len() && i < b.len()
        && prefix(a,b,i) && a[i] < b[i]
}
pub open spec fn cmp_spec(a: Seq<u8>, b: Seq<u8>) -> int {
    if a == b { 0 } else if lex_lt(a,b) { -1 } else { 1 }
}
pub proof fn mismatch(a: Seq<u8>, b: Seq<u8>, i: int)
    requires 0 <= i < a.len(), i < b.len(), prefix(a,b,i), a[i] < b[i],
    ensures lex_lt(a,b), !lex_lt(b,a), a != b,
{
    assert forall|j: int| 0 <= j < b.len() && j < a.len()
        && prefix(b,a,j) implies !(b[j] < a[j]) by {
        if j < i {} else if j == i {} else { assert(b[i] == a[i]); }
    }
    if b.len() < a.len() && prefix(b,a,b.len() as int) { assert(b[i] == a[i]); }
}
pub proof fn shorter(a: Seq<u8>, b: Seq<u8>)
    requires a.len() < b.len(), prefix(a,b,a.len() as int),
    ensures lex_lt(a,b), !lex_lt(b,a),
{
    assert forall|j: int| 0 <= j < b.len() && j < a.len()
        && prefix(b,a,j) implies !(b[j] < a[j]) by { assert(a[j] == b[j]); }
}
pub proof fn cmp_laws(a: Seq<u8>, b: Seq<u8>)
    ensures -1 <= cmp_spec(a,b) <= 1,
        (cmp_spec(a,b) == 0) == (a == b),
        (cmp_spec(a,b) < 0) == lex_lt(a,b),
        cmp_spec(a,b) == -cmp_spec(b,a),
        cmp_spec(Seq::empty(),a) <= 0,
{
    if a == b {
        assert(!lex_lt(a,b));
    } else if lex_lt(a,b) {
        if a.len() < b.len() && prefix(a,b,a.len() as int) { shorter(a,b); }
        else {
            let i = choose|i: int| 0 <= i < a.len() && i < b.len()
                && prefix(a,b,i) && a[i] < b[i];
            mismatch(a,b,i);
        }
    } else {
        if prefix(a,b,if a.len() < b.len() { a.len() as int } else { b.len() as int }) {
            if a.len() == b.len() { assert(a =~= b); }
            else { assert(prefix(b,a,b.len() as int)); shorter(b,a); }
        } else {
            let n = if a.len() < b.len() { a.len() as int } else { b.len() as int };
            let i = choose|i: int| 0 <= i < n && a[i] != b[i];
            least_difference(a,b,i);
            let k = choose|k: int| 0 <= k <= i && prefix(a,b,k) && a[k] != b[k];
            if a[k] < b[k] { mismatch(a,b,k); }
            else { assert(prefix(b,a,k)); mismatch(b,a,k); }
        }
    }
    if a.len() == 0 { assert(a =~= Seq::<u8>::empty()); }
    else { assert(prefix(Seq::empty(),a,0)); }
}
pub proof fn least_difference(a: Seq<u8>, b: Seq<u8>, i: int)
    requires 0 <= i < a.len(), i < b.len(), a[i] != b[i],
    ensures exists|k: int| 0 <= k <= i && prefix(a,b,k) && a[k] != b[k],
    decreases i,
{
    if prefix(a,b,i) { assert(exists|k: int| 0 <= k <= i && prefix(a,b,k) && a[k] != b[k]); }
    else {
        let j = choose|j: int| 0 <= j < i && a[j] != b[j];
        least_difference(a,b,j);
    }
}
pub proof fn cmp_trans(a: Seq<u8>, b: Seq<u8>, c: Seq<u8>)
    requires cmp_spec(a,b) <= 0, cmp_spec(b,c) <= 0,
    ensures cmp_spec(a,c) <= 0,
        cmp_spec(a,b) < 0 || cmp_spec(b,c) < 0 ==> cmp_spec(a,c) < 0,
{
    cmp_laws(a,b); cmp_laws(b,c); cmp_laws(a,c);
    if a != b && b != c {
        let i = if prefix(a,b,a.len() as int) && a.len() < b.len() { a.len() as int }
            else { choose|i: int| 0 <= i < a.len() && i < b.len() && prefix(a,b,i) && a[i] < b[i] };
        let j = if prefix(b,c,b.len() as int) && b.len() < c.len() { b.len() as int }
            else { choose|j: int| 0 <= j < b.len() && j < c.len() && prefix(b,c,j) && b[j] < c[j] };
        let k = if i < j { i } else { j };
        assert(prefix(a,c,k)) by {
            assert forall|x: int| 0 <= x < k implies a[x] == c[x] by {}
        }
        if i < j {
            if i == a.len() { shorter(a,c); } else { mismatch(a,c,i); }
        } else if j < i { mismatch(a,c,j); }
        else if i == a.len() { shorter(a,c); } else { mismatch(a,c,i); }
    }
}
pub fn compare(a: &[u8], b: &[u8]) -> (r: i32)
    ensures r as int == cmp_spec(a@,b@),
{
    let mut i = 0usize;
    while i < a.len() && i < b.len()
        invariant i <= a.len(), i <= b.len(), prefix(a@,b@,i as int),
        decreases a.len() - i,
    {
        if a[i] < b[i] { proof { mismatch(a@,b@,i as int); } return -1; }
        if a[i] > b[i] { proof { assert(prefix(b@,a@,i as int)); mismatch(b@,a@,i as int); } return 1; }
        i += 1;
    }
    if a.len() < b.len() { proof { shorter(a@,b@); } -1 }
    else if a.len() > b.len() { proof { assert(prefix(b@,a@,b.len() as int)); shorter(b@,a@); } 1 }
    else { proof { assert(a@ =~= b@); } 0 }
}
pub fn copy_bytes(a: &[u8]) -> (r: Vec<u8>)
    ensures r@ == a@,
{
    let mut r = Vec::with_capacity(a.len());
    let mut i = 0usize;
    while i < a.len()
        invariant i <= a.len(), r@ == a@.take(i as int),
        decreases a.len() - i,
    {
        r.push(a[i]); i += 1;
        proof { assert(r@ =~= a@.take(i as int)); }
    }
    r
}
pub open spec fn contains_spec(r: KeyRange, table: u64, key: Seq<u8>) -> bool {
    r.table == table && cmp_spec(r.lo@,key) <= 0
        && match r.hi { Some(h) => cmp_spec(key,h@) < 0, None => true }
}
pub fn contains(r: &KeyRange, table: u64, key: &[u8]) -> (yes: bool)
    ensures yes == contains_spec(*r,table,key@),
{
    if r.table != table || compare(&r.lo,key) > 0 { return false; }
    match &r.hi { Some(h) => compare(key,h) < 0, None => true }
}
} // verus!
