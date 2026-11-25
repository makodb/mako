//! Shared local and replicated log format. A durable atomic batch saves entries,
//! hard state and sequence together. Authority is an exact, immutable identity;
//! opening local data as a Raft replica (or conversely) is never implicit.
use crate::{
    base::{self, Base, Op},
    Hard, Identity, Record, SavedState,
};

pub const FORMAT: &[u8] = b"mako-rocks-log/1";
pub const KEY_FORMAT: &[u8] = b"f";
pub const KEY_AUTHORITY: &[u8] = b"a";
pub const MAX_PAYLOAD: usize = 8 << 20;

/// Identical no-op and application payload encoding for both ordering drivers.
pub fn encode_command(command: Option<&[u8]>) -> Vec<u8> {
    match command {
        None => vec![0],
        Some(bytes) => {
            let mut out = Vec::with_capacity(bytes.len() + 1);
            out.push(1);
            out.extend_from_slice(bytes);
            out
        }
    }
}
pub fn decode_command(bytes: &[u8]) -> Result<Option<&[u8]>, String> {
    if bytes.len() > MAX_PAYLOAD + 1 {
        return Err("log command exceeds limit".into());
    }
    match bytes.split_first() {
        Some((0, [])) => Ok(None),
        Some((1, body)) => Ok(Some(body)),
        _ => Err("invalid log command".into()),
    }
}

pub struct Journal {
    base: Box<dyn Base>,
    pub sequence: u64,
    pub state: SavedState<Vec<u8>>,
    failed: bool,
}
impl Journal {
    pub fn create(
        mut base: Box<dyn Base>,
        identity: &Identity,
        authority: &[u8],
    ) -> Result<Self, String> {
        if !base.scan(b"").map_err(|e| e.to_string())?.is_empty() {
            return Err("log is not empty".into());
        }
        let initial = Record {
            hard: Some(Hard {
                term: 0,
                vote: u16::MAX,
                commit: 0,
            }),
            ..Record::default()
        };
        let mut ops = vec![
            Op::Put(base::KEY_ID.to_vec(), base::identity_bytes(identity)),
            Op::Put(KEY_FORMAT.to_vec(), FORMAT.to_vec()),
            Op::Put(KEY_AUTHORITY.to_vec(), authority.to_vec()),
        ];
        base::ops_for(&initial, 0, &mut 0, &mut ops);
        base.persist(&ops).map_err(|e| e.to_string())?;
        Self::open(base, identity, authority)
    }
    pub fn open(
        base: Box<dyn Base>,
        identity: &Identity,
        authority: &[u8],
    ) -> Result<Self, String> {
        if base.get(KEY_FORMAT).map_err(|e| e.to_string())?.as_deref() != Some(FORMAT)
            || base
                .get(KEY_AUTHORITY)
                .map_err(|e| e.to_string())?
                .as_deref()
                != Some(authority)
        {
            return Err("log format, authority or durability policy mismatch".into());
        }
        if base
            .get(base::KEY_HARD)
            .map_err(|e| e.to_string())?
            .is_none_or(|v| v.len() != 18)
        {
            return Err("log hard state is missing or malformed".into());
        }
        let (sequence, state) = base::load(&*base, identity, u16::MAX)?;
        validate_state(&state)?;
        Ok(Self {
            base,
            sequence,
            state,
            failed: false,
        })
    }
    pub fn persist(&mut self, record: Record<Vec<u8>>) -> Result<(), String> {
        if self.failed {
            return Err("log requires recovery after an uncertain write".into());
        }
        validate_record(&self.state, &record)?;
        let sequence = self
            .sequence
            .checked_add(1)
            .ok_or("log sequence exhausted")?;
        let mut top = self.state.last();
        let mut ops = Vec::new();
        base::ops_for(&record, sequence, &mut top, &mut ops);
        if let Err(error) = self.base.persist(&ops) {
            self.failed = true;
            return Err(error.to_string());
        }
        // Validation precedes persistence. No validation failure can expose a
        // partially updated in-memory authority after the batch is saved.
        self.state.apply(record)?;
        self.sequence = sequence;
        Ok(())
    }
    /// Copy exact durable authority and the retained suffix into a fresh,
    /// exclusively owned journal with the same identity. Offline conversion
    /// uses logical key/value copying, never copies live RocksDB SST/WAL files.
    pub fn copy_to(&self, destination: &mut Self) -> Result<(), String> {
        if self.failed || destination.failed || destination.sequence != 0 {
            return Err("journal copy requires a sound source and fresh destination".into());
        }
        for key in [base::KEY_ID, KEY_FORMAT, KEY_AUTHORITY] {
            if self.base.get(key).map_err(|e|e.to_string())? != destination.base.get(key).map_err(|e|e.to_string())? {
                return Err("journal copy authority differs".into());
            }
        }
        let mut ops: Vec<Op> = destination.base.scan(b"").map_err(|e|e.to_string())?
            .into_iter().map(|(key,_)| {let mut end=key.clone();end.push(0);Op::DeleteRange(key,end)}).collect();
        ops.extend(self.base.scan(b"").map_err(|e|e.to_string())?.into_iter().map(|(key,value)|Op::Put(key,value)));
        if let Err(error) = destination.base.persist(&ops).and_then(|()|destination.base.flush()) {
            destination.failed = true; return Err(error.to_string());
        }
        destination.sequence = self.sequence;
        destination.state = self.state.clone();
        Ok(())
    }
    pub fn flush(&mut self) -> Result<(), String> {
        self.base.flush().map_err(|e| e.to_string())
    }
}
fn validate_state(state: &SavedState<Vec<u8>>) -> Result<(), String> {
    let last = state
        .snap_index
        .checked_add(state.entries.len() as u64)
        .ok_or("log index overflow")?;
    if state.snap_index == u64::MAX
        || state.hard.commit < state.snap_index
        || state.hard.commit > last
        || state.snap_term > state.hard.term
        || state
            .entries
            .iter()
            .any(|(term, _)| *term > state.hard.term)
    {
        return Err("log has invalid committed prefix or term".into());
    }
    for (_, bytes) in &state.entries {
        decode_command(bytes)?;
    }
    Ok(())
}
fn validate_record(state: &SavedState<Vec<u8>>, record: &Record<Vec<u8>>) -> Result<(), String> {
    let hard = record.hard.unwrap_or(state.hard);
    let (snap, last) = if let Some(s) = &record.snapshot {
        if s.index < state.snap_index
            || s.index == u64::MAX
            || s.term > hard.term
            || s.image.is_none()
            || (s.keep && s.index > state.last())
            || (!s.keep && s.index < state.hard.commit)
        {
            return Err("invalid log snapshot".into());
        }
        (s.index, if s.keep { state.last() } else { s.index })
    } else {
        (state.snap_index, state.last())
    };
    let next_last = if let Some(from) = record.replace_from {
        if from <= snap || from > last.checked_add(1).ok_or("log index exhausted")? {
            return Err("log replacement leaves a gap".into());
        }
        // A replacement may repeat committed bytes, but cannot overwrite them.
        for index in from..=state.hard.commit {
            let old = state.entries.get((index - state.snap_index - 1) as usize);
            let new = record.entries.get((index - from) as usize);
            if old != new {
                return Err("log replacement changes committed history".into());
            }
        }
        from.checked_add(record.entries.len() as u64)
            .and_then(|n| n.checked_sub(1))
            .ok_or("log index exhausted")?
    } else {
        if !record.entries.is_empty() {
            return Err("entries lack replacement index".into());
        }
        last
    };
    if hard.term < state.hard.term
        || hard.commit < state.hard.commit
        || hard.commit < snap
        || hard.commit > next_last
    {
        return Err("log authority regressed or exceeds stored entries".into());
    }
    for (term, bytes) in &record.entries {
        if *term > hard.term {
            return Err("entry term exceeds authority".into());
        }
        decode_command(bytes)?;
    }
    Ok(())
}

/// The physical store owns its directory lock through every append and replay.
#[cfg(feature = "rocksdb")]
pub struct RocksLog {
    pub journal: Journal,
    _lock: std::fs::File,
}
#[cfg(feature = "rocksdb")]
impl RocksLog {
    pub fn open(
        path: &std::path::Path,
        identity: Identity,
        authority: &[u8],
        create: bool,
        sync: bool,
    ) -> Result<Self, String> {
        use std::{fs, io::Write};
        let error = |e: std::io::Error| e.to_string();
        let parent = path
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(std::path::Path::new("."));
        if create {
            fs::create_dir_all(parent).map_err(error)?;
        }
        let name = path
            .file_name()
            .ok_or("log needs a directory name")?
            .to_string_lossy();
        let lock = fs::OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(parent.join(format!(".{name}.lock")))
            .map_err(error)?;
        lock.try_lock()
            .map_err(|e| format!("log directory is locked: {e}"))?;
        if path.join("wal").exists() {
            return Err("legacy segmented WAL requires offline format conversion; existing data was retained".into());
        }
        let mut authority = authority.to_vec();
        authority.extend_from_slice(if sync { b"\nsync" } else { b"\nasync" });
        if !path.exists() {
            if !create {
                return Err("log missing; creation must be explicit".into());
            }
            let side = parent.join(format!(".{name}.creating"));
            if side.exists() {
                fs::remove_dir_all(&side).map_err(error)?;
            }
            fs::create_dir(&side).map_err(error)?;
            fs::create_dir(side.join("images")).map_err(error)?;
            {
                let base = crate::rocks::RocksBase::open_journal(&side.join("base"), true, sync)
                    .map_err(error)?;
                let mut journal = Journal::create(Box::new(base), &identity, &authority)?;
                // Creation always persists the format/identity, including in
                // explicitly weak async mode, before publishing the directory.
                journal.flush()?;
                let mut marker = fs::File::create(side.join("FORMAT")).map_err(error)?;
                marker.write_all(FORMAT).map_err(error)?;
                marker.sync_all().map_err(error)?;
            }
            fs::File::open(&side)
                .and_then(|f| f.sync_all())
                .map_err(error)?;
            fs::rename(&side, path).map_err(error)?;
        }
        if fs::read(path.join("FORMAT")).map_err(error)? != FORMAT {
            return Err("unsupported log directory format".into());
        }
        // Also complete interrupted directory publication on reopen.
        for ancestor in path.ancestors().skip(1) {
            let p = if ancestor.as_os_str().is_empty() {
                std::path::Path::new(".")
            } else {
                ancestor
            };
            fs::File::open(p)
                .and_then(|f| f.sync_all())
                .map_err(error)?;
        }
        let base = crate::rocks::RocksBase::open_journal(&path.join("base"), false, sync)
            .map_err(error)?;
        let journal = Journal::open(Box::new(base), &identity, &authority)?;
        if create && journal.sequence != 0 {
            return Err("log already contains records; reopen without create".into());
        }
        Ok(Self {
            journal,
            _lock: lock,
        })
    }
}
