//! The only unsafe code in the safe Masstree wrapper.

#![allow(unsafe_code)]

use std::{ffi::c_void, mem, num::NonZeroUsize, ptr};

use crate::{
    status_result, Error, InsertError, InsertOutcome, KeyBound, NativeStatus,
    PublicationDisposition, RecordId, RuntimeConfig, RuntimeHealth, ScanChunk, ScanDirection,
    ScanEntry, ScanRequest, ScanResume, ScanStopReason,
};

pub(crate) struct AcquiredRuntime {
    pub raw: RuntimeHandle,
    pub max_threads: u32,
    pub max_key_length: usize,
    pub features: u64,
    pub build_id: mtree_sys::BuildId,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct RuntimeHandle(NonZeroUsize);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct ThreadHandle(NonZeroUsize);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct TreeHandle(NonZeroUsize);

impl RuntimeHandle {
    fn from_ptr(pointer: *mut mtree_sys::Runtime) -> Option<Self> {
        NonZeroUsize::new(pointer.expose_provenance()).map(Self)
    }

    fn as_mut_ptr(self) -> *mut mtree_sys::Runtime {
        ptr::with_exposed_provenance_mut(self.0.get())
    }

    fn as_const_ptr(self) -> *const mtree_sys::Runtime {
        ptr::with_exposed_provenance(self.0.get())
    }
}

impl ThreadHandle {
    fn from_ptr(pointer: *mut mtree_sys::Thread) -> Option<Self> {
        NonZeroUsize::new(pointer.expose_provenance()).map(Self)
    }

    fn as_mut_ptr(self) -> *mut mtree_sys::Thread {
        ptr::with_exposed_provenance_mut(self.0.get())
    }
}

impl TreeHandle {
    fn from_ptr(pointer: *mut mtree_sys::Tree) -> Option<Self> {
        NonZeroUsize::new(pointer.expose_provenance()).map(Self)
    }

    fn as_mut_ptr(self) -> *mut mtree_sys::Tree {
        ptr::with_exposed_provenance_mut(self.0.get())
    }
}

pub(crate) fn acquire(config: RuntimeConfig) -> Result<AcquiredRuntime, Error> {
    verify_static_abi()?;

    let mut native_config = mtree_sys::RuntimeConfig {
        struct_size: u32::MAX,
        abi_version: u32::MAX,
        required_features: u64::MAX,
        max_threads: u32::MAX,
        max_key_length: u32::MAX,
        reserved: [u64::MAX; 2],
    };
    // SAFETY: `native_config` is valid writable storage for the exact queried
    // POD layout, verified immediately above.
    status_result(unsafe { mtree_sys::mt_runtime_config_init(&mut native_config) })?;
    if native_config
        != (mtree_sys::RuntimeConfig {
            struct_size: mem::size_of::<mtree_sys::RuntimeConfig>() as u32,
            abi_version: mtree_sys::ABI_VERSION,
            required_features: 0,
            max_threads: 0,
            max_key_length: 0,
            reserved: [0; 2],
        })
    {
        return Err(Error::AbiMismatch("runtime config initialization differs"));
    }
    native_config.required_features = mtree_sys::REQUIRED_V1_FEATURES;
    native_config.max_threads = config.max_threads.unwrap_or(0);
    native_config.max_key_length = config.max_key_length.unwrap_or(0);

    let mut raw = ptr::null_mut();
    // SAFETY: both pointers reference live, correctly aligned ABI values. The
    // native runtime has process lifetime by its advertised feature contract.
    let acquire_status = unsafe { mtree_sys::mt_runtime_acquire(&native_config, &mut raw) };
    if let Some(status) = NativeStatus::from_raw(acquire_status) {
        if !raw.is_null() {
            return Err(Error::AbiMismatch(
                "failed runtime acquisition returned a handle",
            ));
        }
        return Err(Error::Native(status));
    }
    let raw = RuntimeHandle::from_ptr(raw).ok_or(Error::AbiMismatch(
        "runtime acquisition returned a null handle",
    ))?;

    let mut max_threads = 0;
    let mut max_key_length = 0;
    let mut build_id = mtree_sys::BuildId::default();
    // SAFETY: the acquired runtime handle is registered natively and outputs
    // point to live scalar/POD storage.
    unsafe {
        status_result(mtree_sys::mt_runtime_max_threads(
            raw.as_const_ptr(),
            &mut max_threads,
        ))?;
        status_result(mtree_sys::mt_runtime_max_key_length(
            raw.as_const_ptr(),
            &mut max_key_length,
        ))?;
        status_result(mtree_sys::mt_get_build_fingerprint(&mut build_id))?;
    }
    let native_max_threads = unsafe { mtree_sys::mt_max_threads() };
    let native_max_key_length = unsafe { mtree_sys::mt_max_key_length() };
    if max_threads == 0 || max_threads > native_max_threads {
        return Err(Error::AbiMismatch("runtime worker limit is invalid"));
    }
    if max_key_length == 0 || max_key_length > native_max_key_length {
        return Err(Error::AbiMismatch("runtime key limit is invalid"));
    }
    if build_id.low == 0 || build_id.high == 0 {
        return Err(Error::AbiMismatch("native build fingerprint is empty"));
    }
    if config
        .max_threads
        .is_some_and(|requested| requested != 0 && requested != max_threads)
    {
        return Err(Error::AbiMismatch("requested worker limit was not honored"));
    }
    if config
        .max_key_length
        .is_some_and(|requested| requested != 0 && usize::try_from(requested) != Ok(max_key_length))
    {
        return Err(Error::AbiMismatch("requested key limit was not honored"));
    }
    Ok(AcquiredRuntime {
        raw,
        max_threads,
        max_key_length,
        // SAFETY: this argument-free identity query cannot dereference memory.
        features: unsafe { mtree_sys::mt_feature_bits() },
        build_id,
    })
}

fn verify_static_abi() -> Result<(), Error> {
    // SAFETY: argument-free ABI identity queries have no pointer preconditions.
    unsafe {
        if mtree_sys::mt_abi_version() != mtree_sys::ABI_VERSION {
            return Err(Error::AbiMismatch("ABI version differs"));
        }
        let features = mtree_sys::mt_feature_bits();
        if features & mtree_sys::REQUIRED_V1_FEATURES != mtree_sys::REQUIRED_V1_FEATURES {
            return Err(Error::AbiMismatch("required feature bits are absent"));
        }
        if mtree_sys::mt_pointer_width() as usize != mem::size_of::<usize>() * 8 {
            return Err(Error::AbiMismatch("pointer width differs"));
        }
        let expected_endianness = if cfg!(target_endian = "little") {
            mtree_sys::BYTE_ORDER_LITTLE_ENDIAN
        } else {
            mtree_sys::BYTE_ORDER_BIG_ENDIAN
        };
        if mtree_sys::mt_endianness() != expected_endianness {
            return Err(Error::AbiMismatch("endianness differs"));
        }
        if mtree_sys::mt_runtime_config_size() != mem::size_of::<mtree_sys::RuntimeConfig>()
            || mtree_sys::mt_runtime_config_alignment()
                != mem::align_of::<mtree_sys::RuntimeConfig>()
            || mtree_sys::mt_build_id_size() != mem::size_of::<mtree_sys::BuildId>()
            || mtree_sys::mt_build_id_alignment() != mem::align_of::<mtree_sys::BuildId>()
            || mtree_sys::mt_get_or_insert_result_size()
                != mem::size_of::<mtree_sys::GetOrInsertResult>()
            || mtree_sys::mt_get_or_insert_result_alignment()
                != mem::align_of::<mtree_sys::GetOrInsertResult>()
            || mtree_sys::mt_scan_bound_size() != mem::size_of::<mtree_sys::ScanBound>()
            || mtree_sys::mt_scan_bound_alignment() != mem::align_of::<mtree_sys::ScanBound>()
            || mtree_sys::mt_scan_entry_size() != mem::size_of::<mtree_sys::ScanEntry>()
            || mtree_sys::mt_scan_entry_alignment() != mem::align_of::<mtree_sys::ScanEntry>()
            || mtree_sys::mt_scan_result_size() != mem::size_of::<mtree_sys::ScanResult>()
            || mtree_sys::mt_scan_result_alignment() != mem::align_of::<mtree_sys::ScanResult>()
        {
            return Err(Error::AbiMismatch("public POD layout differs"));
        }
        if mtree_sys::mt_exported_symbols_fingerprint() != mtree_sys::EXPORTED_SYMBOLS_FINGERPRINT {
            return Err(Error::AbiMismatch("exported symbol set differs"));
        }
        if mtree_sys::mt_max_key_length() != mtree_sys::CONFIGURED_MAX_KEY_LENGTH {
            return Err(Error::AbiMismatch("native maximum key length differs"));
        }
        if mtree_sys::mt_max_threads() == 0 {
            return Err(Error::AbiMismatch("native worker limit is zero"));
        }
        if mtree_sys::mt_record_id_limit() != u64::MAX {
            return Err(Error::AbiMismatch("RecordId scalar domain differs"));
        }
    }
    Ok(())
}

pub(crate) fn runtime_health(runtime: RuntimeHandle) -> Result<RuntimeHealth, Error> {
    let mut health = u32::MAX;
    // SAFETY: `runtime` originates from successful acquisition and native
    // runtime storage has process lifetime.
    let status = unsafe { mtree_sys::mt_runtime_health(runtime.as_const_ptr(), &mut health) };
    if let Some(status) = NativeStatus::from_raw(status) {
        if health != 0 {
            return Err(Error::AbiMismatch(
                "failed health query did not clear output",
            ));
        }
        return Err(Error::Native(status));
    }
    match health {
        mtree_sys::RUNTIME_HEALTHY => Ok(RuntimeHealth::Healthy),
        mtree_sys::RUNTIME_POISONED => Ok(RuntimeHealth::Poisoned),
        _ => Err(Error::AbiMismatch("unknown runtime health state")),
    }
}

pub(crate) fn thread_attach(runtime: RuntimeHandle) -> Result<ThreadHandle, Error> {
    let mut thread = ptr::null_mut();
    // SAFETY: `runtime` is a live acquired handle and `thread` is writable.
    let status = unsafe { mtree_sys::mt_thread_attach(runtime.as_mut_ptr(), &mut thread) };
    if let Some(status) = NativeStatus::from_raw(status) {
        if !thread.is_null() {
            return Err(Error::AbiMismatch(
                "failed thread attachment returned a handle",
            ));
        }
        return Err(Error::Native(status));
    }
    ThreadHandle::from_ptr(thread).ok_or(Error::AbiMismatch(
        "thread attachment returned a null handle",
    ))
}

pub(crate) fn thread_quiesce(thread: ThreadHandle) -> Result<(), Error> {
    // SAFETY: `thread` originates from successful same-thread attachment; the
    // safe Worker checks thread affinity before reaching this call.
    status_result(unsafe { mtree_sys::mt_thread_quiesce(thread.as_mut_ptr()) })
}

pub(crate) fn tree_create(
    runtime: RuntimeHandle,
    thread: ThreadHandle,
) -> Result<TreeHandle, Error> {
    let mut tree = ptr::null_mut();
    // SAFETY: both opaque handles were validated by their constructors and the
    // caller checked their shared runtime and thread affinity.
    let status =
        unsafe { mtree_sys::mt_tree_create(runtime.as_mut_ptr(), thread.as_mut_ptr(), &mut tree) };
    if let Some(status) = NativeStatus::from_raw(status) {
        if !tree.is_null() {
            return Err(Error::AbiMismatch("failed tree creation returned a handle"));
        }
        return Err(Error::Native(status));
    }
    TreeHandle::from_ptr(tree).ok_or(Error::AbiMismatch("tree creation returned a null handle"))
}

pub(crate) fn tree_release_best_effort(tree: TreeHandle) {
    // SAFETY: tree facade handles have process lifetime. Native release is
    // idempotence-checked and performs no thread-affine destruction in v1.
    let _ = unsafe { mtree_sys::mt_tree_release(tree.as_mut_ptr()) };
}

pub(crate) fn runtime_shutdown(runtime: RuntimeHandle, thread: ThreadHandle) -> Result<(), Error> {
    // SAFETY: same validated handles as other worker-scoped calls.
    status_result(unsafe {
        mtree_sys::mt_runtime_shutdown(runtime.as_mut_ptr(), thread.as_mut_ptr())
    })
}

pub(crate) fn get(tree: TreeHandle, thread: ThreadHandle, key: &[u8]) -> Result<u64, Error> {
    let mut result = u64::MAX;
    // SAFETY: the key slice remains live for the call and output points to a
    // writable scalar. Safe-side checks cover thread/runtime/key bounds.
    let status = unsafe {
        mtree_sys::mt_get(
            tree.as_mut_ptr(),
            thread.as_mut_ptr(),
            key.as_ptr().cast::<c_void>(),
            key.len(),
            &mut result,
        )
    };
    if let Some(status) = NativeStatus::from_raw(status) {
        if result != mtree_sys::RECORD_ID_NONE {
            return Err(Error::AbiMismatch("failed lookup did not clear output"));
        }
        return Err(Error::Native(status));
    }
    Ok(result)
}

pub(crate) fn get_or_insert(
    tree: TreeHandle,
    thread: ThreadHandle,
    key: &[u8],
    candidate: RecordId,
) -> Result<InsertOutcome, InsertError> {
    let mut result = mtree_sys::GetOrInsertResult {
        winner: u64::MAX,
        publication: u32::MAX,
        inserted: u8::MAX,
        reserved: [u8::MAX; 3],
    };
    // SAFETY: identical handle/key/output proof to `get`; candidate is
    // nonzero by construction.
    let raw_status = unsafe {
        mtree_sys::mt_get_or_insert(
            tree.as_mut_ptr(),
            thread.as_mut_ptr(),
            key.as_ptr().cast::<c_void>(),
            key.len(),
            candidate.get(),
            &mut result,
        )
    };
    if let Some(status) = NativeStatus::from_raw(raw_status) {
        let Ok((publication, winner)) = decode_failed_insert(result, candidate) else {
            return Err(InsertError {
                error: Error::InvalidPublication,
                publication: PublicationDisposition::Unknown,
                winner: None,
            });
        };
        return Err(InsertError {
            error: Error::Native(status),
            publication,
            winner,
        });
    }

    decode_successful_insert(result, candidate).map_err(|()| InsertError {
        error: Error::InvalidPublication,
        publication: PublicationDisposition::Unknown,
        winner: None,
    })
}

pub(crate) fn scan(
    tree: TreeHandle,
    thread: ThreadHandle,
    request: ScanRequest<'_>,
) -> Result<ScanChunk, Error> {
    let mut entries = Vec::new();
    entries
        .try_reserve_exact(request.entry_capacity())
        .map_err(|_| Error::AllocationLimit {
            requested: request.entry_capacity(),
        })?;
    entries.resize(request.entry_capacity(), mtree_sys::ScanEntry::default());

    let mut arena = Vec::new();
    arena
        .try_reserve_exact(request.key_arena_capacity())
        .map_err(|_| Error::AllocationLimit {
            requested: request.key_arena_capacity(),
        })?;
    arena.resize(request.key_arena_capacity(), 0_u8);

    let lower = encode_bound(request.lower());
    let upper = encode_bound(request.upper());
    let direction = match request.direction() {
        ScanDirection::Forward => mtree_sys::SCAN_FORWARD,
        ScanDirection::Reverse => mtree_sys::SCAN_REVERSE,
    };
    let initialized = initialized_scan_result();
    let mut result = mtree_sys::ScanResult {
        entries_written: usize::MAX,
        arena_bytes_used: usize::MAX,
        next_key_bytes_required: usize::MAX,
        stop_reason: u32::MAX,
        resume: u32::MAX,
        resume_key_offset: usize::MAX,
        resume_key_length: usize::MAX,
        reserved: [u64::MAX; 2],
    };
    // SAFETY: handles originate from successful native construction; encoded
    // bounds borrow live slices for this call; both vectors expose writable,
    // correctly aligned storage for their advertised capacities; `result` is
    // a live ABI POD output.
    let raw_status = unsafe {
        mtree_sys::mt_scan(
            tree.as_mut_ptr(),
            thread.as_mut_ptr(),
            direction,
            &lower,
            &upper,
            entries.as_mut_ptr(),
            entries.len(),
            arena.as_mut_ptr().cast::<c_void>(),
            arena.len(),
            &mut result,
        )
    };
    if let Some(status) = NativeStatus::from_raw(raw_status) {
        if result != initialized {
            return Err(Error::AbiMismatch(
                "failed scan did not initialize its result",
            ));
        }
        return Err(Error::Native(status));
    }
    decode_scan(entries, arena, result, request)
}

fn encode_bound(bound: KeyBound<'_>) -> mtree_sys::ScanBound {
    match bound {
        KeyBound::Unbounded => mtree_sys::ScanBound {
            key: ptr::null(),
            key_length: 0,
            kind: mtree_sys::SCAN_BOUND_ABSENT,
            reserved: 0,
        },
        KeyBound::Included(key) => mtree_sys::ScanBound {
            key: key.as_ptr().cast::<c_void>(),
            key_length: key.len(),
            kind: mtree_sys::SCAN_BOUND_INCLUSIVE,
            reserved: 0,
        },
        KeyBound::Excluded(key) => mtree_sys::ScanBound {
            key: key.as_ptr().cast::<c_void>(),
            key_length: key.len(),
            kind: mtree_sys::SCAN_BOUND_EXCLUSIVE,
            reserved: 0,
        },
    }
}

fn initialized_scan_result() -> mtree_sys::ScanResult {
    mtree_sys::ScanResult {
        entries_written: 0,
        arena_bytes_used: 0,
        next_key_bytes_required: 0,
        stop_reason: mtree_sys::SCAN_STOP_END,
        resume: mtree_sys::SCAN_RESUME_NONE,
        resume_key_offset: 0,
        resume_key_length: 0,
        reserved: [0; 2],
    }
}

fn decode_scan(
    raw_entries: Vec<mtree_sys::ScanEntry>,
    arena: Vec<u8>,
    result: mtree_sys::ScanResult,
    request: ScanRequest<'_>,
) -> Result<ScanChunk, Error> {
    if result.reserved != [0; 2]
        || result.entries_written > raw_entries.len()
        || result.arena_bytes_used > arena.len()
    {
        return Err(Error::AbiMismatch("scan result exceeds caller storage"));
    }

    let mut decoded = Vec::new();
    decoded
        .try_reserve_exact(result.entries_written)
        .map_err(|_| Error::AllocationLimit {
            requested: result.entries_written,
        })?;
    let mut expected_offset = 0_usize;
    let mut previous: Option<&[u8]> = None;
    for raw in raw_entries.iter().take(result.entries_written) {
        let end = raw
            .key_offset
            .checked_add(raw.key_length)
            .ok_or(Error::AbiMismatch("scan key offset overflowed"))?;
        if raw.key_offset != expected_offset || end > result.arena_bytes_used {
            return Err(Error::AbiMismatch(
                "scan key slice is not a packed arena prefix",
            ));
        }
        let key = &arena[raw.key_offset..end];
        let record_id = RecordId::new(raw.record_id)
            .ok_or(Error::AbiMismatch("scan returned the reserved RecordId"))?;
        if !key_in_bounds(key, request.lower(), request.upper()) {
            return Err(Error::AbiMismatch("scan returned a key outside its bounds"));
        }
        if previous.is_some_and(|prior| match request.direction() {
            ScanDirection::Forward => prior >= key,
            ScanDirection::Reverse => prior <= key,
        }) {
            return Err(Error::AbiMismatch("scan keys are not strictly ordered"));
        }

        let mut owned = Vec::new();
        owned
            .try_reserve_exact(key.len())
            .map_err(|_| Error::AllocationLimit {
                requested: key.len(),
            })?;
        owned.extend_from_slice(key);
        decoded.push(ScanEntry {
            key: owned.into_boxed_slice(),
            record_id,
        });
        previous = Some(key);
        expected_offset = end;
    }
    if expected_offset != result.arena_bytes_used {
        return Err(Error::AbiMismatch("scan arena contains unreferenced bytes"));
    }

    let (stop_reason, resume) = decode_scan_stop(&result, &raw_entries, &arena, request)?;
    Ok(ScanChunk {
        entries: decoded,
        stop_reason,
        resume,
        next_key_bytes_required: result.next_key_bytes_required,
    })
}

fn decode_scan_stop(
    result: &mtree_sys::ScanResult,
    raw_entries: &[mtree_sys::ScanEntry],
    arena: &[u8],
    request: ScanRequest<'_>,
) -> Result<(ScanStopReason, ScanResume), Error> {
    match (result.stop_reason, result.resume) {
        (mtree_sys::SCAN_STOP_END, mtree_sys::SCAN_RESUME_NONE)
            if result.next_key_bytes_required == 0
                && result.resume_key_offset == 0
                && result.resume_key_length == 0 =>
        {
            Ok((ScanStopReason::End, ScanResume::None))
        }
        (
            raw_stop @ (mtree_sys::SCAN_STOP_ENTRY_CAPACITY
            | mtree_sys::SCAN_STOP_KEY_ARENA_CAPACITY),
            mtree_sys::SCAN_RESUME_UNCHANGED_INPUT,
        ) if result.entries_written == 0
            && result.resume_key_offset == 0
            && result.resume_key_length == 0
            && capacity_stop_is_consistent(raw_stop, result, request) =>
        {
            Ok((scan_stop_reason(raw_stop), ScanResume::UnchangedInput))
        }
        (
            raw_stop @ (mtree_sys::SCAN_STOP_ENTRY_CAPACITY
            | mtree_sys::SCAN_STOP_KEY_ARENA_CAPACITY),
            mtree_sys::SCAN_RESUME_EXCLUSIVE_LAST,
        ) if result.entries_written != 0 => {
            if !capacity_stop_is_consistent(raw_stop, result, request) {
                return Err(Error::AbiMismatch(
                    "scan capacity stop contradicts caller storage",
                ));
            }
            let last = raw_entries
                .get(result.entries_written - 1)
                .ok_or(Error::AbiMismatch("scan resume entry is absent"))?;
            let end = last
                .key_offset
                .checked_add(last.key_length)
                .ok_or(Error::AbiMismatch("scan resume key overflowed"))?;
            if result.resume_key_offset != last.key_offset
                || result.resume_key_length != last.key_length
                || end > result.arena_bytes_used
            {
                return Err(Error::AbiMismatch("scan resume key is not the last entry"));
            }
            Ok((
                scan_stop_reason(raw_stop),
                ScanResume::Exclusive(arena[last.key_offset..end].into()),
            ))
        }
        _ => Err(Error::AbiMismatch("scan stop and resume metadata disagree")),
    }
}

fn capacity_stop_is_consistent(
    raw_stop: u32,
    result: &mtree_sys::ScanResult,
    request: ScanRequest<'_>,
) -> bool {
    match raw_stop {
        mtree_sys::SCAN_STOP_ENTRY_CAPACITY => result.entries_written == request.entry_capacity(),
        mtree_sys::SCAN_STOP_KEY_ARENA_CAPACITY => {
            result.entries_written < request.entry_capacity()
                && result.next_key_bytes_required
                    > request
                        .key_arena_capacity()
                        .saturating_sub(result.arena_bytes_used)
        }
        _ => false,
    }
}

fn scan_stop_reason(raw: u32) -> ScanStopReason {
    match raw {
        mtree_sys::SCAN_STOP_ENTRY_CAPACITY => ScanStopReason::EntryCapacity,
        mtree_sys::SCAN_STOP_KEY_ARENA_CAPACITY => ScanStopReason::KeyArenaCapacity,
        _ => unreachable!("caller matches only capacity stop reasons"),
    }
}

fn key_in_bounds(key: &[u8], lower: KeyBound<'_>, upper: KeyBound<'_>) -> bool {
    let above_lower = match lower {
        KeyBound::Unbounded => true,
        KeyBound::Included(bound) => key >= bound,
        KeyBound::Excluded(bound) => key > bound,
    };
    let below_upper = match upper {
        KeyBound::Unbounded => true,
        KeyBound::Included(bound) => key <= bound,
        KeyBound::Excluded(bound) => key < bound,
    };
    above_lower && below_upper
}

fn publication(raw: u32) -> Option<PublicationDisposition> {
    match raw {
        mtree_sys::PUBLICATION_FAILURE_BEFORE_PUBLICATION => {
            Some(PublicationDisposition::FailureBeforePublication)
        }
        mtree_sys::PUBLICATION_CANDIDATE_INSERTED => {
            Some(PublicationDisposition::CandidateInserted)
        }
        mtree_sys::PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED => {
            Some(PublicationDisposition::CandidateProvenUnpublished)
        }
        mtree_sys::PUBLICATION_UNKNOWN => Some(PublicationDisposition::Unknown),
        _ => None,
    }
}

fn decode_successful_insert(
    result: mtree_sys::GetOrInsertResult,
    candidate: RecordId,
) -> Result<InsertOutcome, ()> {
    if result.reserved != [0; 3] {
        return Err(());
    }
    let winner = RecordId::new(result.winner);
    match (publication(result.publication), result.inserted, winner) {
        (Some(PublicationDisposition::CandidateInserted), 1, Some(winner))
            if winner == candidate =>
        {
            Ok(InsertOutcome::Inserted(winner))
        }
        (Some(PublicationDisposition::CandidateProvenUnpublished), 0, Some(winner)) => {
            Ok(InsertOutcome::Existing(winner))
        }
        _ => Err(()),
    }
}

fn decode_failed_insert(
    result: mtree_sys::GetOrInsertResult,
    candidate: RecordId,
) -> Result<(PublicationDisposition, Option<RecordId>), ()> {
    if result.reserved != [0; 3] {
        return Err(());
    }
    let winner = RecordId::new(result.winner);
    let publication = publication(result.publication).ok_or(())?;
    match (publication, result.inserted, winner) {
        (PublicationDisposition::FailureBeforePublication, 0, None)
        | (PublicationDisposition::Unknown, 0, None) => Ok((publication, None)),
        (PublicationDisposition::CandidateInserted, 1, Some(winner)) if winner == candidate => {
            Ok((publication, Some(winner)))
        }
        (PublicationDisposition::CandidateProvenUnpublished, 0, winner) => {
            Ok((publication, winner))
        }
        _ => Err(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn candidate() -> RecordId {
        RecordId::new(41).unwrap()
    }

    fn result(
        winner: u64,
        publication: mtree_sys::PublicationDisposition,
        inserted: u8,
    ) -> mtree_sys::GetOrInsertResult {
        mtree_sys::GetOrInsertResult {
            winner,
            publication,
            inserted,
            reserved: [0; 3],
        }
    }

    fn scan_request(direction: ScanDirection) -> ScanRequest<'static> {
        ScanRequest::new(direction)
            .with_lower(KeyBound::Included(b""))
            .with_upper(KeyBound::Excluded(b"zz"))
            .with_entry_capacity(4)
            .with_key_arena_capacity(16)
    }

    fn scan_result(
        entries_written: usize,
        arena_bytes_used: usize,
        stop_reason: u32,
        resume: u32,
    ) -> mtree_sys::ScanResult {
        mtree_sys::ScanResult {
            entries_written,
            arena_bytes_used,
            next_key_bytes_required: 0,
            stop_reason,
            resume,
            resume_key_offset: 0,
            resume_key_length: 0,
            reserved: [0; 2],
        }
    }

    #[test]
    fn typed_opaque_handles_are_send_sync_without_unsafe_impls() {
        fn assert_send_sync<T: Send + Sync>() {}

        assert_send_sync::<RuntimeHandle>();
        assert_send_sync::<ThreadHandle>();
        assert_send_sync::<TreeHandle>();
    }

    #[test]
    fn successful_insert_results_require_an_exact_shape() {
        let candidate = candidate();
        assert_eq!(
            decode_successful_insert(
                result(
                    candidate.get(),
                    mtree_sys::PUBLICATION_CANDIDATE_INSERTED,
                    1,
                ),
                candidate,
            ),
            Ok(InsertOutcome::Inserted(candidate))
        );
        let winner = RecordId::new(73).unwrap();
        assert_eq!(
            decode_successful_insert(
                result(
                    winner.get(),
                    mtree_sys::PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED,
                    0,
                ),
                candidate,
            ),
            Ok(InsertOutcome::Existing(winner))
        );

        for malformed in [
            result(0, mtree_sys::PUBLICATION_FAILURE_BEFORE_PUBLICATION, 0),
            result(candidate.get(), mtree_sys::PUBLICATION_UNKNOWN, 0),
            result(
                candidate.get(),
                mtree_sys::PUBLICATION_CANDIDATE_INSERTED,
                0,
            ),
            result(
                candidate.get() + 1,
                mtree_sys::PUBLICATION_CANDIDATE_INSERTED,
                1,
            ),
        ] {
            assert_eq!(decode_successful_insert(malformed, candidate), Err(()));
        }
    }

    #[test]
    fn failed_insert_results_preserve_only_proven_consistent_metadata() {
        let candidate = candidate();
        assert_eq!(
            decode_failed_insert(
                result(0, mtree_sys::PUBLICATION_FAILURE_BEFORE_PUBLICATION, 0,),
                candidate,
            ),
            Ok((PublicationDisposition::FailureBeforePublication, None))
        );
        assert_eq!(
            decode_failed_insert(result(0, mtree_sys::PUBLICATION_UNKNOWN, 0), candidate,),
            Ok((PublicationDisposition::Unknown, None))
        );
        assert_eq!(
            decode_failed_insert(
                result(
                    candidate.get(),
                    mtree_sys::PUBLICATION_CANDIDATE_INSERTED,
                    1,
                ),
                candidate,
            ),
            Ok((PublicationDisposition::CandidateInserted, Some(candidate),))
        );
        assert_eq!(
            decode_failed_insert(
                result(0, mtree_sys::PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED, 0,),
                candidate,
            ),
            Ok((PublicationDisposition::CandidateProvenUnpublished, None,))
        );

        let mut bad_reserved = result(0, mtree_sys::PUBLICATION_UNKNOWN, 0);
        bad_reserved.reserved[1] = 1;
        for malformed in [
            bad_reserved,
            result(0, 99, 0),
            result(0, mtree_sys::PUBLICATION_UNKNOWN, 1),
            result(
                candidate.get() + 1,
                mtree_sys::PUBLICATION_CANDIDATE_INSERTED,
                1,
            ),
        ] {
            assert_eq!(decode_failed_insert(malformed, candidate), Err(()));
        }
    }

    #[test]
    fn scan_decoder_owns_packed_binary_keys_and_preserves_empty_keys() {
        let entries = vec![
            mtree_sys::ScanEntry {
                key_offset: 0,
                key_length: 0,
                record_id: 1,
            },
            mtree_sys::ScanEntry {
                key_offset: 0,
                key_length: 2,
                record_id: 2,
            },
            mtree_sys::ScanEntry {
                key_offset: 2,
                key_length: 1,
                record_id: 3,
            },
        ];
        let arena = vec![0, 0xff, b'z'];
        let result = scan_result(
            entries.len(),
            arena.len(),
            mtree_sys::SCAN_STOP_END,
            mtree_sys::SCAN_RESUME_NONE,
        );
        let chunk =
            decode_scan(entries, arena, result, scan_request(ScanDirection::Forward)).unwrap();

        assert_eq!(chunk.stop_reason(), ScanStopReason::End);
        assert_eq!(chunk.resume(), &ScanResume::None);
        assert_eq!(chunk.entries()[0].key(), b"");
        assert_eq!(chunk.entries()[1].key(), &[0, 0xff]);
        assert_eq!(chunk.entries()[2].key(), b"z");
        assert_eq!(chunk.entries()[2].record_id().get(), 3);
    }

    #[test]
    fn scan_decoder_requires_exclusive_last_resume_after_progress() {
        let entries = vec![
            mtree_sys::ScanEntry {
                key_offset: 0,
                key_length: 1,
                record_id: 4,
            },
            mtree_sys::ScanEntry {
                key_offset: 1,
                key_length: 2,
                record_id: 5,
            },
        ];
        let arena = b"abb".to_vec();
        let mut result = scan_result(
            2,
            3,
            mtree_sys::SCAN_STOP_ENTRY_CAPACITY,
            mtree_sys::SCAN_RESUME_EXCLUSIVE_LAST,
        );
        result.next_key_bytes_required = 7;
        result.resume_key_offset = 1;
        result.resume_key_length = 2;
        let request = scan_request(ScanDirection::Forward).with_entry_capacity(2);
        let chunk = decode_scan(entries, arena, result, request).unwrap();

        assert_eq!(chunk.stop_reason(), ScanStopReason::EntryCapacity);
        assert_eq!(
            chunk.resume(),
            &ScanResume::Exclusive(Box::from(&b"bb"[..]))
        );
        assert_eq!(chunk.next_key_bytes_required(), 7);
    }

    #[test]
    fn scan_decoder_rejects_invalid_ids_order_bounds_offsets_and_resume() {
        fn entry(offset: usize, key_length: usize, record_id: u64) -> mtree_sys::ScanEntry {
            mtree_sys::ScanEntry {
                key_offset: offset,
                key_length,
                record_id,
            }
        }

        let end = scan_result(1, 1, mtree_sys::SCAN_STOP_END, mtree_sys::SCAN_RESUME_NONE);
        assert!(matches!(
            decode_scan(
                vec![entry(0, 1, 0)],
                b"a".to_vec(),
                end,
                scan_request(ScanDirection::Forward)
            ),
            Err(Error::AbiMismatch(_))
        ));
        let two = scan_result(2, 2, mtree_sys::SCAN_STOP_END, mtree_sys::SCAN_RESUME_NONE);
        assert!(matches!(
            decode_scan(
                vec![entry(0, 1, 1), entry(1, 1, 2)],
                b"aa".to_vec(),
                two,
                scan_request(ScanDirection::Forward)
            ),
            Err(Error::AbiMismatch(_))
        ));
        assert!(matches!(
            decode_scan(
                vec![entry(1, 1, 1)],
                b"a".to_vec(),
                end,
                scan_request(ScanDirection::Forward)
            ),
            Err(Error::AbiMismatch(_))
        ));
        assert!(matches!(
            decode_scan(
                vec![entry(0, 1, 1)],
                b"z".to_vec(),
                end,
                scan_request(ScanDirection::Forward).with_upper(KeyBound::Excluded(b"z"))
            ),
            Err(Error::AbiMismatch(_))
        ));

        let mut bad_resume = scan_result(
            1,
            1,
            mtree_sys::SCAN_STOP_KEY_ARENA_CAPACITY,
            mtree_sys::SCAN_RESUME_EXCLUSIVE_LAST,
        );
        bad_resume.resume_key_offset = 1;
        bad_resume.resume_key_length = 1;
        assert!(matches!(
            decode_scan(
                vec![entry(0, 1, 1)],
                b"a".to_vec(),
                bad_resume,
                scan_request(ScanDirection::Forward)
            ),
            Err(Error::AbiMismatch(_))
        ));
    }
}
