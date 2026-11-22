#![cfg(mtree_native_integration)]

use std::{ffi::c_void, ptr, slice};
use sto_tpcc_ffi::*;

const OK: i32 = 0;
const UNKNOWN: i32 = 7;

fn native_error() -> String {
    let mut bytes = vec![0_u8; sto_tpcc_last_error_length() + 1];
    let mut actual = 0;
    let _ =
        unsafe { sto_tpcc_last_error_copy(bytes.as_mut_ptr().cast(), bytes.len(), &mut actual) };
    String::from_utf8_lossy(&bytes[..actual]).into_owned()
}

struct Native {
    db: *mut StoTpccDb,
    table: *mut StoTpccTable,
    replay_table: *mut StoTpccTable,
    thread: *mut StoTpccThread,
}

impl Native {
    fn new() -> Self {
        unsafe {
            let mut native = Self {
                db: ptr::null_mut(),
                table: ptr::null_mut(),
                replay_table: ptr::null_mut(),
                thread: ptr::null_mut(),
            };
            assert_eq!(sto_tpcc_db_create(ptr::null(), &mut native.db), OK);
            assert_eq!(
                sto_tpcc_table_create(native.db, ptr::null(), &mut native.table),
                OK
            );
            assert_eq!(
                sto_tpcc_table_create(native.db, ptr::null(), &mut native.replay_table),
                OK
            );
            assert_eq!(sto_tpcc_thread_create(native.db, &mut native.thread), OK);
            native
        }
    }

    fn begin(&self, term: u32) {
        assert_eq!(unsafe { sto_tpcc_protocol_begin(self.thread, term) }, OK);
    }

    fn put(&self, value: &[u8], lock_now: bool) {
        assert_eq!(
            unsafe {
                sto_tpcc_protocol_mutate(
                    self.thread,
                    self.table,
                    201,
                    0,
                    b"row".as_ptr(),
                    3,
                    value.as_ptr(),
                    value.len(),
                    u32::from(lock_now),
                )
            },
            OK
        );
    }

    fn read(&self) -> (Vec<u8>, u32) {
        let mut value = [0u8; 64];
        let mut actual = 0;
        let mut metadata = 0;
        assert_eq!(
            unsafe {
                sto_tpcc_protocol_get(
                    self.thread,
                    self.table,
                    b"row".as_ptr(),
                    3,
                    value.as_mut_ptr(),
                    value.len(),
                    &mut actual,
                    &mut metadata,
                )
            },
            OK
        );
        (value[..actual].to_vec(), metadata)
    }

    fn finish(&self, committed: bool) -> (i32, StoTpccProtocolReport) {
        let mut report = StoTpccProtocolReport::default();
        let status =
            unsafe { sto_tpcc_protocol_finish(self.thread, u32::from(committed), &mut report) };
        (status, report)
    }
}

impl Drop for Native {
    fn drop(&mut self) {
        unsafe {
            assert_eq!(sto_tpcc_thread_destroy(self.thread), OK);
            assert_eq!(sto_tpcc_table_destroy(self.table), OK);
            assert_eq!(sto_tpcc_table_destroy(self.replay_table), OK);
            assert_eq!(sto_tpcc_db_destroy(self.db), OK);
        }
    }
}

#[test]
fn retained_participant_repeated_locking_installs_copied_final_payload_and_metadata() {
    let native = Native::new();
    native.begin(3);
    let mut request = b"first".to_vec();
    native.put(&request, true);
    request.fill(b'x');
    assert_eq!(native.read().0, b"first");
    native.put(b"final", true);
    assert_eq!(native.read().0, b"final");
    assert_eq!(unsafe { sto_tpcc_protocol_validate(native.thread) }, OK);
    let mut base = 0;
    assert_eq!(
        unsafe { sto_tpcc_protocol_timestamp(native.thread, &mut base) },
        OK
    );
    assert_eq!(
        unsafe { sto_tpcc_protocol_install(native.thread, base) },
        OK
    );
    let (status, report) = native.finish(false);
    assert_eq!(status, OK);
    assert_eq!(
        report.disposition, 2,
        "abort after INSTALL must complete commit"
    );
    assert_eq!(report.time_term, base * 10 + 3);
    native.begin(3);
    assert_eq!(native.read(), (b"final".to_vec(), base * 10 + 3));
    assert_eq!(native.finish(false).0, OK);
}

#[derive(Default)]
struct Submission {
    bytes: Vec<u8>,
    fail_after_submission: bool,
}

unsafe extern "C" fn submit(context: *mut c_void, request: *mut StoTpccProtocolRequest) -> i32 {
    let context = unsafe { &mut *context.cast::<Submission>() };
    let request = unsafe { &*request };
    // These tests use a real local participant with the actual Rust coordinator;
    // only its legacy asynchronous log sink is replaced by a capturing sink.
    if request.action != 5 {
        return 5;
    }
    context.bytes = unsafe { slice::from_raw_parts(request.value, request.value_length) }.to_vec();
    if context.fail_after_submission {
        UNKNOWN
    } else {
        OK
    }
}

fn commit(native: &Native, submission: &mut Submission) -> (i32, StoTpccProtocolReport) {
    let mut report = StoTpccProtocolReport::default();
    let status = unsafe {
        sto_tpcc_protocol_commit(
            native.thread,
            12345,
            Some(submit),
            (submission as *mut Submission).cast(),
            &mut report,
        )
    };
    (status, report)
}

#[test]
fn coordinator_final_write_log_replays_original_decision_without_relogging() {
    let native = Native::new();
    native.begin(2);
    native.put(b"obsolete", false);
    native.put(b"latest", false);
    let mut submission = Submission::default();
    let (status, report) = commit(&native, &mut submission);
    assert_eq!(status, OK);
    assert_eq!(report.disposition, 2);
    let bytes = &submission.bytes;
    assert_eq!(
        u32::from_ne_bytes(bytes[..4].try_into().unwrap()),
        report.time_term
    );
    assert_eq!(u16::from_ne_bytes(bytes[4..6].try_into().unwrap()), 1);
    assert_eq!(u32::from_ne_bytes(bytes[6..10].try_into().unwrap()), 15);
    assert_eq!(u16::from_ne_bytes(bytes[10..12].try_into().unwrap()), 3);
    assert_eq!(&bytes[12..15], b"row");
    assert_eq!(u16::from_ne_bytes(bytes[15..17].try_into().unwrap()), 6);
    assert_eq!(&bytes[17..23], b"latest");
    assert_eq!(u16::from_ne_bytes(bytes[23..25].try_into().unwrap()), 201);
    assert_eq!(
        u32::from_ne_bytes(bytes[25..29].try_into().unwrap()),
        report.time_term
    );
    assert_eq!(u32::from_ne_bytes(bytes[29..33].try_into().unwrap()), 12345);
    let replay_table = native.replay_table;
    unsafe {
        for _ in 0..2 {
            assert_eq!(
                sto_tpcc_replay_apply(
                    native.thread,
                    replay_table,
                    b"row".as_ptr(),
                    3,
                    b"latest".as_ptr(),
                    6,
                    0,
                    report.time_term
                ),
                OK
            );
        }
        native.begin(2);
        let mut value = [0u8; 32];
        let mut actual = 0;
        let mut encoded = 0;
        assert_eq!(
            sto_tpcc_protocol_get(
                native.thread,
                replay_table,
                b"row".as_ptr(),
                3,
                value.as_mut_ptr(),
                value.len(),
                &mut actual,
                &mut encoded
            ),
            OK
        );
        assert_eq!(&value[..actual], b"latest");
        assert_eq!(encoded, report.time_term);
        assert_eq!(native.finish(false).0, OK);
    }
}

#[test]
fn coordinator_log_uncertainty_cannot_be_retried_or_aborted_after_publication() {
    let mut native = Native::new();
    native.begin(1);
    native.put(b"published", false);
    let mut submission = Submission {
        fail_after_submission: true,
        ..Default::default()
    };
    let (status, report) = commit(&native, &mut submission);
    assert_eq!(status, UNKNOWN);
    assert_eq!(report.disposition, 3);
    assert!(!submission.bytes.is_empty());
    assert_eq!(native.finish(false).0, UNKNOWN);
    assert_eq!(
        unsafe { sto_tpcc_protocol_begin(native.thread, 1) },
        UNKNOWN
    );
    unsafe {
        assert_eq!(sto_tpcc_thread_destroy(native.thread), OK);
        native.thread = ptr::null_mut();
        assert_eq!(sto_tpcc_thread_create(native.db, &mut native.thread), OK);
    }
    native.begin(1);
    assert_eq!(native.read(), (b"published".to_vec(), report.time_term));
    assert_eq!(native.finish(false).0, OK);
}

#[test]
fn historical_mutations_publish_tombstones_without_corrupting_physical_cardinality() {
    let native = Native::new();
    unsafe {
        assert_eq!(
            sto_tpcc_replay_apply(
                native.thread,
                native.table,
                b"row".as_ptr(),
                3,
                b"stable".as_ptr(),
                6,
                0,
                100
            ),
            OK
        );
        assert_eq!(
            sto_tpcc_replay_apply(
                native.thread,
                native.table,
                b"row".as_ptr(),
                3,
                ptr::null(),
                0,
                1,
                200
            ),
            OK
        );
    }
    native.begin(1);
    let mut stable = [0_u32; 10];
    stable[0] = 10;
    assert_eq!(
        unsafe { sto_tpcc_protocol_visibility(native.thread, stable.as_ptr(), 1, 0, 0) },
        OK
    );
    assert_eq!(native.read(), (b"stable".to_vec(), 100));
    assert_eq!(
        unsafe {
            sto_tpcc_protocol_mutate(
                native.thread,
                native.table,
                201,
                2,
                b"row".as_ptr(),
                3,
                ptr::null(),
                0,
                0,
            )
        },
        OK
    );
    let mut bytes = [0_u8; 32];
    let mut actual = 0;
    let mut metadata = 0;
    assert_eq!(
        unsafe {
            sto_tpcc_protocol_get(
                native.thread,
                native.table,
                b"row".as_ptr(),
                3,
                bytes.as_mut_ptr(),
                bytes.len(),
                &mut actual,
                &mut metadata,
            )
        },
        1,
        "an accepted historical DELETE must be visible to this transaction"
    );
    let mut submission = Submission::default();
    assert_eq!(commit(&native, &mut submission).0, OK);
    let mut rows = u64::MAX;
    assert_eq!(unsafe { sto_tpcc_table_size(native.table, &mut rows) }, OK);
    assert_eq!(rows, 0, "the physical head was already a tombstone");

    unsafe {
        assert_eq!(
            sto_tpcc_replay_apply(
                native.thread,
                native.replay_table,
                b"row".as_ptr(),
                3,
                ptr::null(),
                0,
                1,
                110
            ),
            OK
        );
        assert_eq!(
            sto_tpcc_replay_apply(
                native.thread,
                native.replay_table,
                b"row".as_ptr(),
                3,
                b"speculative".as_ptr(),
                11,
                0,
                210
            ),
            OK
        );
    }
    native.begin(1);
    stable[0] = 11;
    assert_eq!(
        unsafe { sto_tpcc_protocol_visibility(native.thread, stable.as_ptr(), 1, 0, 0) },
        OK
    );
    assert_eq!(
        unsafe {
            sto_tpcc_protocol_mutate(
                native.thread,
                native.replay_table,
                202,
                1,
                b"row".as_ptr(),
                3,
                b"replacement".as_ptr(),
                11,
                0,
            )
        },
        OK
    );
    assert_eq!(commit(&native, &mut submission).0, OK);
    assert_eq!(
        unsafe { sto_tpcc_table_size(native.replay_table, &mut rows) },
        OK
    );
    assert_eq!(
        rows, 1,
        "historical absence does not make a live physical row new"
    );
}

enum RemoteCall {
    Stage {
        table_id: u16,
        operation: u8,
        key: Vec<u8>,
        value: Vec<u8>,
    },
    Timestamp,
    Validate,
    Install(u32),
    Abort,
}

struct ParticipantEndpoint {
    requests: std::sync::mpsc::SyncSender<RemoteCall>,
    replies: std::sync::mpsc::Receiver<(i32, u32)>,
}

// Dispatches to a real participant on its owning native worker thread.
unsafe extern "C" fn participant_endpoint(
    context: *mut c_void,
    request: *mut StoTpccProtocolRequest,
) -> i32 {
    let endpoint = unsafe { &*context.cast::<ParticipantEndpoint>() };
    let request = unsafe { &mut *request };
    let call = match request.action {
        0 => RemoteCall::Stage {
            table_id: request.table_id,
            operation: request.operation,
            key: unsafe { std::slice::from_raw_parts(request.key, request.key_length) }.to_vec(),
            value: unsafe { std::slice::from_raw_parts(request.value, request.value_length) }
                .to_vec(),
        },
        1 => RemoteCall::Timestamp,
        2 => RemoteCall::Validate,
        3 => RemoteCall::Install(request.timestamp),
        4 => RemoteCall::Abort,
        // A remote-only coordinator must not log foreign rows.
        _ => return 5,
    };
    if endpoint.requests.send(call).is_err() {
        return 5;
    }
    match endpoint.replies.recv() {
        Ok((status, timestamp)) => {
            request.timestamp = timestamp;
            status
        }
        Err(_) => 5,
    }
}

#[test]
fn remote_unconditional_put_supersedes_an_unsent_duplicate_insert() {
    let coordinator = Native::new();
    std::thread::scope(|scope| {
        let (requests, incoming) = std::sync::mpsc::sync_channel(0);
        let (responses, replies) = std::sync::mpsc::sync_channel(0);
        let participant = scope.spawn(move || {
            let remote = Native::new();
            assert_eq!(
                unsafe {
                    sto_tpcc_replay_apply(
                        remote.thread,
                        remote.table,
                        b"row".as_ptr(),
                        3,
                        b"existing".as_ptr(),
                        8,
                        0,
                        10,
                    )
                },
                OK
            );
            remote.begin(0);
            while let Ok(call) = incoming.recv() {
                let mut timestamp = 0;
                let status = match call {
                    RemoteCall::Stage {
                        table_id,
                        operation,
                        key,
                        value,
                    } => {
                        let status = unsafe {
                            sto_tpcc_protocol_mutate(
                                remote.thread,
                                remote.table,
                                table_id,
                                operation,
                                key.as_ptr(),
                                key.len(),
                                value.as_ptr(),
                                value.len(),
                                0,
                            )
                        };
                        match status {
                            0..=2 => unsafe { sto_tpcc_protocol_lock(remote.thread) },
                            _ => status,
                        }
                    }
                    RemoteCall::Timestamp => unsafe {
                        sto_tpcc_protocol_timestamp(remote.thread, &mut timestamp)
                    },
                    RemoteCall::Validate => unsafe { sto_tpcc_protocol_validate(remote.thread) },
                    RemoteCall::Install(timestamp) => {
                        let status = unsafe { sto_tpcc_protocol_install(remote.thread, timestamp) };
                        if status == OK {
                            remote.finish(true).0
                        } else {
                            status
                        }
                    }
                    RemoteCall::Abort => remote.finish(false).0,
                };
                assert_eq!(status, OK, "participant: {}", native_error());
                responses.send((status, timestamp)).unwrap();
            }
            remote.begin(0);
            let observed = remote.read();
            assert_eq!(remote.finish(false).0, OK);
            observed
        });
        let mut endpoint = ParticipantEndpoint { requests, replies };
        coordinator.begin(0);
        for (operation, value) in [(1, b"conditional".as_slice()), (0, b"final".as_slice())] {
            assert_eq!(
                unsafe {
                    sto_tpcc_protocol_stage_remote(
                        coordinator.thread,
                        1,
                        201,
                        operation,
                        b"row".as_ptr(),
                        3,
                        value.as_ptr(),
                        value.len(),
                    )
                },
                OK
            );
        }
        let mut report = StoTpccProtocolReport::default();
        let status = unsafe {
            sto_tpcc_protocol_commit(
                coordinator.thread,
                0,
                Some(participant_endpoint),
                (&mut endpoint as *mut ParticipantEndpoint).cast(),
                &mut report,
            )
        };
        drop(endpoint);
        let observed = participant.join().unwrap();
        assert_eq!(status, OK, "coordinator: {}", native_error());
        assert_eq!(observed, (b"final".to_vec(), report.time_term));
    });
}
