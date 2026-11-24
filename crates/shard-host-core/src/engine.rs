//! The engine contracts used by the verified one-key storage operation.
//! Implementations must supply these contracts for an exclusively admitted
//! migration cell. Raw pointers, native validation and log publication remain
//! obligations of the adapter, not facts proved by this trait declaration.
use vstd::prelude::*;

verus! {

pub const ENGINE_MISS: i32 = 1;

pub struct Report {
    pub status: i32,
    pub disposition: u32,
}

pub open spec fn bytes(value: Option<&[u8]>) -> Option<Seq<u8>> {
    match value { Some(v) => Some(v@), None => None }
}

pub trait WriteEngine {
    /// The selected cell, protected by the migration's admission and drain.
    spec fn value(&self) -> Option<Seq<u8>>;
    spec fn active(&self) -> bool;
    spec fn pending(&self) -> Option<Seq<u8>>;
    spec fn staged_write(&self) -> bool;
    spec fn certified(&self) -> bool;

    fn idle(&self) -> (idle: bool)
        ensures idle == !self.active();

    /// E1. Begin cannot install a write. Even a failed begin may need closing.
    fn begin(&mut self) -> (status: i32)
        ensures final(self).value() == old(self).value(),
            status == super::status::ENGINE_OK ==> final(self).active();

    /// E2. Stage observes the cell and records the requested effect. An absent
    /// deletion is a read that still requires certification.
    fn stage(&mut self, value: Option<&[u8]>) -> (status: i32)
        requires old(self).active(),
        ensures final(self).value() == old(self).value(),
            final(self).active() == old(self).active(),
            status == super::status::ENGINE_OK ==> final(self).pending() == bytes(value) && final(self).staged_write(),
            status == ENGINE_MISS ==> value is None && final(self).value() is None
                && !final(self).staged_write();

    /// E3. Validation has no installed effect.
    fn validate(&mut self) -> (status: i32)
        requires old(self).active(), !old(self).staged_write(),
        ensures final(self).value() == old(self).value(),
            final(self).active() == old(self).active(),
            final(self).staged_write() == old(self).staged_write(),
            status == super::status::ENGINE_OK ==> final(self).certified();

    /// E4. Finish is used to abort, or to finish a validated read-only attempt.
    /// A successful close releases the attempt. No write is installed here.
    fn finish(&mut self, committed: bool) -> (report: Report)
        requires committed ==> old(self).active() && !old(self).staged_write() && old(self).certified(),
        ensures final(self).value() == old(self).value(),
            report.status == super::status::ENGINE_OK ==> !final(self).active(),
            !super::status::indefinite(report.status) ==> !final(self).active();

    /// E5. Commit runs the distributed protocol, including its log callback.
    /// Definite failure preserves the cell; definite success installs the
    /// staged value. Indeterminate results must terminate the process.
    fn commit(&mut self) -> (report: Report)
        requires old(self).active(), old(self).staged_write(),
        ensures
            super::status::succeeded(super::status::Stage::Commit, report.status, report.disposition) ==>
                final(self).value() == old(self).pending() && !final(self).active(),
            super::status::commit_refused(report.status, report.disposition) ==>
                final(self).value() == old(self).value() && !final(self).active();
}

} // verus!
