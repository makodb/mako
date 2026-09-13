//! Compact terminal outcomes and their fail-closed decode.
//!
//! The decode rules are those of the C ABI engine's outcome types: a report
//! is internally consistent only when its commit disposition, cleanup result,
//! and record/holder witnesses could have been produced by one run of the
//! terminal protocol. Anything else makes the cache terminate rather than
//! release or retry a slot whose obligation it cannot prove.

use std::num::NonZeroU64;

use mako_engine_api::{
    CommitDisposition, CommitHolderReport, CommitRecordReport, CommitReport, Error, MakoTimestamp,
};

/// Whether the report describes a definite terminal outcome or a quarantine.
pub(crate) fn report_is_well_formed(report: &CommitReport) -> bool {
    match (report.disposition, report.cleanup) {
        (CommitDisposition::Committed, Ok(())) => true,
        (CommitDisposition::Aborted(error), Ok(())) => matches!(
            error,
            Error::Conflict
                | Error::CommitHookRejected
                | Error::TimestampExhausted
                | Error::InvalidArgument
                | Error::OutOfMemory
                | Error::Internal
                | Error::TransactionTooLarge
                | Error::TransactionFinished
        ),
        (_, Err(Error::WorkerPoisoned)) => true,
        _ => false,
    }
}

fn quarantined(report: &CommitReport) -> bool {
    report.cleanup == Err(Error::WorkerPoisoned)
}

const UNKNOWN_REPORT: CommitReport = CommitReport {
    disposition: CommitDisposition::Unknown(Error::Internal),
    cleanup: Err(Error::Internal),
};

/// Compact result from the fused one-put record terminals.
#[derive(Debug, Clone, Copy)]
#[must_use = "a bound record outcome must be published or decoded and pinned before acknowledgement"]
pub struct TrustedUncheckedOnePutRecordOutcome {
    pub(crate) report: CommitReport,
    pub(crate) record_bound: bool,
    pub(crate) record_written: bool,
}

impl TrustedUncheckedOnePutRecordOutcome {
    /// Whether the engine returned the exact ordinary committed outcome.
    #[inline(always)]
    pub fn is_committed(self) -> bool {
        self.report.disposition == CommitDisposition::Committed
            && self.report.cleanup.is_ok()
            && self.record_bound
            && self.record_written
    }

    /// Fail-closed completion-contract decode.
    #[cold]
    #[inline(never)]
    pub fn into_report(self) -> CommitRecordReport {
        let mut contract_valid =
            report_is_well_formed(&self.report) && (!self.record_written || self.record_bound);
        if self.report.disposition == CommitDisposition::Committed && !self.record_written {
            contract_valid = false;
        }
        if self.record_bound
            && !self.record_written
            && self.report.disposition != CommitDisposition::Aborted(Error::CommitHookRejected)
            && !quarantined(&self.report)
        {
            contract_valid = false;
        }
        CommitRecordReport {
            commit: if contract_valid {
                self.report
            } else {
                UNKNOWN_REPORT
            },
            completion_contract_valid: contract_valid,
            record_bound: self.record_bound,
            record_written: contract_valid && self.record_written,
        }
    }
}

/// Compact outcome of a concurrent engine-ordered one-put terminal.
#[derive(Debug, Clone, Copy)]
#[must_use = "an accepted engine order must be adopted before acknowledgement"]
pub struct TrustedNativeOrderedOnePutRecordOutcome {
    pub(crate) inner: TrustedUncheckedOnePutRecordOutcome,
    pub(crate) ordered_sequence: u64,
    pub(crate) timestamp_stamp: u64,
    pub(crate) target_bound: bool,
    pub(crate) native_holder_ready: u64,
}

impl TrustedNativeOrderedOnePutRecordOutcome {
    /// Whether the `(timestamp, sequence)` pair is both zero or both valid.
    #[inline(always)]
    pub fn order_witness_valid(self) -> bool {
        if self.ordered_sequence == 0 {
            self.timestamp_stamp == 0
        } else {
            mako_clock::expand(self.timestamp_stamp).is_some()
        }
    }

    /// The accepted order, when the engine assigned both halves.
    #[inline(always)]
    pub fn accepted_order(self) -> Option<(MakoTimestamp, NonZeroU64)> {
        if !self.order_witness_valid() {
            return None;
        }
        Some((
            mako_clock::expand(self.timestamp_stamp)?,
            NonZeroU64::new(self.ordered_sequence)?,
        ))
    }

    /// Whether the engine committed, cleaned up, and completed the target.
    #[inline(always)]
    pub fn is_committed(self) -> bool {
        self.order_witness_valid()
            && self.ordered_sequence != 0
            && self.target_bound
            && self.inner.is_committed()
    }

    /// Whether the holder terminal published READY itself.
    #[inline(always)]
    pub fn native_holder_ready(self) -> bool {
        self.native_holder_ready == 1
    }

    /// Fail-closed decode.
    #[cold]
    #[inline(never)]
    pub fn into_report(self) -> CommitRecordReport {
        let order_pair_valid = self.order_witness_valid();
        let mut report = self.inner.into_report();
        if !order_pair_valid
            || (self.inner.record_written && !self.target_bound)
            || self.native_holder_ready > 1
            || (self.native_holder_ready == 1 && !self.is_committed())
        {
            report.completion_contract_valid = false;
            report.record_written = false;
        }
        report
    }
}

impl mako_engine_api::NativeOrderedRecordOutcome for TrustedNativeOrderedOnePutRecordOutcome {
    fn order_witness_valid(self) -> bool {
        Self::order_witness_valid(self)
    }
    fn accepted_order(self) -> Option<(MakoTimestamp, NonZeroU64)> {
        Self::accepted_order(self)
    }
    fn is_committed(self) -> bool {
        Self::is_committed(self)
    }
    fn native_holder_ready(self) -> bool {
        Self::native_holder_ready(self)
    }
    fn into_report(self) -> CommitRecordReport {
        Self::into_report(self)
    }
}

/// Compact result of the callback-free preselected one-put record terminal.
///
/// `record_state` is `(timestamp_stamp << 1) | written`.
#[derive(Debug, Clone, Copy)]
#[must_use = "a preselected record outcome must be published, released, or decoded and pinned"]
pub struct TrustedPreselectedUncheckedOnePutRecordOutcome {
    pub(crate) report: CommitReport,
    pub(crate) record_state: u64,
}

impl TrustedPreselectedUncheckedOnePutRecordOutcome {
    /// Timestamp with which the engine accepted the target, if nonzero.
    #[inline(always)]
    pub fn accepted_timestamp(self) -> Option<MakoTimestamp> {
        mako_clock::expand(self.record_state >> 1)
    }

    /// Raw complete-record witness bit.
    #[inline(always)]
    pub const fn record_written(self) -> bool {
        self.record_state & 1 != 0
    }

    /// Whether the engine returned the exact ordinary committed outcome.
    #[inline(always)]
    pub fn is_committed(self) -> bool {
        self.report.disposition == CommitDisposition::Committed
            && self.report.cleanup.is_ok()
            && self.record_written()
            && self.accepted_timestamp().is_some()
    }

    /// Fail-closed decode.
    #[cold]
    #[inline(never)]
    pub fn into_report(self) -> CommitRecordReport {
        let timestamp_stamp = self.record_state >> 1;
        let record_bound = self.accepted_timestamp().is_some();
        let witness_claimed = self.record_written();
        let timestamp_valid = timestamp_stamp == 0 || record_bound;
        let mut contract_valid = timestamp_valid
            && (!witness_claimed || record_bound)
            && report_is_well_formed(&self.report);
        if record_bound {
            contract_valid &= witness_claimed
                && (self.report.disposition == CommitDisposition::Committed
                    || quarantined(&self.report));
        } else {
            contract_valid &=
                !witness_claimed && self.report.disposition != CommitDisposition::Committed;
        }
        CommitRecordReport {
            commit: if contract_valid {
                self.report
            } else {
                UNKNOWN_REPORT
            },
            completion_contract_valid: contract_valid,
            record_bound,
            record_written: contract_valid && witness_claimed,
        }
    }
}

impl mako_engine_api::PreselectedRecordOutcome for TrustedPreselectedUncheckedOnePutRecordOutcome {
    fn accepted_timestamp(self) -> Option<MakoTimestamp> {
        Self::accepted_timestamp(self)
    }
    fn record_written(self) -> bool {
        Self::record_written(self)
    }
    fn is_committed(self) -> bool {
        Self::is_committed(self)
    }
    fn into_report(self) -> CommitRecordReport {
        Self::into_report(self)
    }
}

/// Compact result of the callback-free preselected one-put holder terminals.
///
/// `holder_state` is `(timestamp_stamp << 1) | sealed`.
#[derive(Debug, Clone, Copy)]
#[must_use = "a preselected holder outcome must be published, released, or decoded and pinned"]
pub struct TrustedPreselectedUncheckedOnePutHolderOutcome {
    pub(crate) report: CommitReport,
    pub(crate) holder_state: u64,
}

impl TrustedPreselectedUncheckedOnePutHolderOutcome {
    /// Timestamp only for the exact ordinary success word.
    #[inline(always)]
    pub fn committed_timestamp(self) -> Option<MakoTimestamp> {
        if self.report.disposition == CommitDisposition::Committed
            && self.report.cleanup.is_ok()
            && self.holder_sealed()
        {
            mako_clock::expand(self.holder_state >> 1)
        } else {
            None
        }
    }

    /// Accepted Mako timestamp, if nonzero and in range.
    #[inline(always)]
    pub fn accepted_timestamp(self) -> Option<MakoTimestamp> {
        mako_clock::expand(self.holder_state >> 1)
    }

    /// Whether the engine sealed the complete payload.
    #[inline(always)]
    pub const fn holder_sealed(self) -> bool {
        self.holder_state & 1 != 0
    }

    /// Whether this is the exact ordinary committed outcome.
    #[inline(always)]
    pub fn is_committed(self) -> bool {
        self.committed_timestamp().is_some()
    }

    /// Fail-closed decode.
    #[cold]
    #[inline(never)]
    pub fn into_report(self) -> CommitHolderReport {
        let timestamp_stamp = self.holder_state >> 1;
        let holder_bound = self.accepted_timestamp().is_some();
        let sealed_claimed = self.holder_sealed();
        let timestamp_valid = timestamp_stamp == 0 || holder_bound;
        let mut contract_valid = timestamp_valid
            && (!sealed_claimed || holder_bound)
            && report_is_well_formed(&self.report);
        if holder_bound {
            contract_valid &= sealed_claimed
                && (self.report.disposition == CommitDisposition::Committed
                    || quarantined(&self.report));
        } else {
            contract_valid &=
                !sealed_claimed && self.report.disposition != CommitDisposition::Committed;
        }
        CommitHolderReport {
            commit: if contract_valid {
                self.report
            } else {
                UNKNOWN_REPORT
            },
            completion_contract_valid: contract_valid,
            holder_bound,
            holder_sealed: contract_valid && sealed_claimed,
        }
    }
}

impl mako_engine_api::PreselectedHolderOutcome for TrustedPreselectedUncheckedOnePutHolderOutcome {
    fn committed_timestamp(self) -> Option<MakoTimestamp> {
        Self::committed_timestamp(self)
    }
    fn accepted_timestamp(self) -> Option<MakoTimestamp> {
        Self::accepted_timestamp(self)
    }
    fn holder_sealed(self) -> bool {
        Self::holder_sealed(self)
    }
    fn is_committed(self) -> bool {
        Self::is_committed(self)
    }
    fn into_report(self) -> CommitHolderReport {
        Self::into_report(self)
    }
}
