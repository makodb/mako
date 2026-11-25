//! The same semantic scenarios run against model and native engines. These
//! test the trusted storage contract separately from distributed protocol proofs.
use super::*;
use std::ops::Bound::Unbounded;

fn table(db: &Database) -> Table {
    Session::new(db).create_table(TableConfig).unwrap()
}
fn commit(mut p: SessionParticipant<'_>) -> u32 {
    p.lock().unwrap();
    let timestamp = p.timestamp().unwrap();
    p.validate().unwrap();
    p.install(timestamp).unwrap();
    p.finish(true).unwrap();
    timestamp * 10
}
fn read(db: &Database, t: &Table, key: &[u8]) -> VersionedValue {
    let mut w = Session::new(db);
    let mut p = w.begin_participant(0).unwrap();
    let v = p.get(t, key).unwrap();
    p.finish(false).unwrap();
    v
}

pub fn private_writes_and_ordered_scans(db: &Database) {
    let t = table(db);
    let mut w = Session::new(db);
    let mut p = w.begin_participant(0).unwrap();
    for k in [b"z".as_slice(), b"\0", b"a"] {
        assert!(p.mutate(&t, 1, Mutation::Insert, k, k, false).unwrap());
    }
    assert!(!p
        .mutate(&t, 1, Mutation::Insert, b"a", b"changed", false)
        .unwrap());
    assert_eq!(
        p.get(&t, b"a").unwrap().value.as_deref(),
        Some(b"a".as_slice())
    );
    let forward = p.scan(&t, Unbounded, Unbounded, false, 10).unwrap();
    assert_eq!(
        forward
            .rows
            .iter()
            .map(|r| r.key.as_slice())
            .collect::<Vec<_>>(),
        vec![b"\0".as_slice(), b"a", b"z"]
    );
    assert_eq!(
        p.scan(&t, Unbounded, Unbounded, true, 1).unwrap().rows[0].key,
        b"z"
    );
    p.finish(false).unwrap();
    assert!(read(db, &t, b"a").value.is_none());
}

pub fn absence_and_range_dependencies(db: &Database) {
    let t = table(db);
    let mut a = Session::new(db);
    let mut b = Session::new(db);
    let mut absent = a.begin_participant(0).unwrap();
    assert!(absent.get(&t, b"missing").unwrap().value.is_none());
    let mut writer = b.begin_participant(0).unwrap();
    writer
        .mutate(&t, 1, Mutation::Put, b"missing", b"v", false)
        .unwrap();
    commit(writer);
    assert!(absent.validate().is_err());
    absent.finish(false).unwrap();
    let mut scan = a.begin_participant(0).unwrap();
    scan.scan(&t, Unbounded, Unbounded, false, 10).unwrap();
    let mut writer = b.begin_participant(0).unwrap();
    writer
        .mutate(&t, 1, Mutation::Put, b"phantom", b"v", false)
        .unwrap();
    commit(writer);
    assert!(scan.validate().is_err());
    scan.finish(false).unwrap();
}

pub fn global_lock_barrier_rejects_write_skew(db: &Database) {
    let t = table(db);
    let mut a = Session::new(db);
    let mut b = Session::new(db);
    let mut first = a.begin_participant(0).unwrap();
    let mut second = b.begin_participant(0).unwrap();
    first.get(&t, b"x").unwrap();
    second.get(&t, b"y").unwrap();
    first
        .mutate(&t, 1, Mutation::Put, b"y", b"1", false)
        .unwrap();
    second
        .mutate(&t, 1, Mutation::Put, b"x", b"1", false)
        .unwrap();
    first.lock().unwrap();
    second.lock().unwrap();
    let one = first.validate();
    let two = second.validate();
    assert!(
        one.is_err() || two.is_err(),
        "write skew passed the global lock barrier"
    );
    first.finish(false).unwrap();
    second.finish(false).unwrap();
}

pub fn prepare_is_private_frozen_and_retained(db: &Database) {
    let t = table(db);
    let mut a = Session::new(db);
    let mut b = Session::new(db);
    let mut p = a.begin_participant(0).unwrap();
    p.mutate(&t, 1, Mutation::Put, b"key", b"value", false)
        .unwrap();
    p.lock().unwrap();
    let timestamp = p.timestamp().unwrap();
    p.validate().unwrap();
    let rows = p.prepared_rows(timestamp * 10).unwrap();
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0].value.as_deref(), Some(b"value".as_slice()));
    p.retain();
    let mut other = b.begin_participant(0).unwrap();
    match other.get(&t, b"key") {
        Ok(v) => assert!(v.value.is_none(), "Prepare published data"),
        Err(e) => assert_eq!(e.kind, ErrorKind::Conflict),
    }
    other.finish(false).unwrap();
    let mut p = a.resume_participant().unwrap();
    assert_eq!(p.prepared_rows(timestamp * 10).unwrap(), rows);
    p.install(timestamp).unwrap();
    p.finish(true).unwrap();
    assert_eq!(
        read(db, &t, b"key").value.as_deref(),
        Some(b"value".as_slice())
    );
}

pub fn replay_preserves_tombstones_and_versions(db: &Database) {
    let t = table(db);
    let mut w = Session::new(db);
    w.replay(&t, b"key", Some(b"old"), 100).unwrap();
    w.replay(&t, b"key", None, 200).unwrap();
    for _ in 0..3 {
        w.replay(&t, b"key", Some(b"old"), 100).unwrap();
        w.replay(&t, b"key", None, 200).unwrap();
    }
    let absent = read(db, &t, b"key");
    assert_eq!(absent.value, None);
    assert_eq!(absent.time_term, 200);
    w.replay(&t, b"key", Some(b"new"), 300).unwrap();
    assert_eq!(
        read(db, &t, b"key").value.as_deref(),
        Some(b"new".as_slice())
    );
}

pub fn run(db: &Database) {
    private_writes_and_ordered_scans(db);
    absence_and_range_dependencies(db);
    global_lock_barrier_rejects_write_skew(db);
    prepare_is_private_frozen_and_retained(db);
    replay_preserves_tombstones_and_versions(db);
    checkpoint_keeps_committed_state_behind_prepare(db);
}

pub fn checkpoint_keeps_committed_state_behind_prepare(db: &Database) {
    let t = table(db);
    let mut exporter = Session::new(db);
    exporter
        .replay(&t, b"key", Some(b"committed"), 100)
        .unwrap();
    exporter.replay(&t, b"deleted", None, 200).unwrap();
    let mut owner = Session::new(db);
    let mut prepared = owner.begin_participant(0).unwrap();
    prepared
        .mutate(&t, 1, Mutation::Put, b"key", b"private", false)
        .unwrap();
    prepared
        .mutate(&t, 1, Mutation::Put, b"new", b"private", false)
        .unwrap();
    prepared.lock().unwrap();
    let timestamp = prepared.timestamp().unwrap();
    prepared.validate().unwrap();
    prepared.prepared_rows(timestamp * 10).unwrap();
    prepared.retain();
    // No installation occurs while this cut is exported. Prepared protection
    // stays live; snapshot traversal must neither wait for it nor expose intent.
    let mut rows = Vec::new();
    let mut after = None;
    loop {
        let t = t.clone();
        let cursor = after.clone();
        let page = exporter
            .call(move |w| w.checkpoint_page(&t, cursor.as_deref(), 1))
            .unwrap();
        if let Some(last) = page.rows.last() {
            assert!(after.as_ref().is_none_or(|a| a < &last.key));
            after = Some(last.key.clone());
        }
        assert!(page.end || !page.rows.is_empty());
        rows.extend(page.rows);
        if page.end {
            break;
        }
    }
    let deleted = rows.iter().find(|r| r.key == b"deleted").unwrap();
    assert_eq!((deleted.value.as_ref(), deleted.time_term), (None, 200));
    let key = rows.iter().find(|r| r.key == b"key").unwrap();
    assert_eq!(
        (key.value.as_deref(), key.time_term),
        (Some(b"committed".as_slice()), 100)
    );
    assert!(rows
        .iter()
        .all(|r| r.value.as_deref() != Some(b"private".as_slice())));
    owner.resume_participant().unwrap().finish(false).unwrap();
}

// Each worker is created and destroyed on its own OS thread. Masstree permits
// only one worker attachment per thread, including across database handles.
// Commands carry owned values; transaction borrows never cross a channel.
type Work = Box<dyn FnOnce(&mut Worker) + Send>;
struct Session {
    send: Option<std::sync::mpsc::Sender<Work>>,
    thread: Option<std::thread::JoinHandle<()>>,
}
impl Session {
    fn new(db: &Database) -> Self {
        let db = db.clone();
        let (send, recv) = std::sync::mpsc::channel::<Work>();
        let thread = std::thread::spawn(move || {
            let mut worker = db.worker().unwrap();
            for work in recv {
                work(&mut worker);
            }
            let _ = worker.abort();
        });
        Self {
            send: Some(send),
            thread: Some(thread),
        }
    }
    fn call<R: Send + 'static>(&self, f: impl FnOnce(&mut Worker) -> R + Send + 'static) -> R {
        let (send, recv) = std::sync::mpsc::sync_channel(1);
        self.send
            .as_ref()
            .unwrap()
            .send(Box::new(move |worker| {
                let _ = send.send(f(worker));
            }))
            .unwrap();
        recv.recv().unwrap()
    }
    fn create_table(&mut self, config: TableConfig) -> Result<Table> {
        self.call(move |w| w.create_table(config))
    }
    fn begin_participant(&mut self, term: u32) -> Result<SessionParticipant<'_>> {
        self.call(move |w| {
            w.begin_participant(term)?.retain();
            Ok(())
        })?;
        Ok(SessionParticipant {
            session: self,
            done: false,
        })
    }
    fn resume_participant(&mut self) -> Result<SessionParticipant<'_>> {
        Ok(SessionParticipant {
            session: self,
            done: false,
        })
    }
    fn replay(&mut self, t: &Table, k: &[u8], v: Option<&[u8]>, tt: u32) -> Result<()> {
        let (t, k, v) = (t.clone(), k.to_vec(), v.map(Vec::from));
        self.call(move |w| w.replay(&t, &k, v.as_deref(), tt))
    }
}
impl Drop for Session {
    fn drop(&mut self) {
        self.send.take();
        if let Some(thread) = self.thread.take() {
            thread.join().unwrap();
        }
    }
}
struct SessionParticipant<'a> {
    session: &'a Session,
    done: bool,
}
impl SessionParticipant<'_> {
    fn step<R: Send + 'static>(
        &self,
        f: impl FnOnce(&mut Participant<'_>) -> R + Send + 'static,
    ) -> R {
        self.session.call(move |w| {
            let mut p = w.resume_participant().unwrap();
            let r = f(&mut p);
            p.retain();
            r
        })
    }
    fn get(&mut self, t: &Table, k: &[u8]) -> Result<VersionedValue> {
        let (t, k) = (t.clone(), k.to_vec());
        self.step(move |p| p.get(&t, &k))
    }
    fn mutate(
        &mut self,
        t: &Table,
        id: u16,
        m: Mutation,
        k: &[u8],
        v: &[u8],
        lock: bool,
    ) -> Result<bool> {
        let (t, k, v) = (t.clone(), k.to_vec(), v.to_vec());
        self.step(move |p| p.mutate(&t, id, m, &k, &v, lock))
    }
    fn scan(
        &mut self,
        t: &Table,
        lo: Bound<&[u8]>,
        hi: Bound<&[u8]>,
        rev: bool,
        limit: usize,
    ) -> Result<VersionedScan> {
        let t = t.clone();
        let lo = lo.map(Vec::from);
        let hi = hi.map(Vec::from);
        self.step(move |p| {
            p.scan(
                &t,
                lo.as_ref().map(Vec::as_slice),
                hi.as_ref().map(Vec::as_slice),
                rev,
                limit,
            )
        })
    }
    fn lock(&mut self) -> Result<()> {
        self.step(|p| p.lock())
    }
    fn timestamp(&mut self) -> Result<u32> {
        self.step(|p| p.timestamp())
    }
    fn validate(&mut self) -> Result<()> {
        self.step(|p| p.validate())
    }
    fn prepared_rows(&mut self, tt: u32) -> Result<Vec<RedoRow>> {
        self.step(move |p| p.prepared_rows(tt))
    }
    fn install(&mut self, ts: u32) -> Result<()> {
        self.step(move |p| p.install(ts))
    }
    fn finish(mut self, committed: bool) -> Result<ParticipantReport> {
        self.done = true;
        self.session
            .call(move |w| w.resume_participant()?.finish(committed))
    }
    fn retain(mut self) {
        self.done = true;
    }
}
impl Drop for SessionParticipant<'_> {
    fn drop(&mut self) {
        if !self.done {
            self.session.call(|w| w.abort()).unwrap();
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn model_contract() {
        super::run(&crate::model::database());
    }
}
