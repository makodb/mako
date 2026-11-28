//! Bounded materialization of already committed effects. No transaction is
//! validated or decided here; the durable log remains authority for the gap.
use super::*;
use mako_storage_api::ApplicationBatch;
use sto_core::protocol::verified::recovery;

/// Limits for the optional Masstree layer's pending RocksDB writes.
#[derive(Clone, Copy, Debug)]
pub struct Options {
    pub records: usize,
    pub bytes: usize,
    pub rows: usize,
    pub interval: Duration,
}
impl Default for Options {
    fn default() -> Self {
        Self {
            records: 128,
            bytes: 8 * 1024 * 1024,
            rows: 50_000,
            interval: Duration::from_millis(100),
        }
    }
}
impl Options {
    pub(super) fn validate(self) -> Result<Self> {
        if self.records == 0
            || self.records > 4096
            || self.bytes == 0
            || self.bytes > 16 * 1024 * 1024
            || self.rows == 0
            || self.rows > 100_000
            || self.interval.is_zero()
        {
            return Err(storage("invalid RocksDB materialization limits"));
        }
        Ok(self)
    }
}
#[derive(Default)]
pub(super) struct Pending {
    records: Vec<ApplicationBatch>,
    bytes: usize,
    rows: usize,
}
impl Pending {
    pub fn full(&self, limits: Options) -> bool {
        self.records.len() >= limits.records
            || self.bytes >= limits.bytes
            || self.rows >= limits.rows
    }
    pub fn clear(&mut self) {
        *self = Self::default();
    }
}
impl State {
    pub(super) fn persist(
        &self,
        gate: &mut Gate,
        prior: crate::application::Position,
        change: &crate::application::Change,
    ) -> Result<()> {
        // Index has already checked the identity of an exact duplicate.
        if change.position.index <= prior.index {
            return Ok(());
        }
        let ids = change.table_ids();
        self.replay.tables(ids.clone())?;
        let application = &self.persistent;
        application.ensure_tables(&ids).map_err(engine)?;
        if !self.accelerated {
            return application
                .apply(
                    change.position.into(),
                    change.fingerprint(),
                    &change.rows,
                    &change.metadata()?,
                )
                .map_err(engine);
        }
        let record = ApplicationBatch {
            position: change.position.into(),
            fingerprint: change.fingerprint(),
            rows: change.rows.clone(),
            metadata: change.metadata()?,
        };
        let bytes = record.bytes();
        if !gate.materialization.records.is_empty()
            && (gate.materialization.bytes.saturating_add(bytes) > self.limits.bytes
                || gate.materialization.rows.saturating_add(record.rows.len()) > self.limits.rows)
        {
            self.flush_locked(gate, prior.into())?;
        }
        // A single legal log record can exceed the batching target; it is
        // flushed by itself immediately after publishing the application cut.
        gate.materialization.bytes = gate.materialization.bytes.saturating_add(bytes);
        gate.materialization.rows += record.rows.len();
        gate.materialization.records.push(record);
        Ok(())
    }
    pub(super) fn flush_locked(&self, gate: &mut Gate, applied: Position) -> Result<()> {
        Self::ensure(gate)?;
        if gate.materialization.records.is_empty() {
            return Ok(());
        }
        let result = (|| {
            let application = &self.persistent;
            let pending = &gate.materialization.records;
            let durable = application.position().map_err(engine)?;
            if !recovery::materialization_batch(
                durable.index,
                pending[0].position.index,
                pending.last().unwrap().position.index,
                applied.index,
            ) {
                return Err(storage("invalid RocksDB materialization cut"));
            }
            application.apply_batches(pending).map_err(engine)?;
            gate.materialization.clear();
            Ok(())
        })();
        if let Err(error) = &result {
            gate.fault = Some(error.to_string());
        }
        result
    }
    pub(super) fn flush(&self) -> Result<()> {
        let mut gate = self.gate.lock();
        let applied = self.position()?;
        self.flush_locked(&mut gate, applied)
    }
}
pub(super) struct Flusher {
    stop: Option<mpsc::Sender<()>>,
    thread: Option<JoinHandle<()>>,
}
impl Flusher {
    pub fn start(state: &Arc<State>) -> Result<Self> {
        let (stop, receive) = mpsc::channel();
        let interval = state.limits.interval;
        let weak = Arc::downgrade(state);
        let thread = std::thread::Builder::new()
            .name("mako-materialize".into())
            .spawn(move || {
                while matches!(
                    receive.recv_timeout(interval),
                    Err(mpsc::RecvTimeoutError::Timeout)
                ) {
                    let Some(state) = weak.upgrade() else { break };
                    if state.flush().is_err() {
                        break;
                    }
                }
            })
            .map_err(storage)?;
        Ok(Self {
            stop: Some(stop),
            thread: Some(thread),
        })
    }
}
impl Drop for Flusher {
    fn drop(&mut self) {
        if let Some(stop) = self.stop.take() {
            let _ = stop.send(());
        }
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}
