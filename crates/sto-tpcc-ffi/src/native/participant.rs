//! Rust ownership and callback adaptation for the existing retained participant.
use super::*;
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum Mutation {
    Put = 0,
    Insert = 1,
    Remove = 2,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VersionedValue {
    pub value: Option<Vec<u8>>,
    pub time_term: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VersionedRow {
    pub key: Vec<u8>,
    pub value: Vec<u8>,
    pub time_term: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VersionedScan {
    pub rows: Vec<VersionedRow>,
    pub read_dependency: u32,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ParticipantReport {
    pub disposition: u32,
    pub phase: u32,
    pub time_term: u32,
    pub read_dependency: u32,
    pub empty: bool,
}

/// Application operations invoked synchronously by the existing coordinator.
/// The implementation owns remote participant sessions and log admission.
/// Errors after installation retain the coordinator's unknown-outcome semantics.
pub trait ProtocolCallbacks {
    fn stage(
        &mut self,
        shard: u32,
        table: u16,
        mutation: Mutation,
        key: &[u8],
        value: &[u8],
    ) -> Result<()>;
    fn timestamp(&mut self) -> Result<u32>;
    fn validate(&mut self) -> Result<()>;
    fn install(&mut self, timestamp: u32) -> Result<()>;
    fn abort(&mut self) -> Result<()>;
    fn log(&mut self, bytes: &[u8], time_term: u32) -> Result<()>;
}

/// May be retained between messages by one affine worker. Dropping an
/// undecided participant aborts its local state; dropping an installed or
/// uncertain one preserves fencing. Coordinators use `Coordinator` so body
/// errors and unwinds also release their remote participant sessions.
pub struct Participant<'worker> {
    worker: &'worker mut Worker,
    term: u32,
    finished: bool,
}

impl Worker {
    /// Resume a participant retained on this same worker between requests.
    /// This reacquires its exclusive borrow; no raw lifetime is extended.
    pub fn resume_participant(&mut self) -> Result<Participant<'_>> {
        if self.raw.protocol.is_uncertain() {
            return Err(Error {
                kind: ErrorKind::UnknownOutcome,
                message: "participant outcome is unresolved".into(),
            });
        }
        if self.raw.active.is_none() || !self.raw.sto_worker.has_participant() {
            return Err(invalid("worker has no retained participant"));
        }
        let term = self.raw.protocol.term();
        Ok(Participant {
            worker: self,
            term,
            finished: false,
        })
    }

    pub fn begin_coordinator<'worker, 'callbacks>(
        &'worker mut self,
        term: u32,
        callbacks: &'callbacks mut dyn ProtocolCallbacks,
    ) -> Result<Coordinator<'worker, 'callbacks>> {
        Ok(Coordinator {
            participant: Some(self.begin_participant(term)?),
            callbacks,
        })
    }

    pub fn begin_participant(&mut self, term: u32) -> Result<Participant<'_>> {
        // SAFETY: unique access to a live same-thread handle. The core validates
        // term, phase and uncertain-outcome fencing before starting an attempt.
        checked(unsafe { sto_tpcc_protocol_begin(&mut *self.raw, term) })?;
        self.pinned.clear();
        Ok(Participant {
            worker: self,
            term,
            finished: false,
        })
    }
}

impl Participant<'_> {
    /// Release this Rust borrow while keeping the transaction and table pins
    /// on the affine worker. Only `resume_participant` or explicit cleanup may
    /// use the worker until this attempt terminates.
    pub fn retain(mut self) {
        self.finished = true;
    }

    /// Abort remote sessions and local state before installation. Always try
    /// local cleanup even when a remote peer fails or its callback unwinds.
    pub fn abort(mut self, callbacks: &mut dyn ProtocolCallbacks) -> Result<()> {
        let report = self.report()?;
        let remote = if report.phase == 3 || report.disposition == 3 {
            Err(Error {
                kind: ErrorKind::UnknownOutcome,
                message: "cannot abort after installation".into(),
            })
        } else {
            catch_unwind(AssertUnwindSafe(|| callbacks.abort())).unwrap_or_else(|_| {
                Err(Error {
                    kind: ErrorKind::Fatal,
                    message: "abort callback panicked".into(),
                })
            })
        };
        let local = self.worker.abort();
        self.release_finished();
        local.and(remote)
    }

    pub fn report(&mut self) -> Result<ParticipantReport> {
        let mut report = StoTpccProtocolReport::default();
        let mut empty = 0;
        checked(unsafe {
            sto_tpcc_protocol_state(&mut *self.worker.raw, &mut report, &mut empty)
        })?;
        Ok(ParticipantReport {
            disposition: report.disposition,
            phase: report.phase,
            time_term: report.time_term,
            read_dependency: report.read_dependency,
            empty: empty != 0,
        })
    }

    pub fn visibility(&mut self, stable: [Option<u32>; 10], failed: Option<u32>) -> Result<()> {
        let mut known = 0;
        let mut values = [0; 10];
        for (i, value) in stable.into_iter().enumerate() {
            if let Some(value) = value {
                known |= 1 << i;
                values[i] = value;
            }
        }
        checked(unsafe {
            sto_tpcc_protocol_visibility(
                &mut *self.worker.raw,
                values.as_ptr(),
                known,
                failed.unwrap_or(0),
                u32::from(failed.is_some()),
            )
        })
    }

    pub fn get(&mut self, table: &Table, key: &[u8]) -> Result<VersionedValue> {
        self.worker.pin(table)?;
        let mut value = vec![0; 256];
        loop {
            let mut actual = 0;
            let mut time_term = 0;
            let code = unsafe {
                sto_tpcc_protocol_get(
                    &mut *self.worker.raw,
                    &*table.inner,
                    key.as_ptr(),
                    key.len(),
                    value.as_mut_ptr(),
                    value.len(),
                    &mut actual,
                    &mut time_term,
                )
            };
            match code {
                0 => {
                    value.truncate(actual);
                    return Ok(VersionedValue {
                        value: Some(value),
                        time_term,
                    });
                }
                1 => {
                    return Ok(VersionedValue {
                        value: None,
                        time_term,
                    })
                }
                4 => value.resize(actual, 0),
                _ => return Err(error(code)),
            }
        }
    }

    /// Returns false for an insert that found a key or a delete that missed.
    pub fn mutate(
        &mut self,
        table: &Table,
        table_id: u16,
        mutation: Mutation,
        key: &[u8],
        value: &[u8],
        lock_now: bool,
    ) -> Result<bool> {
        self.worker.pin(table)?;
        let code = unsafe {
            sto_tpcc_protocol_mutate(
                &mut *self.worker.raw,
                &*table.inner,
                table_id,
                mutation as u8,
                key.as_ptr(),
                key.len(),
                value.as_ptr(),
                value.len(),
                u32::from(lock_now),
            )
        };
        match code {
            0 => Ok(true),
            1 | 2 => Ok(false),
            _ => Err(error(code)),
        }
    }

    pub fn stage_remote(
        &mut self,
        shard: u32,
        table: u16,
        mutation: Mutation,
        key: &[u8],
        value: &[u8],
    ) -> Result<()> {
        checked(unsafe {
            sto_tpcc_protocol_stage_remote(
                &mut *self.worker.raw,
                shard,
                table,
                mutation as u8,
                key.as_ptr(),
                key.len(),
                value.as_ptr(),
                value.len(),
            )
        })
    }

    pub fn observe_remote_read(&mut self, shard: u32, time_term: u32) -> Result<()> {
        checked(unsafe { sto_tpcc_protocol_observe_read(&mut *self.worker.raw, shard, time_term) })
    }

    /// Scan dependencies include tombstones and traversed rows omitted from the
    /// response. Never infer this bound solely from the returned visible rows.
    pub fn observe_remote_scan(&mut self, shard: u32, dependency: u32) -> Result<()> {
        use mako_clock::distributed::{DistributedTerm, DistributedTimeTerm, DistributedTimestamp};
        let encoded = if dependency == 0 {
            0
        } else {
            let timestamp = DistributedTimestamp::new(dependency)
                .map_err(|e| invalid(&format!("invalid scan dependency: {e}")))?;
            let term = DistributedTerm::new(self.term)
                .map_err(|e| invalid(&format!("invalid participant term: {e}")))?;
            DistributedTimeTerm::new(timestamp, term).encode_legacy()
        };
        self.observe_remote_read(shard, encoded)
    }

    pub fn scan(
        &mut self,
        table: &Table,
        lower: Bound<&[u8]>,
        upper: Bound<&[u8]>,
        reverse: bool,
        limit: usize,
    ) -> Result<VersionedScan> {
        self.worker.pin(table)?;
        let (lo_kind, lo) = bound(lower);
        let (hi_kind, hi) = bound(upper);
        let mut rows = Vec::<VersionedRow>::new();
        let mut visited = 0;
        checked(unsafe {
            sto_tpcc_protocol_scan(
                &mut *self.worker.raw,
                &*table.inner,
                i32::from(reverse),
                lo_kind,
                lo.as_ptr(),
                lo.len(),
                hi_kind,
                hi.as_ptr(),
                hi.len(),
                limit,
                Some(collect_versioned),
                (&mut rows as *mut Vec<VersionedRow>).cast(),
                &mut visited,
            )
        })?;
        Ok(VersionedScan {
            rows,
            read_dependency: self.report()?.read_dependency,
        })
    }

    pub fn lock(&mut self) -> Result<()> {
        checked(unsafe { sto_tpcc_protocol_lock(&mut *self.worker.raw) })
    }
    pub fn timestamp(&mut self) -> Result<u32> {
        let mut timestamp = 0;
        checked(unsafe { sto_tpcc_protocol_timestamp(&mut *self.worker.raw, &mut timestamp) })?;
        Ok(timestamp)
    }
    pub fn validate(&mut self) -> Result<()> {
        checked(unsafe { sto_tpcc_protocol_validate(&mut *self.worker.raw) })
    }
    pub fn install(&mut self, timestamp: u32) -> Result<()> {
        checked(unsafe { sto_tpcc_protocol_install(&mut *self.worker.raw, timestamp) })
    }

    pub fn flush_remote(&mut self, callbacks: &mut dyn ProtocolCallbacks) -> Result<()> {
        let mut context = CallbackContext {
            callbacks,
            failure: None,
        };
        let code = unsafe {
            sto_tpcc_protocol_flush_remote(
                &mut *self.worker.raw,
                Some(dispatch),
                (&mut context as *mut CallbackContext<'_>).cast(),
            )
        };
        callback_result(code, context.failure)
    }

    pub fn submit_log(
        &mut self,
        latency: u32,
        callbacks: &mut dyn ProtocolCallbacks,
    ) -> Result<()> {
        let mut context = CallbackContext {
            callbacks,
            failure: None,
        };
        let code = unsafe {
            sto_tpcc_protocol_log(
                &mut *self.worker.raw,
                latency,
                Some(dispatch),
                (&mut context as *mut CallbackContext<'_>).cast(),
            )
        };
        callback_result(code, context.failure)
    }

    pub fn finish(mut self, committed: bool) -> Result<ParticipantReport> {
        let mut report = StoTpccProtocolReport::default();
        let code = unsafe {
            sto_tpcc_protocol_finish(&mut *self.worker.raw, u32::from(committed), &mut report)
        };
        self.release_finished();
        checked(code)?;
        Ok(ParticipantReport {
            disposition: report.disposition,
            phase: report.phase,
            time_term: report.time_term,
            read_dependency: report.read_dependency,
            empty: true,
        })
    }

    pub fn commit(
        mut self,
        latency: u32,
        callbacks: &mut dyn ProtocolCallbacks,
    ) -> Result<ParticipantReport> {
        let mut context = CallbackContext {
            callbacks,
            failure: None,
        };
        let mut report = StoTpccProtocolReport::default();
        let code = unsafe {
            sto_tpcc_protocol_commit(
                &mut *self.worker.raw,
                latency,
                Some(dispatch),
                (&mut context as *mut CallbackContext<'_>).cast(),
                &mut report,
            )
        };
        self.release_finished();
        callback_result(code, context.failure)?;
        Ok(ParticipantReport {
            disposition: report.disposition,
            phase: report.phase,
            time_term: report.time_term,
            read_dependency: report.read_dependency,
            empty: true,
        })
    }

    fn release_finished(&mut self) {
        if self.worker.raw.active.is_none() {
            self.worker.pinned.clear();
            self.finished = true;
        }
    }
}

/// Owns remote cleanup as well as the local participant. A transaction body
/// may return an error or unwind without leaving peers holding locks.
pub struct Coordinator<'worker, 'callbacks> {
    participant: Option<Participant<'worker>>,
    callbacks: &'callbacks mut dyn ProtocolCallbacks,
}
impl<'worker> std::ops::Deref for Coordinator<'worker, '_> {
    type Target = Participant<'worker>;
    fn deref(&self) -> &Self::Target {
        self.participant.as_ref().expect("live coordinator")
    }
}
impl std::ops::DerefMut for Coordinator<'_, '_> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.participant.as_mut().expect("live coordinator")
    }
}
impl Coordinator<'_, '_> {
    pub fn flush_remote(&mut self) -> Result<()> {
        self.participant
            .as_mut()
            .expect("live coordinator")
            .flush_remote(self.callbacks)
    }
    pub fn commit(mut self, latency: u32) -> Result<ParticipantReport> {
        self.participant
            .take()
            .expect("live coordinator")
            .commit(latency, self.callbacks)
    }
    pub fn abort(mut self) -> Result<()> {
        self.participant
            .take()
            .expect("live coordinator")
            .abort(self.callbacks)
    }
}
impl Drop for Coordinator<'_, '_> {
    fn drop(&mut self) {
        if let Some(participant) = self.participant.take() {
            let _ = participant.abort(self.callbacks);
        }
    }
}

impl Drop for Participant<'_> {
    fn drop(&mut self) {
        if !self.finished {
            let _ = self.worker.abort();
        }
    }
}

struct CallbackContext<'a> {
    callbacks: &'a mut dyn ProtocolCallbacks,
    failure: Option<Error>,
}

fn callback_result(code: i32, failure: Option<Error>) -> Result<()> {
    checked(code).map_err(|mut result| {
        if let Some(failure) = failure {
            result.message = failure.to_string();
        }
        result
    })
}

unsafe extern "C" fn dispatch(context: *mut c_void, request: *mut StoTpccProtocolRequest) -> i32 {
    // SAFETY: the synchronous coordinator call supplies these unique live
    // objects. Request spans are immutable for the callback's duration.
    let context = unsafe { &mut *context.cast::<CallbackContext<'_>>() };
    let request = unsafe { &mut *request };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let bytes = |data: *const u8, len: usize| -> &[u8] {
            if len == 0 {
                &[]
            } else {
                unsafe { slice::from_raw_parts(data, len) }
            }
        };
        match request.action {
            0 => {
                let mutation = match request.operation {
                    0 => Mutation::Put,
                    1 => Mutation::Insert,
                    2 => Mutation::Remove,
                    _ => return Err(invalid("invalid mutation callback")),
                };
                context.callbacks.stage(
                    request.shard,
                    request.table_id,
                    mutation,
                    bytes(request.key, request.key_length),
                    bytes(request.value, request.value_length),
                )
            }
            1 => {
                request.timestamp = context.callbacks.timestamp()?;
                Ok(())
            }
            2 => context.callbacks.validate(),
            3 => context.callbacks.install(request.timestamp),
            4 => context.callbacks.abort(),
            5 => context.callbacks.log(
                bytes(request.value, request.value_length),
                request.timestamp,
            ),
            _ => Err(invalid("invalid protocol callback action")),
        }
    }))
    .unwrap_or_else(|_| {
        Err(Error {
            kind: ErrorKind::Fatal,
            message: "protocol callback panicked".into(),
        })
    });
    match result {
        Ok(()) => 0,
        Err(failure) => {
            let code = match failure.kind {
                ErrorKind::Conflict => 3,
                ErrorKind::Capacity => 6,
                ErrorKind::UnknownOutcome => 7,
                ErrorKind::CommittedFailure => 8,
                _ => 5,
            };
            context.failure = Some(failure);
            code
        }
    }
}

unsafe extern "C" fn collect_versioned(
    context: *mut c_void,
    key: *const u8,
    key_len: usize,
    value: *const u8,
    value_len: usize,
    time_term: u32,
) -> i32 {
    // SAFETY: the synchronous scan gives valid immutable slices and the unique
    // Vec context supplied above. No native row reference escapes this call.
    let rows = unsafe { &mut *context.cast::<Vec<VersionedRow>>() };
    let key = if key_len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(key, key_len) }
    };
    let value = if value_len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(value, value_len) }
    };
    rows.push(VersionedRow {
        key: key.to_vec(),
        value: value.to_vec(),
        time_term,
    });
    0
}
