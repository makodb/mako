//! Connect the actual Rust host definitions to the actual migration store
//! specifications. The native engine supplies the admitted cell/frame and
//! certified-hop contracts documented in shard-host-core::engine and ::walk.
use vstd::prelude::*;
use crate::{bytes, storage, rust_storage_core as host};
use crate::types::{KeyRange, Row};

verus! {

pub proof fn same_byte_order(a: Seq<u8>, b: Seq<u8>)
    ensures host::order::cmp_spec(a,b) == bytes::cmp_spec(a,b),
{
    assert forall|n:int| host::order::prefix(a,b,n) == bytes::prefix(a,b,n) by {}
    assert(host::order::lex_lt(a,b) == bytes::lex_lt(a,b));
}

pub proof fn same_identity_order(a: storage::Identity,b: storage::Identity)
    ensures host::scan::identity_after(b.0,b.1,a.0,a.1) == (storage::order(a,b) < 0),
{
    same_byte_order(a.0,b.0);
    same_byte_order(a.1,b.1);
    bytes::cmp_laws(a.0,b.0);
}

/// A warehouse's physical keys are admitted by exactly the migration range
/// and cursor predicate, including two warehouses with equal physical keys.
pub proof fn warehouse_bounds_match_store(r: KeyRange,row: Row,after: Option<Row>)
    ensures host::scan::fixed_row_allowed(
        row.coordinate@, row.key@, r.lo@,
        match r.hi { None => Seq::empty(), Some(hi) => hi@ }, r.hi is Some,
        match after { None => Seq::empty(), Some(a) => a.coordinate@ },
        match after { None => Seq::empty(), Some(a) => a.key@ }, after is Some)
        == (storage::in_range(r,storage::row_cell(r.table,row))
            && storage::beyond(storage::row_option(after),storage::identity(row))),
{
    same_byte_order(r.lo@,row.coordinate@);
    if let Some(hi) = r.hi { same_byte_order(row.coordinate@,hi@); }
    if let Some(a) = after { same_identity_order(storage::identity(a),storage::identity(row)); }
}

/// The native engine's one-cell frame plus the verified write_cell result
/// supplies RawStore's whole-window successful put/delete contract.
pub proof fn one_cell_effect_is_store_effect(before: storage::Image, after: storage::Image,
                                            cell: storage::Cell, value: Option<Seq<u8>>)
    requires
        storage::value(after,cell) == value,
        forall|other: storage::Cell| other != cell ==>
            storage::value(after,other) == storage::value(before,other),
    ensures after == match value {
        Some(bytes) => before.insert(cell,bytes),
        None => before.remove(cell),
    },
{
    let expected = match value {
        Some(bytes) => before.insert(cell,bytes),
        None => before.remove(cell),
    };
    assert forall|key:storage::Cell| after.dom().contains(key) == expected.dom().contains(key) by {
        if key != cell { assert(storage::value(after,key) == storage::value(before,key)); }
    }
    assert forall|key:storage::Cell| after.dom().contains(key) implies after[key] == expected[key] by {
        if key != cell { assert(storage::value(after,key) == storage::value(before,key)); }
    }
    assert(after =~= expected);
}

pub proof fn refused_cell_effect_preserves_store(before: storage::Image, after: storage::Image,
                                                cell: storage::Cell)
    requires
        storage::value(after,cell) == storage::value(before,cell),
        forall|other:storage::Cell| other != cell ==>
            storage::value(after,other) == storage::value(before,other),
    ensures after == before,
{
    one_cell_effect_is_store_effect(before,after,cell,storage::value(before,cell));
    if before.dom().contains(cell) { assert(before.insert(cell,before[cell]) =~= before); }
    else { assert(before.remove(cell) =~= before); }
}

} // verus!
