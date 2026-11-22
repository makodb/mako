//! Retained native participants and the scalar Mako coordinator.
//!
//! Network and log callbacks borrow bytes synchronously. All storage state,
//! final mutation ownership, decision selection and terminal outcomes live here.
use super::*;
use mako_clock::distributed::{
    global_distributed, DistributedTerm, DistributedTimeTerm, DistributedTimestamp,
};
use std::ffi::c_void;
use sto_core::{DefiniteOutcome, ParticipantError, ParticipantPhase};
use sto_masstree::ReadVisibility;

const LOCAL: u32 = u32::MAX;
const STAGE: u32 = 0;
const TIMESTAMP: u32 = 1;
const VALIDATE: u32 = 2;
const INSTALL: u32 = 3;
const ABORT: u32 = 4;
const LOG: u32 = 5;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct StoTpccProtocolReport {
    pub disposition: u32,
    pub phase: u32,
    pub time_term: u32,
    pub read_dependency: u32,
}

#[repr(C)]
pub struct StoTpccProtocolRequest {
    pub action: u32,
    pub shard: u32,
    pub table_id: u16,
    pub operation: u8,
    pub key: *const u8,
    pub key_length: usize,
    pub value: *const u8,
    pub value_length: usize,
    pub timestamp: u32,
}

impl StoTpccProtocolRequest {
    fn phase(action: u32, timestamp: u32) -> Self {
        Self {
            action,
            shard: LOCAL,
            table_id: 0,
            operation: 0,
            key: ptr::null(),
            key_length: 0,
            value: ptr::null(),
            value_length: 0,
            timestamp,
        }
    }
}

pub type StoTpccProtocolCallback =
    unsafe extern "C" fn(*mut c_void, *mut StoTpccProtocolRequest) -> i32;
pub type StoTpccVersionedScanCallback =
    unsafe extern "C" fn(*mut c_void, *const u8, usize, *const u8, usize, u32) -> i32;

struct FinalWrite {
    shard: u32,
    table_id: u16,
    operation: u8,
    key: Vec<u8>,
    value: Vec<u8>,
    sent: bool,
}

pub(super) struct ProtocolState {
    term: DistributedTerm,
    visibility: ReadVisibility,
    writes: Vec<FinalWrite>,
    touched: Vec<u32>,
    dependency: Option<DistributedTimestamp>,
    decision: Option<DistributedTimeTerm>,
    report: StoTpccProtocolReport,
    log: Vec<u8>,
    sizes_published: bool,
    log_submitted: bool,
}

impl Default for ProtocolState {
    fn default() -> Self {
        let term = DistributedTerm::new(0).expect("zero term is valid");
        Self {
            term,
            visibility: ReadVisibility::latest(term),
            writes: Vec::new(),
            touched: Vec::new(),
            dependency: None,
            decision: None,
            report: StoTpccProtocolReport::default(),
            log: Vec::new(),
            sizes_published: false,
            log_submitted: false,
        }
    }
}

impl ProtocolState {
    pub(super) fn is_uncertain(&self) -> bool {
        self.report.disposition == 3
    }
    fn reset(&mut self, term: DistributedTerm) {
        self.term = term;
        self.visibility = ReadVisibility::latest(term);
        self.writes.clear();
        self.touched.clear();
        self.dependency = None;
        self.decision = None;
        self.report = StoTpccProtocolReport {
            phase: 1,
            ..Default::default()
        };
        self.log.clear();
        self.sizes_published = false;
        self.log_submitted = false;
    }

    fn observe(&mut self, metadata: Option<DistributedTimeTerm>) {
        if let Some(metadata) = metadata {
            let base = metadata.timestamp();
            if self
                .dependency
                .is_none_or(|current| base.get() > current.get())
            {
                self.dependency = Some(base);
                self.report.read_dependency = base.get();
            }
        }
    }

    fn touch(&mut self, shard: u32) -> FfiResult<()> {
        if shard != LOCAL && !self.touched.contains(&shard) {
            self.touched
                .try_reserve(1)
                .map_err(|_| resource_exhausted(format_args!("participant set capacity")))?;
            self.touched.push(shard);
        }
        Ok(())
    }

    fn stage(
        &mut self,
        shard: u32,
        table_id: u16,
        operation: u8,
        key: &[u8],
        value: &[u8],
    ) -> FfiResult<()> {
        if operation > 2
            || table_id & 0x8000 != 0
            || key.len() > u16::MAX as usize
            || value.len() > u16::MAX as usize
        {
            return Err(fatal(format_args!(
                "mutation cannot be represented by legacy log/wire"
            )));
        }
        self.touch(shard)?;
        if let Some(write) = self
            .writes
            .iter_mut()
            .find(|write| write.shard == shard && write.table_id == table_id && write.key == key)
        {
            // Conditional INSERT never replaces an already staged live value.
            // Preserve both its payload and whether it was sent to the helper.
            if operation == 1 && write.operation != 2 {
                return Ok(());
            }
            write
                .value
                .try_reserve(value.len().saturating_sub(write.value.len()))
                .map_err(|_| resource_exhausted(format_args!("final write payload capacity")))?;
            write.value.clear();
            write.value.extend_from_slice(value);
            // PUT supersedes an earlier conditional INSERT. DELETE followed
            // by INSERT is an unconditional replacement until sent.
            write.operation = match (write.sent, write.operation, operation) {
                (false, 2, 1) => 0,
                (_, _, next) => next,
            };
            write.sent = false;
            return Ok(());
        }
        let mut owned_key = Vec::new();
        let mut owned_value = Vec::new();
        owned_key
            .try_reserve_exact(key.len())
            .map_err(|_| resource_exhausted(format_args!("final write key capacity")))?;
        owned_value
            .try_reserve_exact(value.len())
            .map_err(|_| resource_exhausted(format_args!("final write payload capacity")))?;
        self.writes
            .try_reserve(1)
            .map_err(|_| resource_exhausted(format_args!("final write ledger capacity")))?;
        owned_key.extend_from_slice(key);
        owned_value.extend_from_slice(value);
        self.writes.push(FinalWrite {
            shard,
            table_id,
            operation,
            key: owned_key,
            value: owned_value,
            sent: false,
        });
        Ok(())
    }

    fn prepare_log(&mut self, latency: u32) -> FfiResult<()> {
        let count = self
            .writes
            .iter()
            .filter(|write| write.shard == LOCAL)
            .count();
        if count == 0 {
            self.log.clear();
            return Ok(());
        }
        let count =
            u16::try_from(count).map_err(|_| fatal(format_args!("too many legacy log records")))?;
        let payload_size = self
            .writes
            .iter()
            .filter(|write| write.shard == LOCAL)
            .try_fold(0usize, |n, write| {
                n.checked_add(
                    6 + write.key.len()
                        + if write.operation == 2 {
                            1
                        } else {
                            write.value.len()
                        },
                )
            })
            .ok_or_else(|| fatal(format_args!("legacy log length overflow")))?;
        let payload_size_u32 = u32::try_from(payload_size)
            .map_err(|_| fatal(format_args!("legacy log exceeds u32 framing")))?;
        // Participants reserve complete framing before accepting validation;
        // INSTALL patches its chosen decision without allocation.
        let encoded = self.decision.map_or(0, DistributedTimeTerm::encode_legacy);
        self.log.clear();
        self.log
            .try_reserve(18 + payload_size)
            .map_err(|_| resource_exhausted(format_args!("legacy log capacity")))?;
        self.log.extend_from_slice(&encoded.to_ne_bytes());
        self.log.extend_from_slice(&count.to_ne_bytes());
        self.log.extend_from_slice(&payload_size_u32.to_ne_bytes());
        for write in self.writes.iter().filter(|write| write.shard == LOCAL) {
            self.log
                .extend_from_slice(&(write.key.len() as u16).to_ne_bytes());
            self.log.extend_from_slice(&write.key);
            let value = if write.operation == 2 {
                &[0u8][..]
            } else {
                &write.value
            };
            self.log
                .extend_from_slice(&(value.len() as u16).to_ne_bytes());
            self.log.extend_from_slice(value);
            let table_id = write.table_id | if write.operation == 2 { 0x8000 } else { 0 };
            self.log.extend_from_slice(&table_id.to_ne_bytes());
        }
        self.log.extend_from_slice(&encoded.to_ne_bytes());
        self.log.extend_from_slice(&latency.to_ne_bytes());
        Ok(())
    }
}

fn phase_error(error: ParticipantError) -> Status {
    match error {
        ParticipantError::Aborted(reason) => status_from_abort(reason),
        ParticipantError::Failure(error) => failure_status(error),
        ParticipantError::InvalidUse(error) => fatal(format_args!("participant phase: {error}")),
    }
}

pub(super) fn failure_status(error: CommitFailure) -> Status {
    set_last_error(format_args!("participant terminal: {error}"));
    match error {
        CommitFailure::Poisoned {
            outcome: DefiniteOutcome::Committed(_),
            ..
        } => Status::CommittedError,
        CommitFailure::Poisoned { .. } => Status::Fatal,
        CommitFailure::Indeterminate(_) => Status::Unknown,
    }
}

fn participant_access<R>(
    handle: &mut StoTpccThread,
    operation: impl for<'borrow> FnOnce(
        &mut Transaction<'borrow, Active>,
        &Worker,
    ) -> Result<R, AccessError>,
) -> FfiResult<R> {
    let worker = &handle.native_worker;
    handle
        .sto_worker
        .with_participant(|participant| {
            participant.with_transaction(|transaction| operation(transaction, worker))
        })
        .ok_or_else(|| fatal(format_args!("no participant active")))?
        .map_err(|error| status_from_access("participant operation", error))
}

fn require_phase(handle: &mut StoTpccThread, expected: ParticipantPhase) -> FfiResult<()> {
    if handle
        .sto_worker
        .with_participant(|participant| participant.phase())
        == Some(expected)
    {
        Ok(())
    } else {
        Err(fatal(format_args!(
            "participant is not in {expected:?} phase"
        )))
    }
}

fn lock(handle: &mut StoTpccThread) -> FfiResult<()> {
    handle
        .sto_worker
        .with_participant(|participant| participant.lock_writes())
        .ok_or_else(|| fatal(format_args!("no participant active")))?
        .map_err(phase_error)
}

fn validate(handle: &mut StoTpccThread) -> FfiResult<()> {
    require_phase(handle, ParticipantPhase::Collecting)?;
    handle.protocol.prepare_log(0)?;
    handle
        .sto_worker
        .with_participant(|participant| participant.validate())
        .ok_or_else(|| fatal(format_args!("no participant active")))?
        .map_err(phase_error)?;
    handle.protocol.report.phase = 2;
    Ok(())
}

fn install(handle: &mut StoTpccThread, decision: DistributedTimeTerm) -> FfiResult<()> {
    require_phase(handle, ParticipantPhase::Validated)?;
    global_distributed().observe(decision.timestamp());
    handle.protocol.decision = Some(decision);
    handle.protocol.report.time_term = decision.encode_legacy();
    if !handle.protocol.log.is_empty() {
        let encoded = decision.encode_legacy().to_ne_bytes();
        let footer = handle.protocol.log.len() - 8;
        handle.protocol.log[..4].copy_from_slice(&encoded);
        handle.protocol.log[footer..footer + 4].copy_from_slice(&encoded);
    }
    handle.protocol.report.disposition = 3;
    handle
        .sto_worker
        .with_participant(|participant| participant.install(decision))
        .ok_or_else(|| fatal(format_args!("no participant active")))?
        .map_err(|error| {
            let status = phase_error(error);
            if status == Status::CommittedError {
                status
            } else {
                Status::Unknown
            }
        })?;
    handle.protocol.report.phase = 3;
    handle.protocol.report.disposition = 2;
    if !handle.protocol.sizes_published {
        for pending in &handle.pending_size {
            adjust_logical_rows(&pending.table.logical_rows, pending.delta)
                .map_err(|_| Status::CommittedError)?;
        }
        handle.protocol.sizes_published = true;
    }
    Ok(())
}

pub(super) fn finish(handle: &mut StoTpccThread, committed: bool) -> FfiResult<Status> {
    if handle.active.is_some() && !handle.sto_worker.has_participant() {
        return Err(fatal(format_args!(
            "ordinary transaction requires ordinary terminal"
        )));
    }
    let uncertain = handle.protocol.report.disposition == 3;
    let Some(active) = handle.active.take() else {
        return Ok(if uncertain {
            Status::Unknown
        } else {
            Status::Ok
        });
    };
    let result = handle
        .sto_worker
        .finish_participant(|participant| participant.finish(committed))
        .ok_or_else(|| fatal(format_args!("native scope without participant")));
    let status = match result {
        Ok(Ok(CommitOutcome::Committed(_))) => {
            handle.protocol.report.disposition = 2;
            Status::Ok
        }
        Ok(Ok(CommitOutcome::Aborted(reason))) => {
            handle.protocol.report.disposition = 1;
            if committed {
                status_from_abort(reason)
            } else {
                Status::Ok
            }
        }
        Ok(Err(error)) => {
            let status = failure_status(error);
            handle.protocol.report.disposition = if status == Status::CommittedError {
                2
            } else if status == Status::Unknown {
                3
            } else {
                1
            };
            status
        }
        Err(status) => status,
    };
    handle.pending_size.clear();
    if uncertain {
        handle.protocol.report.disposition = 3;
    }
    if let Err(error) = active.rcu_scope.close() {
        set_last_error(format_args!("participant RCU close: {error}"));
        return Err(if uncertain {
            Status::Unknown
        } else if handle.protocol.report.disposition == 2 {
            Status::CommittedError
        } else {
            Status::Fatal
        });
    }
    handle.protocol.report.phase = 0;
    if uncertain {
        return Err(Status::Unknown);
    }
    Ok(status)
}

unsafe fn checked_handle<'a>(handle: *mut StoTpccThread) -> FfiResult<&'a mut StoTpccThread> {
    let handle = unsafe { required_mut(handle, "thread")? };
    handle.ensure_owner()?;
    Ok(handle)
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_begin(thread: *mut StoTpccThread, term: u32) -> i32 {
    boundary("protocol_begin", || {
        let handle = unsafe { checked_handle(thread)? };
        if handle.protocol.report.disposition == 3 {
            return Err(Status::Unknown);
        }
        let term = DistributedTerm::new(term)
            .map_err(|error| fatal(format_args!("invalid term: {error}")))?;
        txn_begin_impl(handle)?;
        if !handle.sto_worker.promote() {
            return Err(fatal(format_args!("participant promotion failed")));
        }
        handle.protocol.reset(term);
        Ok(Status::Ok)
    })
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_state(
    thread: *mut StoTpccThread,
    report: *mut StoTpccProtocolReport,
    empty: *mut u32,
) -> i32 {
    boundary("protocol_state", || {
        let handle = unsafe { checked_handle(thread)? };
        *unsafe { required_mut(report, "report")? } = handle.protocol.report;
        *unsafe { required_mut(empty, "empty")? } = handle
            .sto_worker
            .with_participant(|p| u32::from(p.is_empty()))
            .unwrap_or(1);
        Ok(Status::Ok)
    })
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_visibility(
    thread: *mut StoTpccThread,
    stable: *const u32,
    known: u32,
    failure: u32,
    failure_known: u32,
) -> i32 {
    boundary("protocol_visibility", || {
        let handle = unsafe { checked_handle(thread)? };
        require_phase(handle, ParticipantPhase::Collecting)?;
        let stable = unsafe { checked_slice(stable, 10, "stable watermarks")? };
        for (index, value) in stable.iter().enumerate() {
            handle.protocol.visibility.stable_through[index] =
                (known & (1 << index) != 0).then_some(*value);
        }
        handle.protocol.visibility.failed_shard_through = (failure_known != 0).then_some(failure);
        Ok(Status::Ok)
    })
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_observe_read(
    thread: *mut StoTpccThread,
    shard: u32,
    encoded: u32,
) -> i32 {
    boundary("protocol_observe_read", || {
        let handle = unsafe { checked_handle(thread)? };
        require_phase(handle, ParticipantPhase::Collecting)?;
        handle.protocol.touch(shard)?;
        let metadata = if encoded == 0 {
            None
        } else {
            Some(
                DistributedTimeTerm::decode_legacy(encoded)
                    .map_err(|error| fatal(format_args!("invalid read metadata: {error}")))?,
            )
        };
        handle.protocol.observe(metadata);
        Ok(Status::Ok)
    })
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_get(
    thread: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_len: usize,
    output: *mut u8,
    capacity: usize,
    actual: *mut usize,
    encoded: *mut u32,
) -> i32 {
    boundary("protocol_get", || {
        let actual = unsafe { required_mut(actual, "actual")? };
        *actual = 0;
        let encoded = unsafe { required_mut(encoded, "time_term")? };
        *encoded = 0;
        let handle = unsafe { checked_handle(thread)? };
        let table = unsafe { required_ref(table, "table")? };
        let key = unsafe { bytes(key, key_len, "key")? };
        let output = unsafe { output_bytes(output, capacity, "value")? };
        let visibility = handle.protocol.visibility;
        let read = participant_access(handle, |transaction, worker| {
            table
                .state
                .table
                .get_versioned(transaction, worker, key, visibility)
        })?;
        handle.protocol.observe(read.metadata);
        *encoded = read.metadata.map_or(0, DistributedTimeTerm::encode_legacy);
        let Some(value) = read.value else {
            return Ok(Status::Miss);
        };
        *actual = value.len();
        if value.len() > output.len() {
            return Ok(Status::BufferTooSmall);
        }
        output[..value.len()].copy_from_slice(value.as_ref());
        Ok(Status::Ok)
    })
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_scan(
    thread: *mut StoTpccThread,
    table: *const StoTpccTable,
    direction: i32,
    lower_kind: i32,
    lower: *const u8,
    lower_len: usize,
    upper_kind: i32,
    upper: *const u8,
    upper_len: usize,
    limit: usize,
    callback: Option<StoTpccVersionedScanCallback>,
    context: *mut c_void,
    visited: *mut usize,
) -> i32 {
    boundary("protocol_scan", || {
        let visited = unsafe { required_mut(visited, "visited")? };
        *visited = 0;
        let handle = unsafe { checked_handle(thread)? };
        let table = unsafe { required_ref(table, "table")? };
        let lower = unsafe { bytes(lower, lower_len, "lower")? };
        let upper = unsafe { bytes(upper, upper_len, "upper")? };
        let direction = match direction {
            0 => ScanDirection::Forward,
            1 => ScanDirection::Reverse,
            _ => return Err(fatal(format_args!("invalid scan direction"))),
        };
        let request = ScanRequest::new(direction, limit)
            .with_lower(scan_bound(lower_kind, lower, "lower")?)
            .with_upper(scan_bound(upper_kind, upper, "upper")?);
        let callback = callback.ok_or_else(|| fatal(format_args!("scan callback missing")))?;
        let visibility = handle.protocol.visibility;
        let scan = participant_access(handle, |transaction, worker| {
            table
                .state
                .table
                .scan_versioned(transaction, worker, request, visibility)
        })?;
        if let Some(dependency) = scan.dependency {
            if handle
                .protocol
                .dependency
                .is_none_or(|current| dependency.get() > current.get())
            {
                handle.protocol.dependency = Some(dependency);
                handle.protocol.report.read_dependency = dependency.get();
            }
        }
        for row in scan.rows {
            *visited += 1;
            // SAFETY: Callback borrows owned scan rows only during this call.
            if unsafe {
                callback(
                    context,
                    row.key.as_ptr(),
                    row.key.len(),
                    row.value.as_ptr(),
                    row.value.len(),
                    row.metadata.map_or(0, DistributedTimeTerm::encode_legacy),
                )
            } != 0
            {
                break;
            }
        }
        Ok(Status::Ok)
    })
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_mutate(
    thread: *mut StoTpccThread,
    table: *const StoTpccTable,
    table_id: u16,
    operation: u8,
    key: *const u8,
    key_len: usize,
    value: *const u8,
    value_len: usize,
    lock_now: u32,
) -> i32 {
    boundary("protocol_mutate", || {
        let handle = unsafe { checked_handle(thread)? };
        let table = unsafe { required_ref(table, "table")? };
        let key = unsafe { bytes(key, key_len, "key")? };
        let value = unsafe { bytes(value, value_len, "value")? };
        // Capture the selected payload and dependency together, before the
        // final-write ledger owns its copied mutation.
        let visibility = handle.protocol.visibility;
        let previous = participant_access(handle, |transaction, worker| {
            table
                .state
                .table
                .get_versioned(transaction, worker, key, visibility)
        })?;
        handle.protocol.observe(previous.metadata);
        let previous = previous.value;
        if operation == 1 && previous.is_some() {
            return Ok(Status::Duplicate);
        }
        if operation == 2 && previous.is_none() {
            return Ok(Status::Miss);
        }
        handle
            .protocol
            .stage(LOCAL, table_id, operation, key, value)?;
        let previous_present = participant_access(handle, |transaction, worker| match operation {
            0 | 1 => table
                .state
                .table
                .put_with_previous_presence(transaction, worker, key, value),
            2 => table
                .state
                .table
                .stage_tombstone_with_previous_presence(transaction, worker, key),
            _ => unreachable!("ledger validates operation"),
        })?;
        let delta = if operation == 2 {
            -i64::from(previous_present)
        } else {
            i64::from(!previous_present)
        };
        if delta != 0 {
            handle.record_size_delta(&table.state, delta)?;
        }
        if lock_now != 0 {
            lock(handle)?;
        }
        Ok(Status::Ok)
    })
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_stage_remote(
    thread: *mut StoTpccThread,
    shard: u32,
    table_id: u16,
    operation: u8,
    key: *const u8,
    key_len: usize,
    value: *const u8,
    value_len: usize,
) -> i32 {
    boundary("protocol_stage_remote", || {
        let handle = unsafe { checked_handle(thread)? };
        require_phase(handle, ParticipantPhase::Collecting)?;
        if shard == LOCAL {
            return Err(fatal(format_args!("remote shard is local sentinel")));
        }
        let key = unsafe { bytes(key, key_len, "key")? };
        let value = unsafe { bytes(value, value_len, "value")? };
        handle
            .protocol
            .stage(shard, table_id, operation, key, value)?;
        Ok(Status::Ok)
    })
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_lock(thread: *mut StoTpccThread) -> i32 {
    boundary("protocol_lock", || {
        lock(unsafe { checked_handle(thread)? })?;
        Ok(Status::Ok)
    })
}
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_validate(thread: *mut StoTpccThread) -> i32 {
    boundary("protocol_validate", || {
        validate(unsafe { checked_handle(thread)? })?;
        Ok(Status::Ok)
    })
}
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_install(thread: *mut StoTpccThread, base: u32) -> i32 {
    boundary("protocol_install", || {
        let handle = unsafe { checked_handle(thread)? };
        let base = DistributedTimestamp::new(base)
            .map_err(|error| fatal(format_args!("invalid decision: {error}")))?;
        install(handle, DistributedTimeTerm::new(base, handle.protocol.term))?;
        Ok(Status::Ok)
    })
}
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_finish(
    thread: *mut StoTpccThread,
    committed: u32,
    report: *mut StoTpccProtocolReport,
) -> i32 {
    boundary("protocol_finish", || {
        let report = unsafe { required_mut(report, "report")? };
        let handle = unsafe { checked_handle(thread)? };
        let result = finish(handle, committed != 0);
        *report = handle.protocol.report;
        result
    })
}

fn callback_status(raw: i32) -> Status {
    match raw {
        0 => Status::Ok,
        3 => Status::Retry,
        6 => Status::ResourceExhausted,
        7 => Status::Unknown,
        8 => Status::CommittedError,
        _ => Status::Fatal,
    }
}

fn invoke(
    callback: StoTpccProtocolCallback,
    context: *mut c_void,
    request: &mut StoTpccProtocolRequest,
) -> FfiResult<()> {
    // SAFETY: The C caller guarantees synchronous no-unwind callbacks; all
    // request bytes remain owned by the participant for the complete call.
    let status = callback_status(unsafe { callback(context, request) });
    if status == Status::Ok {
        Ok(())
    } else {
        Err(status)
    }
}

fn submit_log(
    handle: &mut StoTpccThread,
    latency: u32,
    callback: StoTpccProtocolCallback,
    context: *mut c_void,
) -> FfiResult<()> {
    if handle.protocol.is_uncertain() {
        return Err(Status::Unknown);
    }
    if handle.protocol.log_submitted {
        return Ok(());
    }
    if !handle.protocol.log.is_empty() {
        let footer = handle.protocol.log.len() - 4;
        handle.protocol.log[footer..].copy_from_slice(&latency.to_ne_bytes());
        let mut request = StoTpccProtocolRequest::phase(LOG, handle.protocol.report.time_term);
        request.value = handle.protocol.log.as_ptr();
        request.value_length = handle.protocol.log.len();
        if invoke(callback, context, &mut request).is_err() {
            handle.protocol.report.disposition = 3;
            return Err(Status::Unknown);
        }
    }
    handle.protocol.log_submitted = true;
    Ok(())
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_log(
    thread: *mut StoTpccThread,
    latency: u32,
    callback: Option<StoTpccProtocolCallback>,
    context: *mut c_void,
) -> i32 {
    boundary("protocol_log", || {
        let handle = unsafe { checked_handle(thread)? };
        let callback = callback.ok_or_else(|| fatal(format_args!("protocol callback missing")))?;
        if handle.protocol.report.phase != 3 {
            return Err(fatal(format_args!("log before install")));
        }
        submit_log(handle, latency, callback, context)?;
        Ok(Status::Ok)
    })
}

fn coordinate(
    handle: &mut StoTpccThread,
    latency: u32,
    callback: StoTpccProtocolCallback,
    context: *mut c_void,
) -> FfiResult<Status> {
    let prepared = (|| {
        flush_remote(handle, callback, context)?;
        lock(handle)?;
        let mut proposal = global_distributed()
            .reserve()
            .map_err(|error| fatal(format_args!("distributed timestamp: {error}")))?;
        if !handle.protocol.touched.is_empty() {
            let mut request = StoTpccProtocolRequest::phase(TIMESTAMP, 0);
            invoke(callback, context, &mut request)?;
            if request.timestamp > proposal.get() {
                proposal = DistributedTimestamp::new(request.timestamp)
                    .map_err(|error| fatal(format_args!("remote proposal: {error}")))?;
            }
        }
        if let Some(dependency) = handle.protocol.dependency {
            if proposal.get() <= dependency.get() {
                global_distributed().observe(proposal);
                proposal = global_distributed()
                    .reserve_after(dependency)
                    .map_err(|error| fatal(format_args!("read dependency timestamp: {error}")))?;
            }
        }
        let decision = DistributedTimeTerm::new(proposal, handle.protocol.term);
        handle.protocol.decision = Some(decision);
        handle.protocol.report.time_term = decision.encode_legacy();
        // All allocation and certification precede the irreversible boundary.
        validate(handle)?;
        if !handle.protocol.touched.is_empty() {
            invoke(
                callback,
                context,
                &mut StoTpccProtocolRequest::phase(VALIDATE, 0),
            )?;
        }
        Ok(decision)
    })();
    let decision = match prepared {
        Ok(decision) => decision,
        Err(status) => {
            let remote_cleanup = if handle.protocol.touched.is_empty() {
                Ok(())
            } else {
                invoke(
                    callback,
                    context,
                    &mut StoTpccProtocolRequest::phase(ABORT, 0),
                )
            };
            let cleanup = finish(handle, false)?;
            if cleanup != Status::Ok {
                return Ok(cleanup);
            }
            if remote_cleanup.is_err() {
                return Err(Status::Fatal);
            }
            return Ok(status);
        }
    };
    // From this point neither local nor remote failures can mean retry.
    if install(handle, decision).is_err() {
        handle.protocol.report.disposition = 3;
        return Err(Status::Unknown);
    }
    if !handle.protocol.touched.is_empty()
        && invoke(
            callback,
            context,
            &mut StoTpccProtocolRequest::phase(INSTALL, decision.timestamp().get()),
        )
        .is_err()
    {
        handle.protocol.report.disposition = 3;
        return Err(Status::Unknown);
    }
    if submit_log(handle, latency, callback, context).is_err() {
        handle.protocol.report.disposition = 3;
        return Err(Status::Unknown);
    }
    finish(handle, true)
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_commit(
    thread: *mut StoTpccThread,
    latency: u32,
    callback: Option<StoTpccProtocolCallback>,
    context: *mut c_void,
    report: *mut StoTpccProtocolReport,
) -> i32 {
    boundary("protocol_commit", || {
        let report = unsafe { required_mut(report, "report")? };
        let handle = unsafe { checked_handle(thread)? };
        let callback = callback.ok_or_else(|| fatal(format_args!("protocol callback missing")))?;
        let result = coordinate(handle, latency, callback, context);
        *report = handle.protocol.report;
        result
    })
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_distributed_timestamp(base: *mut u32) -> i32 {
    boundary("distributed_timestamp", || {
        let base = unsafe { required_mut(base, "base")? };
        *base = 0;
        *base = global_distributed()
            .reserve()
            .map_err(|error| fatal(format_args!("distributed timestamp: {error}")))?
            .get();
        Ok(Status::Ok)
    })
}
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_observe_timestamp(encoded: u32) -> i32 {
    boundary("observe_timestamp", || {
        let decision = DistributedTimeTerm::decode_legacy(encoded)
            .map_err(|error| fatal(format_args!("replay timestamp: {error}")))?;
        global_distributed().observe(decision.timestamp());
        Ok(Status::Ok)
    })
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_replay_apply(
    thread: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_len: usize,
    value: *const u8,
    value_len: usize,
    is_delete: u32,
    encoded: u32,
) -> i32 {
    boundary("replay_apply", || {
        let handle = unsafe { checked_handle(thread)? };
        let table = unsafe { required_ref(table, "table")? };
        let key = unsafe { bytes(key, key_len, "key")? };
        let value = unsafe { bytes(value, value_len, "value")? };
        let source = DistributedTimeTerm::decode_legacy(encoded)
            .map_err(|error| fatal(format_args!("replay source: {error}")))?;
        global_distributed().observe(source.timestamp());
        txn_begin_impl(handle)?;
        let worker = &handle.native_worker;
        let access =
            with_active_transaction!(&handle.active, &mut handle.sto_worker, |transaction| {
                let previous = table.state.table.get(transaction, worker, key)?;
                let changed = table.state.table.stage_replay(
                    transaction,
                    worker,
                    key,
                    if is_delete != 0 {
                        None
                    } else {
                        Some(Value::from(value))
                    },
                    source,
                )?;
                Ok::<_, AccessError>((changed, previous.is_some()))
            })?;
        match access {
            Ok((changed, previous_present)) => {
                if changed {
                    let delta = if is_delete != 0 {
                        -i64::from(previous_present)
                    } else {
                        i64::from(!previous_present)
                    };
                    if delta != 0 {
                        record_size_delta_after_staging(handle, &table.state, delta)?;
                    }
                }
                txn_commit_impl(handle)
            }
            Err(error) => {
                let status = status_from_access("replay", error);
                abort_active_attempt_after_fatal(handle)?;
                Ok(status)
            }
        }
    })
}

/// Participant proposal is strictly above every selected read, including
/// tombstones and rows traversed by a scan but not returned on the wire.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_timestamp(
    thread: *mut StoTpccThread,
    base: *mut u32,
) -> i32 {
    boundary("protocol_timestamp", || {
        let handle = unsafe { checked_handle(thread)? };
        let base = unsafe { required_mut(base, "base")? };
        *base = 0;
        let reserved = match handle.protocol.dependency {
            Some(dependency) => global_distributed().reserve_after(dependency),
            None => global_distributed().reserve(),
        };
        *base = reserved
            .map_err(|error| fatal(format_args!("participant proposal: {error}")))?
            .get();
        Ok(Status::Ok)
    })
}

fn flush_remote(
    handle: &mut StoTpccThread,
    callback: StoTpccProtocolCallback,
    context: *mut c_void,
) -> FfiResult<()> {
    require_phase(handle, ParticipantPhase::Collecting)?;
    for write in handle
        .protocol
        .writes
        .iter_mut()
        .filter(|write| write.shard != LOCAL && !write.sent)
    {
        let mut request = StoTpccProtocolRequest::phase(STAGE, 0);
        request.shard = write.shard;
        request.table_id = write.table_id;
        request.operation = write.operation;
        request.key = write.key.as_ptr();
        request.key_length = write.key.len();
        request.value = write.value.as_ptr();
        request.value_length = write.value.len();
        invoke(callback, context, &mut request)?;
        write.sent = true;
    }
    Ok(())
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_protocol_flush_remote(
    thread: *mut StoTpccThread,
    callback: Option<StoTpccProtocolCallback>,
    context: *mut c_void,
) -> i32 {
    boundary("protocol_flush_remote", || {
        let handle = unsafe { checked_handle(thread)? };
        let callback = callback.ok_or_else(|| fatal(format_args!("protocol callback missing")))?;
        flush_remote(handle, callback, context)?;
        Ok(Status::Ok)
    })
}

/// Advances pruning authority without changing any already selected snapshot.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_table_history_floor(
    table: *const StoTpccTable,
    base: u32,
) -> i32 {
    boundary("table_history_floor", || {
        let table = unsafe { required_ref(table, "table")? };
        if base != 0 {
            let floor = DistributedTimestamp::new(base)
                .map_err(|error| fatal(format_args!("history retention floor: {error}")))?;
            table.state.table.advance_history_retention_floor(floor);
        }
        Ok(Status::Ok)
    })
}
