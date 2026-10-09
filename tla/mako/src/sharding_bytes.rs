//! Canonical lossless storage-value encoding shared by protocol and native proofs.
use vstd::prelude::*;
verus! {
/// Ghost-only base-256 encoding with a leading sentinel. The sentinel preserves
/// length, including leading zero bytes; no encoding work enters native code.
pub closed spec fn value_code(value: Seq<u8>) -> int
    decreases value.len(),
{
    if value.len() == 0 { 1 }
    else { 256 * value_code(value.drop_last()) + value.last() as int }
}
pub proof fn value_code_positive(value: Seq<u8>)
    ensures value_code(value) >= 1,
        value.len() > 0 ==> value_code(value) >= 256,
    decreases value.len(),
{
    if value.len() > 0 { value_code_positive(value.drop_last()); }
}
pub proof fn value_code_injective(a: Seq<u8>, b: Seq<u8>)
    requires value_code(a) == value_code(b),
    ensures a == b,
    decreases a.len(),
{
    value_code_positive(a);
    value_code_positive(b);
    if a.len() == 0 || b.len() == 0 {
        assert(a =~= b);
    } else {
        let x = value_code(a.drop_last());
        let y = value_code(b.drop_last());
        if x < y {
            assert(x + 1 <= y);
            assert(256 * x + (a.last() as int) < 256 * y + (b.last() as int));
        } else if y < x {
            assert(y + 1 <= x);
            assert(256 * y + (b.last() as int) < 256 * x + (a.last() as int));
        }
        assert(x == y);
        assert(a.last() == b.last());
        value_code_injective(a.drop_last(), b.drop_last());
        assert(a.drop_last().push(a.last()) =~= a);
        assert(b.drop_last().push(b.last()) =~= b);
    }
}
pub open spec fn value_option(value: Option<Seq<u8>>) -> Option<int> {
    match value { Some(bytes) => Some(value_code(bytes)), None => None }
}
pub proof fn value_option_injective(a: Option<Seq<u8>>, b: Option<Seq<u8>>)
    requires value_option(a) == value_option(b),
    ensures a == b,
{
    if let (Some(x), Some(y)) = (a,b) { value_code_injective(x,y); }
}
}
