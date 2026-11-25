//! Deterministic in-memory specification adapter for protocol/conformance tests.
//! Coarse table predicates deliberately permit extra conflicts. This is not a
//! production OCC implementation or a durable storage backend.
use super::*;
use std::{collections::BTreeMap, sync::Mutex};

type Key = (u64, Vec<u8>);
#[derive(Clone, Default)]
struct Cell {
    value: Option<Vec<u8>>,
    time_term: u32,
    generation: u64,
}
#[derive(Default)]
struct Tree {
    rows: BTreeMap<Vec<u8>, Cell>,
    generation: u64,
}
#[derive(Default)]
struct State {
    tables: BTreeMap<u64, Tree>,
    locks: BTreeMap<Key, u64>,
    ranges: BTreeMap<u64, u64>,
    next_worker: u64,
    clock: u32,
}
#[derive(Clone, Default)]
struct Model(Arc<Mutex<State>>);
pub fn database() -> Database {
    Database::new(Model::default())
}
impl Backend for Model {
    fn kind(&self) -> EngineKind {
        EngineKind::Model
    }
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn worker(&self) -> Result<Box<dyn WorkerBackend>> {
        let mut state = self.0.lock().unwrap();
        state.next_worker += 1;
        Ok(Box::new(ModelWorker {
            state: self.0.clone(),
            owner: state.next_worker,
            attempt: None,
        }))
    }
}
struct ModelTable {
    state: Arc<Mutex<State>>,
    id: u64,
}
impl TableBackend for ModelTable {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn len(&self) -> u64 {
        self.state.lock().unwrap().tables[&self.id]
            .rows
            .values()
            .filter(|c| c.value.is_some())
            .count() as u64
    }
}
#[derive(Default)]
struct Attempt {
    term: u32,
    dependency: u32,
    reads: BTreeMap<Key, u64>,
    scans: BTreeMap<u64, u64>,
    writes: BTreeMap<Key, (u16, Option<Vec<u8>>)>,
    validated: bool,
    installed: bool,
    doomed: bool,
}
struct ModelWorker {
    state: Arc<Mutex<State>>,
    owner: u64,
    attempt: Option<Attempt>,
}
fn conflict() -> Error {
    Error {
        kind: ErrorKind::Conflict,
        message: "model conflict".into(),
    }
}
impl ModelWorker {
    fn table(&self, table: &Table) -> Result<u64> {
        let table = table
            .as_any()
            .downcast_ref::<ModelTable>()
            .ok_or_else(|| Error::invalid("foreign table backend"))?;
        if !Arc::ptr_eq(&table.state, &self.state) {
            return Err(Error::invalid("foreign database table"));
        }
        Ok(table.id)
    }
    fn begin_attempt(&mut self, term: u32) -> Result<Guard<'_>> {
        if self.attempt.is_some() || term >= 10 {
            return Err(Error::invalid("busy worker or invalid term"));
        }
        self.attempt = Some(Attempt {
            term,
            ..Attempt::default()
        });
        Ok(Guard {
            worker: self,
            retained: false,
        })
    }
    fn release(&mut self) {
        self.attempt.take();
        let mut state = self.state.lock().unwrap();
        state.locks.retain(|_, owner| *owner != self.owner);
        state.ranges.retain(|_, owner| *owner != self.owner);
    }
}
impl Drop for ModelWorker {
    fn drop(&mut self) {
        self.release();
    }
}
impl WorkerBackend for ModelWorker {
    fn create_table(&mut self) -> Result<Arc<dyn TableBackend>> {
        if self.attempt.is_some() {
            return Err(Error::invalid("busy worker"));
        }
        let mut state = self.state.lock().unwrap();
        let id = state.tables.len() as u64 + 1;
        state.tables.insert(id, Tree::default());
        Ok(Arc::new(ModelTable {
            state: self.state.clone(),
            id,
        }))
    }
    fn begin(&mut self) -> Result<Box<dyn LocalTransaction + '_>> {
        Ok(Box::new(self.begin_attempt(0)?))
    }
    fn begin_participant(&mut self, term: u32) -> Result<Box<dyn ParticipantBackend + '_>> {
        Ok(Box::new(self.begin_attempt(term)?))
    }
    fn resume_participant(&mut self) -> Result<Box<dyn ParticipantBackend + '_>> {
        if self.attempt.is_none() {
            return Err(Error::invalid("no retained participant"));
        }
        Ok(Box::new(Guard {
            worker: self,
            retained: false,
        }))
    }
    fn replay(
        &mut self,
        table: &Table,
        key: &[u8],
        value: Option<&[u8]>,
        time_term: u32,
    ) -> Result<()> {
        let id = self.table(table)?;
        if self.attempt.is_some() {
            return Err(Error::invalid("replay on busy worker"));
        }
        let row = RedoRow {
            table: 1,
            key: key.to_vec(),
            value: value.map(Vec::from),
            time_term,
        };
        row.validate().map_err(|e| Error::invalid(e.to_string()))?;
        let mut state = self.state.lock().unwrap();
        if state.locks.contains_key(&(id, key.to_vec())) || state.ranges.contains_key(&id) {
            return Err(conflict());
        }
        state.clock = state.clock.max(time_term / 10);
        let tree = state.tables.get_mut(&id).unwrap();
        if tree
            .rows
            .get(key)
            .is_some_and(|c| !row.supersedes(c.time_term))
        {
            return Ok(());
        }
        let cell = tree.rows.entry(key.to_vec()).or_default();
        cell.value = row.value;
        cell.time_term = time_term;
        cell.generation += 1;
        tree.generation += 1;
        Ok(())
    }
    fn abort(&mut self) -> Result<()> {
        self.release();
        Ok(())
    }
}
struct Guard<'a> {
    worker: &'a mut ModelWorker,
    retained: bool,
}
impl Guard<'_> {
    fn attempt(&mut self) -> Result<&mut Attempt> {
        let a = self
            .worker
            .attempt
            .as_mut()
            .ok_or_else(|| Error::invalid("no participant"))?;
        if a.doomed {
            return Err(conflict());
        }
        Ok(a)
    }
    fn mutable(&mut self) -> Result<()> {
        if self.attempt()?.validated {
            return Err(Error::invalid("sealed participant"));
        }
        Ok(())
    }
    fn read(&mut self, table: &Table, key: &[u8]) -> Result<VersionedValue> {
        self.mutable()?;
        let id = self.worker.table(table)?;
        let state = self.worker.state.lock().unwrap();
        let owned = (id, key.to_vec());
        let a = self.worker.attempt.as_mut().unwrap();
        if state
            .locks
            .get(&owned)
            .is_some_and(|owner| *owner != self.worker.owner)
        {
            a.doomed = true;
            return Err(conflict());
        }
        let cell = state.tables[&id].rows.get(key).cloned().unwrap_or_default();
        if a.reads.get(&owned).is_some_and(|v| *v != cell.generation) {
            a.doomed = true;
            return Err(conflict());
        }
        a.reads.insert(owned.clone(), cell.generation);
        a.dependency = a.dependency.max(cell.time_term / 10);
        Ok(VersionedValue {
            value: a.writes.get(&owned).map_or(cell.value, |(_, v)| v.clone()),
            time_term: cell.time_term,
        })
    }
}
impl Drop for Guard<'_> {
    fn drop(&mut self) {
        if !self.retained {
            self.worker.release();
        }
    }
}
impl ParticipantBackend for Guard<'_> {
    fn get(&mut self, table: &Table, key: &[u8]) -> Result<VersionedValue> {
        self.read(table, key)
    }
    fn mutate(
        &mut self,
        table: &Table,
        table_id: u16,
        mutation: Mutation,
        key: &[u8],
        value: &[u8],
        lock_now: bool,
    ) -> Result<bool> {
        self.mutable()?;
        RedoRow {
            table: table_id,
            key: key.to_vec(),
            value: Some(value.to_vec()),
            time_term: 10,
        }
        .validate()
        .map_err(|e| Error::invalid(e.to_string()))?;
        let old = self.read(table, key)?;
        if (mutation == Mutation::Insert && old.value.is_some())
            || (mutation == Mutation::Remove && old.value.is_none())
        {
            return Ok(false);
        }
        let id = self.worker.table(table)?;
        self.attempt()?.writes.insert(
            (id, key.to_vec()),
            (
                table_id,
                (mutation != Mutation::Remove).then(|| value.to_vec()),
            ),
        );
        if lock_now {
            self.lock()?;
        }
        Ok(true)
    }
    fn scan(
        &mut self,
        table: &Table,
        lower: Bound<&[u8]>,
        upper: Bound<&[u8]>,
        reverse: bool,
        limit: usize,
    ) -> Result<VersionedScan> {
        self.mutable()?;
        let id = self.worker.table(table)?;
        let state = self.worker.state.lock().unwrap();
        let a = self.worker.attempt.as_mut().unwrap();
        if state
            .locks
            .iter()
            .any(|((t, _), owner)| *t == id && *owner != self.worker.owner)
        {
            a.doomed = true;
            return Err(conflict());
        }
        let tree = &state.tables[&id];
        if a.scans.get(&id).is_some_and(|v| *v != tree.generation) {
            a.doomed = true;
            return Err(conflict());
        }
        a.scans.insert(id, tree.generation);
        let in_range = |key: &[u8]| {
            (match lower {
                Bound::Included(lo) => key >= lo,
                Bound::Excluded(lo) => key > lo,
                Bound::Unbounded => true,
            }) && (match upper {
                Bound::Included(hi) => key <= hi,
                Bound::Excluded(hi) => key < hi,
                Bound::Unbounded => true,
            })
        };
        let mut rows = BTreeMap::new();
        for (key, cell) in &tree.rows {
            if in_range(key) {
                a.dependency = a.dependency.max(cell.time_term / 10);
                if let Some(value) = &cell.value {
                    rows.insert(key.clone(), (value.clone(), cell.time_term));
                }
            }
        }
        for ((t, key), (_, value)) in &a.writes {
            if *t == id && in_range(key) {
                if let Some(value) = value {
                    rows.insert(
                        key.clone(),
                        (value.clone(), tree.rows.get(key).map_or(0, |c| c.time_term)),
                    );
                } else {
                    rows.remove(key);
                }
            }
        }
        let mut rows: Vec<_> = rows
            .into_iter()
            .map(|(key, (value, time_term))| VersionedRow {
                key,
                value,
                time_term,
            })
            .collect();
        if reverse {
            rows.reverse();
        }
        rows.truncate(limit);
        Ok(VersionedScan {
            rows,
            read_dependency: a.dependency,
        })
    }
    fn lock(&mut self) -> Result<()> {
        self.mutable()?;
        let mut state = self.worker.state.lock().unwrap();
        let a = self.worker.attempt.as_mut().unwrap();
        let owner = self.worker.owner;
        if a.writes.keys().any(|k| {
            state.locks.get(k).is_some_and(|o| *o != owner)
                || state.ranges.get(&k.0).is_some_and(|o| *o != owner)
        }) || a.scans.keys().any(|t| {
            state.ranges.get(t).is_some_and(|o| *o != owner)
                || state
                    .locks
                    .iter()
                    .any(|((table, _), o)| table == t && *o != owner)
        }) {
            a.doomed = true;
            return Err(conflict());
        }
        for key in a.writes.keys() {
            state.locks.insert(key.clone(), owner);
        }
        for table in a.scans.keys() {
            state.ranges.insert(*table, owner);
        }
        Ok(())
    }
    fn timestamp(&mut self) -> Result<u32> {
        self.attempt()?;
        let mut state = self.worker.state.lock().unwrap();
        let a = self.worker.attempt.as_ref().unwrap();
        state.clock = state
            .clock
            .max(a.dependency)
            .checked_add(1)
            .ok_or_else(|| Error::invalid("timestamp exhausted"))?;
        Ok(state.clock)
    }
    fn validate(&mut self) -> Result<()> {
        self.lock()?;
        let state = self.worker.state.lock().unwrap();
        let a = self.worker.attempt.as_mut().unwrap();
        if a.reads.iter().any(|((t, k), v)| {
            state.tables[t].rows.get(k).map_or(0, |c| c.generation) != *v
                || state
                    .locks
                    .get(&(*t, k.clone()))
                    .is_some_and(|o| *o != self.worker.owner)
        }) || a
            .scans
            .iter()
            .any(|(t, v)| state.tables[t].generation != *v)
        {
            a.doomed = true;
            return Err(conflict());
        }
        a.validated = true;
        Ok(())
    }
    fn prepared_rows(&mut self, time_term: u32) -> Result<Vec<RedoRow>> {
        let a = self.attempt()?;
        if !a.validated || a.installed || time_term % 10 != a.term || time_term / 10 <= a.dependency
        {
            return Err(Error::invalid("invalid prepare"));
        }
        a.writes
            .iter()
            .map(|((_, key), (table, value))| {
                let r = RedoRow {
                    table: *table,
                    key: key.clone(),
                    value: value.clone(),
                    time_term,
                };
                r.validate().map_err(|e| Error::invalid(e.to_string()))?;
                Ok(r)
            })
            .collect()
    }
    fn install(&mut self, timestamp: u32) -> Result<()> {
        let a = self.attempt()?;
        if !a.validated || a.installed || timestamp <= a.dependency {
            return Err(Error::invalid(
                "install without validation or ordered timestamp",
            ));
        }
        let time_term = timestamp
            .checked_mul(10)
            .and_then(|t| t.checked_add(a.term))
            .ok_or_else(|| Error::invalid("timestamp exhausted"))?;
        let mut state = self.worker.state.lock().unwrap();
        let a = self.worker.attempt.as_mut().unwrap();
        for ((t, k), (_, value)) in &a.writes {
            let tree = state.tables.get_mut(t).unwrap();
            let cell = tree.rows.entry(k.clone()).or_default();
            cell.value = value.clone();
            cell.time_term = time_term;
            cell.generation += 1;
            tree.generation += 1;
        }
        state.clock = state.clock.max(timestamp);
        a.installed = true;
        Ok(())
    }
    fn finish(mut self: Box<Self>, committed: bool) -> Result<ParticipantReport> {
        let a = self.worker.attempt.as_ref().unwrap();
        if committed && !a.installed && (!a.validated || !a.writes.is_empty()) {
            return Err(Error::invalid("commit before install"));
        }
        let committed = committed || a.installed;
        self.worker.release();
        self.retained = true;
        Ok(ParticipantReport {
            disposition: if committed { 1 } else { 2 },
            empty: true,
            ..ParticipantReport::default()
        })
    }
    fn retain(mut self: Box<Self>) {
        self.retained = true;
    }
}
impl LocalTransaction for Guard<'_> {
    fn get(&mut self, t: &Table, k: &[u8]) -> Result<Option<Vec<u8>>> {
        Ok(self.read(t, k)?.value)
    }
    fn put(&mut self, t: &Table, k: &[u8], v: &[u8]) -> Result<()> {
        self.mutate(t, 1, Mutation::Put, k, v, false).map(|_| ())
    }
    fn insert(&mut self, t: &Table, k: &[u8], v: &[u8]) -> Result<bool> {
        self.mutate(t, 1, Mutation::Insert, k, v, false)
    }
    fn remove(&mut self, t: &Table, k: &[u8]) -> Result<bool> {
        self.mutate(t, 1, Mutation::Remove, k, &[], false)
    }
    fn scan(
        &mut self,
        t: &Table,
        lo: Bound<&[u8]>,
        hi: Bound<&[u8]>,
        rev: bool,
        limit: usize,
    ) -> Result<Vec<Row>> {
        Ok(ParticipantBackend::scan(self, t, lo, hi, rev, limit)?
            .rows
            .into_iter()
            .map(|r| (r.key, r.value))
            .collect())
    }
    fn commit(mut self: Box<Self>) -> Result<()> {
        self.lock()?;
        let ts = self.timestamp()?;
        self.validate()?;
        self.install(ts)?;
        self.finish(true).map(|_| ())
    }
    fn abort(self: Box<Self>) -> Result<()> {
        self.finish(false).map(|_| ())
    }
}
