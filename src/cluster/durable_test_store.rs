//! Test/example backend only. Real files retain byte batches across process loss.
//! This is not a production RocksDB or transaction-recovery adapter.
use super::native::durable_kv::{CommitResult, DurableKv, ReadResult, WriteOp};
use std::collections::BTreeMap;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

#[derive(Clone, Copy, Debug, Default)]
pub enum Fault {
    #[default]
    None,
    NotCommitted,
    UnknownAbort,
    UnknownCommit,
    UnknownDelayedCommit,
}

pub struct DiskKv {
    root: PathBuf,
    cells: BTreeMap<Vec<u8>, Vec<u8>>,
    pending: Option<Vec<WriteOp>>,
    resolve_commit: bool,
    pub fault: Fault,
    pub skip_fault: usize,
    pub fail_read: bool,
    pub fail_settle: bool,
    pub submissions: usize,
}

fn put_u64(out: &mut Vec<u8>, value: usize) {
    out.extend_from_slice(&u64::try_from(value).unwrap().to_le_bytes());
}
fn put_bytes(out: &mut Vec<u8>, value: &[u8]) {
    put_u64(out, value.len());
    out.extend_from_slice(value);
}
fn take_u64(input: &mut &[u8]) -> usize {
    assert!(input.len() >= 8, "truncated fixture record");
    let (word, rest) = input.split_at(8);
    *input = rest;
    usize::try_from(u64::from_le_bytes(word.try_into().unwrap())).unwrap()
}
fn take_bytes(input: &mut &[u8]) -> Vec<u8> {
    let count = take_u64(input);
    assert!(count <= input.len(), "truncated fixture bytes");
    let (bytes, rest) = input.split_at(count);
    *input = rest;
    bytes.to_vec()
}
fn load(path: &Path) -> Option<Vec<u8>> {
    match File::open(path) {
        Ok(mut file) => {
            let mut bytes = Vec::new();
            file.read_to_end(&mut bytes).unwrap();
            Some(bytes)
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => None,
        Err(error) => panic!("reading fixture {}: {error}", path.display()),
    }
}
fn replace(root: &Path, name: &str, bytes: &[u8]) {
    let temporary = root.join(format!("{name}.new"));
    let mut file = OpenOptions::new().create(true).truncate(true).write(true)
        .open(&temporary).unwrap();
    file.write_all(bytes).unwrap();
    file.sync_all().unwrap();
    drop(file);
    fs::rename(temporary, root.join(name)).unwrap();
    File::open(root).unwrap().sync_all().unwrap();
}

impl DiskKv {
    pub fn open(root: &Path) -> Self {
        fs::create_dir_all(root).unwrap();
        let mut cells = BTreeMap::new();
        if let Some(bytes) = load(&root.join("committed")) {
            let mut input = bytes.as_slice();
            let count = take_u64(&mut input);
            for _ in 0..count {
                let key = take_bytes(&mut input);
                let value = take_bytes(&mut input);
                assert!(cells.insert(key, value).is_none(), "duplicate fixture key");
            }
            assert!(input.is_empty(), "trailing fixture state");
        }
        let mut pending = None;
        let mut resolve_commit = false;
        if let Some(bytes) = load(&root.join("accepted")) {
            let mut input = bytes.as_slice();
            assert!(!input.is_empty(), "truncated accepted batch");
            resolve_commit = match input[0] { 0 => false, 1 => true, _ => panic!("bad disposition") };
            input = &input[1..];
            let count = take_u64(&mut input);
            let mut batch = Vec::with_capacity(count);
            for _ in 0..count {
                let key = take_bytes(&mut input);
                assert!(!input.is_empty(), "missing write kind");
                let kind = input[0];
                input = &input[1..];
                let value = match kind {
                    0 => None,
                    1 => Some(take_bytes(&mut input)),
                    _ => panic!("bad write kind"),
                };
                batch.push(WriteOp { key, value });
            }
            assert!(input.is_empty(), "trailing accepted batch");
            pending = Some(batch);
        }
        Self { root: root.to_owned(), cells, pending, resolve_commit,
            fault: Fault::None, skip_fault: 0, fail_read: false, fail_settle: false, submissions: 0 }
    }

    fn persist_cells(&self) {
        let mut bytes = Vec::new();
        put_u64(&mut bytes, self.cells.len());
        for (key, value) in &self.cells {
            put_bytes(&mut bytes, key);
            put_bytes(&mut bytes, value);
        }
        replace(&self.root, "committed", &bytes);
    }

    fn apply(&mut self, batch: &[WriteOp]) {
        for operation in batch {
            match &operation.value {
                Some(value) => { self.cells.insert(operation.key.clone(), value.clone()); }
                None => { self.cells.remove(&operation.key); }
            }
        }
        self.persist_cells();
    }

    fn accept(&mut self, batch: Vec<WriteOp>, commit: bool) {
        let mut bytes = vec![u8::from(commit)];
        put_u64(&mut bytes, batch.len());
        for operation in &batch {
            put_bytes(&mut bytes, &operation.key);
            bytes.push(u8::from(operation.value.is_some()));
            if let Some(value) = &operation.value { put_bytes(&mut bytes, value); }
        }
        replace(&self.root, "accepted", &bytes);
        self.resolve_commit = commit;
        self.pending = Some(batch);
    }

    pub fn bytes(&self, key: &[u8]) -> Option<&[u8]> {
        self.cells.get(key).map(Vec::as_slice)
    }

    pub fn all_bytes(&self) -> &BTreeMap<Vec<u8>, Vec<u8>> { &self.cells }
    pub fn settled(&self) -> bool { self.pending.is_none() }

    /// Deliberate corruption for malformed-record rejection scenarios only.
    pub fn corrupt(&mut self, key: Vec<u8>, value: Option<Vec<u8>>) {
        self.apply(&[WriteOp { key, value }]);
    }
}

impl DurableKv for DiskKv {
    fn read(&mut self, key: &[u8]) -> ReadResult {
        if std::mem::take(&mut self.fail_read) || self.pending.is_some() {
            return ReadResult::Io;
        }
        match self.cells.get(key) {
            Some(value) => ReadResult::Found(value.clone()),
            None => ReadResult::Absent,
        }
    }

    fn submit(&mut self, batch: Vec<WriteOp>) -> CommitResult {
        if self.pending.is_some() { return CommitResult::NotCommitted; }
        self.submissions += 1;
        let fault = if self.skip_fault == 0 { std::mem::take(&mut self.fault) }
            else { self.skip_fault -= 1; Fault::None };
        match fault {
            Fault::None => { self.apply(&batch); CommitResult::Committed }
            Fault::NotCommitted => CommitResult::NotCommitted,
            Fault::UnknownAbort => { self.accept(batch, false); CommitResult::Unknown }
            Fault::UnknownDelayedCommit => { self.accept(batch, true); CommitResult::Unknown }
            Fault::UnknownCommit => {
                self.apply(&batch);
                self.accept(batch, true);
                CommitResult::Unknown
            }
        }
    }

    fn settle(&mut self) -> bool {
        if std::mem::take(&mut self.fail_settle) { return false; }
        if let Some(batch) = self.pending.take() {
            if self.resolve_commit { self.apply(&batch); }
            fs::remove_file(self.root.join("accepted")).unwrap();
            File::open(&self.root).unwrap().sync_all().unwrap();
        }
        true
    }
}

pub struct Directory(PathBuf);
impl Directory {
    pub fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!("mako-native-kv-{}-{}",
            std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
    pub fn path(&self) -> &Path { &self.0 }
}
impl Drop for Directory {
    fn drop(&mut self) { fs::remove_dir_all(&self.0).unwrap(); }
}
