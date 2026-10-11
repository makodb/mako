//! The write-ahead log: a directory of segments, appended by the flusher,
//! read back at recovery (design §3-§4).
//!
//! A segment is named by its first sequence number. The writer rotates when
//! the open segment has reached `segment_bytes`: the new segment's header is
//! synced, and then its directory entry, before any batch goes into it.

use std::io;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use crate::crash::{armed, crash_point};
use crate::fs::{StoreFile, StoreFs};
use crate::segment::{self, Identity, HEADER_LEN};

#[derive(Clone, Copy, Debug)]
pub struct WalOptions {
    /// Rotate once the open segment holds at least this many bytes.
    pub segment_bytes: u64,
}

impl Default for WalOptions {
    fn default() -> Self {
        WalOptions { segment_bytes: 64 << 20 }
    }
}

/// The WAL's writer. Owned by the flusher thread.
pub struct Wal {
    fs: Arc<dyn StoreFs>,
    dir: PathBuf,
    id: Identity,
    opts: WalOptions,
    cur: Box<dyn StoreFile>,
    cur_len: u64,
    next_seq: u64,
}

fn at(p: &Path) -> impl Fn(io::Error) -> String + Copy + '_ {
    move |e| format!("{}: {e}", p.display())
}

/// The first sequence numbers of the segments in `dir`, sorted (names that
/// are not segments' skipped).
fn segment_firsts(fs: &dyn StoreFs, dir: &Path) -> io::Result<Vec<u64>> {
    let mut segs: Vec<u64> = fs.list(dir)?.iter().filter_map(|n| segment::parse_name(n)).collect();
    segs.sort_unstable();
    Ok(segs)
}

fn new_segment(fs: &dyn StoreFs, dir: &Path, id: &Identity, first_seq: u64) -> io::Result<Box<dyn StoreFile>> {
    let mut f = fs.create_new(&dir.join(segment::name(first_seq)))?;
    f.write_all(&segment::header(id, first_seq))?;
    f.sync_data()?;
    crash_point("wal.rotate.created");
    fs.sync_dir(dir)?;
    crash_point("wal.rotate.dirsync");
    Ok(f)
}

impl Wal {
    /// Starts a WAL in an existing, empty directory: segment 1, synced.
    pub fn create(fs: Arc<dyn StoreFs>, dir: &Path, id: Identity, opts: WalOptions) -> io::Result<Wal> {
        Self::start_at(fs, dir, id, opts, 1)
    }

    /// Continues a recovered WAL whose last durable record is `d`. A last
    /// segment named `d + 1` holds its header alone (recovery cut whatever
    /// followed it): appends go there. Otherwise a fresh segment `d + 1`.
    /// Never delete-then-create: a crash between the two would leave, when
    /// it was the only segment, a store with none.
    pub fn reopen(fs: Arc<dyn StoreFs>, dir: &Path, id: Identity, opts: WalOptions, d: u64) -> io::Result<Wal> {
        if fs.exists(&dir.join(segment::name(d + 1))) {
            return Self::resume(fs, dir, id, opts, d + 1);
        }
        Self::start_at(fs, dir, id, opts, d + 1)
    }

    /// Appends to the existing, synced segment `first_seq`, which holds its
    /// header alone (store creation's segment 1 after the rename; a
    /// recovered empty last segment).
    pub fn resume(fs: Arc<dyn StoreFs>, dir: &Path, id: Identity, opts: WalOptions, first_seq: u64) -> io::Result<Wal> {
        let p = dir.join(segment::name(first_seq));
        let len = fs.read(&p)?.len();
        if len != HEADER_LEN {
            return Err(io::Error::new(io::ErrorKind::InvalidData,
                                      format!("{}: {len} bytes, not a header alone", p.display())));
        }
        let mut cur = fs.open_append(&p)?;
        // A crash may have cut its rotation short, before the header's sync
        // or its directory entry's (a killed process's page cache kept both):
        // make them durable before anything is acknowledged from it.
        cur.sync_data()?;
        fs.sync_dir(dir)?;
        Ok(Wal { fs, dir: dir.to_path_buf(), id, opts, cur, cur_len: HEADER_LEN as u64, next_seq: first_seq })
    }

    fn start_at(fs: Arc<dyn StoreFs>, dir: &Path, id: Identity, opts: WalOptions, first: u64) -> io::Result<Wal> {
        let cur = new_segment(&*fs, dir, &id, first)?;
        Ok(Wal { fs, dir: dir.to_path_buf(), id, opts, cur, cur_len: HEADER_LEN as u64, next_seq: first })
    }

    /// The sequence number the next batch must start at.
    pub fn next_seq(&self) -> u64 {
        self.next_seq
    }

    /// Appends one batch of encoded records numbered from `first_seq`, and
    /// syncs it. On return the batch is durable.
    pub fn append(&mut self, first_seq: u64, records: &[Vec<u8>]) -> io::Result<()> {
        assert_eq!(first_seq, self.next_seq, "WAL batches must be contiguous");
        assert!(!records.is_empty(), "empty WAL batch");
        if self.cur_len >= self.opts.segment_bytes {
            self.cur = new_segment(&*self.fs, &self.dir, &self.id, first_seq)?;
            self.cur_len = HEADER_LEN as u64;
        }
        let bytes = segment::frame(first_seq, records);
        if armed("wal.write.half") {
            let half = bytes.len() / 2;
            self.cur.write_all(&bytes[..half])?;
            crash_point("wal.write.half");
            self.cur.write_all(&bytes[half..])?;
        } else {
            self.cur.write_all(&bytes)?;
        }
        crash_point("wal.write.done");
        self.cur.sync_data()?;
        crash_point("wal.sync.done");
        self.cur_len += bytes.len() as u64;
        self.next_seq += records.len() as u64;
        Ok(())
    }
}

/// What recovery read: the records after `c`, with their sequence numbers,
/// and `d`, the last durable record.
pub struct Recovered {
    pub records: Vec<(u64, Vec<u8>)>,
    pub d: u64,
    /// What recovery repaired (a deleted header-less segment, a cut tail).
    pub repairs: Vec<String>,
}

/// Reads the WAL in `dir` for the store `id`, given that a base already
/// holds records 1..=c (0 without a base). Repairs only what a crash can
/// leave: a last segment whose header is short or bad with nothing but
/// zeros after it (a rotation cut short: the header is synced before any
/// batch, so it holds no records) is deleted; a torn batch at the end of the
/// last segment is cut off. Any other damage -- a bad header with data after
/// it is damage or a newer format -- fails closed with the reason.
pub fn recover(fs: &dyn StoreFs, dir: &Path, id: &Identity, c: u64) -> Result<Recovered, String> {
    let ctx = at(dir);
    let mut segs: Vec<(u64, String)> = Vec::new();
    for name in fs.list(dir).map_err(ctx)? {
        match segment::parse_name(&name) {
            Some(first) => segs.push((first, name)),
            None => return Err(format!("{}: unexpected file {name:?} in the WAL directory", dir.display())),
        }
    }
    segs.sort();
    let mut repairs = Vec::new();

    // Read every segment's header; only the last may be bad.
    let mut loaded: Vec<(u64, PathBuf, Vec<u8>)> = Vec::new();
    let n = segs.len();
    for (k, (first, name)) in segs.into_iter().enumerate() {
        let path = dir.join(&name);
        let bytes = fs.read(&path).map_err(ctx)?;
        match segment::parse_header(&bytes) {
            Ok((hid, hfirst)) => {
                if hid != *id {
                    return Err(format!("{}: belongs to {hid:?}, not {id:?}", path.display()));
                }
                if hfirst != first {
                    return Err(format!("{}: header says first record {hfirst}", path.display()));
                }
                loaded.push((first, path, bytes));
            }
            Err(why) if k + 1 == n && bytes.iter().skip(HEADER_LEN).all(|&b| b == 0) => {
                fs.remove_file(&path).map_err(ctx)?;
                fs.sync_dir(dir).map_err(ctx)?;
                repairs.push(format!("deleted {name}: {why} (a rotation the crash interrupted)"));
            }
            Err(why) if k + 1 == n => {
                return Err(format!("{}: {why}, with data after the header (damage, or a newer format)", path.display()))
            }
            Err(why) => return Err(format!("{}: {why}, and it is not the last segment", path.display())),
        }
    }
    if loaded.is_empty() {
        return Err(format!("{}: no WAL segment", dir.display()));
    }
    // Segments wholly at or below c are the base's; the first kept one must
    // start at or below c + 1.
    while loaded.len() > 1 && loaded[1].0 <= c + 1 {
        loaded.remove(0);
    }
    if loaded[0].0 > c + 1 {
        return Err(format!("{}: WAL starts at record {}, after {} (a gap)", dir.display(), loaded[0].0, c + 1));
    }

    let last = loaded.len() - 1;
    let mut expected = loaded[0].0;
    let mut records = Vec::new();
    let mut cut: Option<(PathBuf, u64)> = None;
    for (k, (first, path, bytes)) in loaded.iter().enumerate() {
        if *first != expected {
            return Err(format!("{}: starts at record {first}, expected {expected}", path.display()));
        }
        let scan = segment::scan(&bytes[HEADER_LEN..]).map_err(|e| format!("{}: {e}", path.display()))?;
        for b in &scan.batches {
            if b.first_seq != expected {
                return Err(format!("{}: batch starts at record {}, expected {expected}", path.display(), b.first_seq));
            }
            for r in &b.records {
                if expected > c {
                    records.push((expected, r.to_vec()));
                }
                expected += 1;
            }
        }
        if let Some(why) = scan.torn {
            if k != last {
                return Err(format!("{}: {why}, and it is not the last segment", path.display()));
            }
            cut = Some((path.clone(), (HEADER_LEN + scan.good_len) as u64));
            repairs.push(format!("cut {}: {why}", path.display()));
        }
    }
    let d = expected - 1;
    if c > d {
        return Err(format!("{}: the base holds record {c}, the WAL ends at {d}", dir.display()));
    }
    if let Some((path, len)) = cut {
        crash_point("recover.cut");
        fs.truncate(&path, len).map_err(ctx)?;
    } else {
        // After a process kill the page cache still holds the last batches,
        // synced or not, and this read saw them. Sync them before anything
        // is built on them: a power cut after the restart must not take
        // away records the restarted server counts as durable. Only the last
        // segment can hold unsynced bytes (each batch is synced before the
        // next is written, and a rotation follows a sync).
        let (_, path, _) = &loaded[last];
        fs.open_append(path).and_then(|mut f| f.sync_data()).map_err(ctx)?;
    }
    Ok(Recovered { records, d, repairs })
}

/// Calls `f` on the records numbered `from..=to`, in order, reading each
/// segment that holds some of them once (the applier's catch-up, design §3).
/// Changes nothing on disk. The open segment may end in a batch still being
/// written; records past `to` are never read, and `to` must be durable.
/// Fails if any record of the range is missing, out of order or repeated.
pub fn for_each_record<F>(fs: &dyn StoreFs, dir: &Path, id: &Identity, from: u64, to: u64, mut f: F)
    -> Result<(), String>
where
    F: FnMut(u64, &[u8]) -> Result<(), String>,
{
    let segs = segment_firsts(fs, dir).map_err(at(dir))?;
    let mut expected = from;
    for (k, first) in segs.iter().enumerate() {
        let next = segs.get(k + 1).copied().unwrap_or(u64::MAX);
        if next <= from || *first > to {
            continue;
        }
        let path = dir.join(segment::name(*first));
        let bytes = fs.read(&path).map_err(at(&path))?;
        let (hid, hfirst) = segment::parse_header(&bytes).map_err(|e| format!("{}: {e}", path.display()))?;
        if hid != *id || hfirst != *first {
            return Err(format!("{}: header names {hid:?} from record {hfirst}", path.display()));
        }
        let scan = segment::scan(&bytes[HEADER_LEN..]).map_err(|e| format!("{}: {e}", path.display()))?;
        for b in &scan.batches {
            for (j, r) in b.records.iter().enumerate() {
                let seq = b.first_seq + j as u64;
                if seq < expected || seq > to {
                    continue;
                }
                if seq != expected {
                    return Err(format!("{}: record {seq} where {expected} was expected", path.display()));
                }
                f(seq, r)?;
                expected += 1;
            }
        }
    }
    if expected != to + 1 {
        return Err(format!("{}: records {from}..={to} are not all in the WAL (stopped at {expected})", dir.display()));
    }
    Ok(())
}

/// The closed segments whose records all lie at or below `c`: those a base
/// holding records 1..=c makes redundant. The last segment (the open one)
/// is never listed.
pub fn covered_segments(fs: &dyn StoreFs, dir: &Path, c: u64) -> io::Result<Vec<PathBuf>> {
    let segs = segment_firsts(fs, dir)?;
    let mut out = Vec::new();
    for w in segs.windows(2) {
        if w[1] <= c + 1 {
            out.push(dir.join(segment::name(w[0])));
        }
    }
    Ok(out)
}
