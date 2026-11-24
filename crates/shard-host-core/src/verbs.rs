//! Executed begin/stage/certify/close/commit control flow for put and remove.
use super::engine::*;
use super::status::*;
use vstd::prelude::*;

verus! {

fn close<E: WriteEngine>(engine: &mut E, failed: i32) -> (answer: Mapped)
    ensures final(engine).value() == old(engine).value(),
        answer != Mapped::FailStop ==> !final(engine).active(),
        answer != Mapped::Shard(SHARD_OK),
{
    let report = engine.finish(false);
    if report.status != ENGINE_OK {
        Mapped::FailStop
    } else {
        map_engine(Stage::Closed, failed, report.disposition)
    }
}

/// Total entry point. A caller with an open transaction is refused without
/// touching it. For an idle engine every returned status leaves it idle, and
/// success means exactly the requested cell effect. Errors preserve the cell.
pub fn write_cell<E: WriteEngine>(engine: &mut E, value: Option<&[u8]>) -> (answer: Mapped)
    ensures
        answer == Mapped::Shard(SHARD_OK) ==> final(engine).value() == bytes(value),
        answer != Mapped::FailStop && answer != Mapped::Shard(SHARD_OK)
            ==> final(engine).value() == old(engine).value(),
        !old(engine).active() && answer != Mapped::FailStop ==> !final(engine).active(),
{
    if !engine.idle() { return Mapped::Shard(SHARD_INVALID); }
    let begun = engine.begin();
    if begun != ENGINE_OK { return close(engine, begun); }
    let staged = engine.stage(value);
    if staged == ENGINE_OK {
        let report = engine.commit();
        return map_engine(Stage::Commit, report.status, report.disposition);
    }
    if staged == ENGINE_MISS {
        let validated = engine.validate();
        if validated != ENGINE_OK { return close(engine, validated); }
        let report = engine.finish(true);
        return map_engine(Stage::ReadOnlyFinish, report.status, report.disposition);
    }
    close(engine, staged)
}

} // verus!
