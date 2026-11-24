//! The executed tombstone-hop loop. Exact enumeration requires a frozen,
//! drained cell range. Live precopy uses the same loop only as advisory I/O.
use vstd::prelude::*;
use super::status::SHARD_EXHAUSTED;

verus! {

pub enum Hop<R> {
    Row(R),
    End,
    Resume(Vec<u8>),
}

pub open spec fn cursor(bytes: Option<&[u8]>) -> Option<Seq<u8>> {
    match bytes { Some(key) => Some(key@), None => None }
}

pub trait Walker {
    type Row;
    /// E6. Admission, source drain, and retained metadata stabilize live rows.
    spec fn stable(&self) -> bool;
    spec fn first(&self) -> Option<Self::Row>;
    /// Every live candidate is strictly after this cursor.
    spec fn skipped_only_absences(&self, after: Option<Seq<u8>>) -> bool;

    /// E7. One certified hop. Its attempt is closed before returning. A resume
    /// cursor skips only tombstones. On a stable range, Row/End is exact.
    fn next(&mut self, after: Option<&[u8]>) -> (result: Result<Hop<Self::Row>, u32>)
        ensures
            final(self).stable() == old(self).stable(),
            old(self).stable() ==> final(self).first() == old(self).first(),
            old(self).stable() && (after is None || old(self).skipped_only_absences(cursor(after))) ==>
                match result {
                    Ok(Hop::Row(row)) => final(self).first() == Some(row),
                    Ok(Hop::End) => final(self).first() is None,
                    Ok(Hop::Resume(key)) => final(self).skipped_only_absences(Some(key@)),
                    Err(_) => true,
                };
}

/// Exactness on a stable range, including all skipped hops. An explicit hop
/// budget makes the loop total even for an indefinitely changing live precopy.
pub fn scan_first<E: Walker>(engine: &mut E) -> (answer: Result<Option<E::Row>, u32>)
    ensures old(engine).stable() && answer is Ok ==> answer->Ok_0 == old(engine).first(),
{
    let mut after: Option<Vec<u8>> = None;
    let mut remaining: u64 = u64::MAX;
    while remaining > 0
        invariant
            engine.stable() == old(engine).stable(),
            engine.stable() ==> engine.first() == old(engine).first(),
            engine.stable() ==> match after {
                None => true,
                Some(key) => engine.skipped_only_absences(Some(key@)),
            },
        decreases remaining,
    {
        remaining -= 1;
        let step = match &after {
            None => engine.next(None),
            Some(key) => engine.next(Some(key.as_slice())),
        };
        match step {
            Ok(Hop::Row(row)) => return Ok(Some(row)),
            Ok(Hop::End) => return Ok(None),
            Ok(Hop::Resume(key)) => after = Some(key),
            Err(status) => return Err(status),
        }
    }
    Err(SHARD_EXHAUSTED)
}

} // verus!
