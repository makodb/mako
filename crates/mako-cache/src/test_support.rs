//! Structural probes compiled only for the Phase 1F mutation gate.
//!
//! These helpers exercise the real write-back transitions while keeping test
//! instrumentation outside the production commit path.

use std::time::Duration;

use mako_local::MakoTimestamp;
use mrx_core::fakes::MemBlobs;

use crate::record::{BackendKey, CommitRecord, DEFAULT_TABLE_ID, Mutation, classify_backend_key};
use crate::writeback::{AppliedWatermark, Writeback, WritebackConfig};

/// Run the exact detached-permit bind transition between two callbacks.
///
/// The record, queue, and publication cell are prepared before `before` runs.
/// `after` runs immediately after [`crate::writeback::DetachedPermit::bind`]
/// returns and before checksum finalization or publication. An integration
/// test uses these callbacks to delimit a thread-local allocation audit.
pub fn probe_detached_bind(before: fn(), after: fn()) {
    let config = WritebackConfig {
        capacity: 1,
        max_apply_retries: 0,
        retry_delay: Duration::from_millis(1),
        ..WritebackConfig::default()
    };
    let writeback =
        Writeback::new_with_watermark(MemBlobs::new(), AppliedWatermark::default(), config)
            .expect("construct bind probe");
    let mut permit = writeback
        .reserve(vec![Mutation::Put {
            table_id: DEFAULT_TABLE_ID,
            key: b"allocation-probe".to_vec(),
            value: b"prepared-before-hook".to_vec(),
        }])
        .expect("prepare bind probe");

    before();
    let bound = permit.bind(MakoTimestamp::new(1).expect("nonzero probe timestamp"));
    after();

    bound
        .expect("bind probe must succeed")
        .publish()
        .expect("publish bind probe");
    assert_eq!(writeback.wait_applied().expect("apply bind probe"), 1);
}

/// Decode `(CacheSeq, MakoTimestamp)` pairs from an in-memory backend.
///
/// Results are sorted by cache sequence and every log row is validated through
/// the production decoder. This keeps the private backend namespace private
/// while allowing the hook-enabled integration gate to compare the native
/// timestamp callback with the exact persisted record.
pub fn decoded_log_timestamps(backend: &MemBlobs) -> Vec<(u64, u32)> {
    let mut decoded: Vec<_> = backend
        .snapshot()
        .into_iter()
        .filter_map(|(key, encoded)| match classify_backend_key(&key) {
            BackendKey::Log(_) => Some(
                CommitRecord::decode(&key, &encoded, crate::writeback::DEFAULT_MAX_RECORD_BYTES)
                    .expect("decode test backend commit record"),
            ),
            BackendKey::Data { .. } | BackendKey::Foreign => None,
        })
        .map(|record| (record.sequence().get(), record.mako_timestamp().get()))
        .collect();
    decoded.sort_unstable_by_key(|(sequence, _)| *sequence);
    decoded
}
