//! Checked export of one borrowed coordinator publication. No routing copy or
//! history is allocated: serialization reads the version and retained table
//! references under the caller's one coordinator mutex, using one output buffer.
use vstd::prelude::*;
use crate::migration::Coordinator;
use crate::routing_codec::{Writer, SNAPSHOT_TAG, table_encoded_size};
use crate::types::Status;
#[cfg(verus_keep_ghost)]
use crate::migration::Native;
#[cfg(verus_keep_ghost)]
use crate::directory::RouteTable;
#[cfg(verus_keep_ghost)]
use crate::routing_codec_proofs::{word64,tables_wire};

verus! {

pub open spec fn published_tables(v: Native) -> Seq<RouteTable> {
    Seq::new(v.current.len(),|i: int| v.snapshots[v.current[i] as int])
}
pub open spec fn publication_wire(v: Native) -> Seq<u8> {
    word64(SNAPSHOT_TAG)+word64(v.published_version)+word64(v.current.len() as u64)
        + tables_wire(published_tables(v))
}

pub fn encode_publication(coordinator: &Coordinator) -> (out: Result<Vec<u8>,Status>)
    requires coordinator.wf(),
    ensures out is Ok ==> out->Ok_0@ == publication_wire(coordinator@),
{
    let count = coordinator.table_count();
    let ghost tables = published_tables(coordinator@);
    let mut size = 24usize;
    let mut i = 0usize;
    while i < count
        invariant coordinator.wf(), count == coordinator@.current.len(),
            tables == published_tables(coordinator@), i <= count,
            size <= isize::MAX as usize,
            size == 24+tables_wire(tables.take(i as int)).len(),
        decreases count-i,
    {
        let table = match coordinator.table_at(i) { Some(t) => t, None => return Err(Status::Invalid) };
        let n = match table_encoded_size(table) { Ok(n) => n, Err(e) => return Err(e) };
        if size > (isize::MAX as usize)-n { return Err(Status::Exhausted); }
        size += n;
        i += 1;
        proof {
            assert(tables.take(i as int).drop_last() == tables.take(i as int-1));
            assert(tables.take(i as int).last() == tables[i as int-1]);
        }
    }
    let mut writer = match Writer::with_capacity(size) { Ok(w) => w, Err(e) => return Err(e) };
    let status = writer.write_u64(SNAPSHOT_TAG);
    if !matches!(status,Status::Ok) { return Err(status); }
    let version = coordinator.published_version();
    let status = writer.write_u64(version);
    if !matches!(status,Status::Ok) { return Err(status); }
    let status = writer.write_u64(count as u64);
    if !matches!(status,Status::Ok) { return Err(status); }
    let mut j = 0usize;
    while j < count
        invariant coordinator.wf(), count == coordinator@.current.len(),
            version == coordinator@.published_version,
            tables == published_tables(coordinator@), j <= count,
            writer.view() == word64(SNAPSHOT_TAG)+word64(version)+word64(count as u64)
                + tables_wire(tables.take(j as int)),
        decreases count-j,
    {
        let table = match coordinator.table_at(j) { Some(t) => t, None => return Err(Status::Invalid) };
        let status = writer.write_table(table);
        if !matches!(status,Status::Ok) { return Err(status); }
        j += 1;
        proof {
            assert(tables.take(j as int).drop_last() == tables.take(j as int-1));
            assert(tables.take(j as int).last() == tables[j as int-1]);
            assert(writer.view() =~= word64(SNAPSHOT_TAG)+word64(version)+word64(count as u64)
                + tables_wire(tables.take(j as int)));
        }
    }
    proof { assert(tables.take(count as int) == tables); }
    Ok(writer.into_bytes())
}

} // verus!
