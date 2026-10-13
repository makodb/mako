//! The store's file access, behind [`StoreFs`].
//!
//! [`RealFs`] is the disk. [`MemFs`] is a model for the tests: it keeps the
//! page cache (what a killed process leaves) apart from what was synced (what
//! a power cut leaves), and directory entries apart from the directory syncs
//! that make them durable. Both keep a ledger of what a power cut would undo:
//! `RealFs` so a crash point can simulate one on a real disk (design §5).

use std::collections::{BTreeMap, HashMap};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, MutexGuard};

/// An open file the store appends to.
pub trait StoreFile: Send {
    fn write_all(&mut self, buf: &[u8]) -> io::Result<()>;
    /// `fdatasync`: the data and the length written so far are durable.
    fn sync_data(&mut self) -> io::Result<()>;
}

/// The operations the store performs on a filesystem.
pub trait StoreFs: Send + Sync {
    fn create_dir(&self, p: &Path) -> io::Result<()>;
    /// Creates a new, empty file; fails if it exists.
    fn create_new(&self, p: &Path) -> io::Result<Box<dyn StoreFile>>;
    /// Opens an existing file to append to. RealFs's power-cut ledger counts
    /// its current bytes as synced (a caller that cannot know syncs it).
    fn open_append(&self, p: &Path) -> io::Result<Box<dyn StoreFile>>;
    /// Takes an exclusive lock on an existing file (`flock`), held while the
    /// returned guard lives; fails at once if another process holds it.
    fn lock(&self, p: &Path) -> io::Result<Box<dyn std::any::Any + Send>>;
    fn read(&self, p: &Path) -> io::Result<Vec<u8>>;
    /// The names in a directory, sorted.
    fn list(&self, dir: &Path) -> io::Result<Vec<String>>;
    fn exists(&self, p: &Path) -> bool;
    fn remove_file(&self, p: &Path) -> io::Result<()>;
    fn remove_dir_all(&self, p: &Path) -> io::Result<()>;
    fn rename(&self, from: &Path, to: &Path) -> io::Result<()>;
    /// Cuts a file to `len` bytes and syncs it.
    fn truncate(&self, p: &Path, len: u64) -> io::Result<()>;
    /// `fsync` on a directory: its entries (creations, renames, removals)
    /// are durable.
    fn sync_dir(&self, dir: &Path) -> io::Result<()>;
    /// Simulates a power cut on the files this object wrote: unsynced data
    /// and directory changes are undone. Used by crash points (`:powercut`).
    fn powercut(&self) -> io::Result<()>;
}

// ---------------------------------------------------------------- RealFs

#[derive(Default)]
struct Ledger {
    /// Per file written through this object: (length written, length synced).
    lens: HashMap<PathBuf, (u64, u64)>,
    /// Directory changes no directory sync covers yet, oldest first.
    uncovered: Vec<DirOp>,
}

enum DirOp {
    Created(PathBuf),
    Renamed { from: PathBuf, to: PathBuf, from_synced: bool, to_synced: bool },
}

fn parent(p: &Path) -> PathBuf {
    p.parent().map(Path::to_path_buf).unwrap_or_default()
}

/// The real filesystem, with a ledger for simulated power cuts.
#[derive(Clone, Default)]
pub struct RealFs {
    ledger: Arc<Mutex<Ledger>>,
}

impl RealFs {
    pub fn new() -> Self {
        Self::default()
    }

    fn ledger(&self) -> MutexGuard<'_, Ledger> {
        self.ledger.lock().unwrap_or_else(|e| e.into_inner())
    }
}

struct RealFile {
    file: File,
    path: PathBuf,
    ledger: Arc<Mutex<Ledger>>,
}

impl StoreFile for RealFile {
    fn write_all(&mut self, buf: &[u8]) -> io::Result<()> {
        self.file.write_all(buf)?;
        let mut l = self.ledger.lock().unwrap_or_else(|e| e.into_inner());
        l.lens.entry(self.path.clone()).or_insert((0, 0)).0 += buf.len() as u64;
        Ok(())
    }

    fn sync_data(&mut self) -> io::Result<()> {
        self.file.sync_data()?;
        let mut l = self.ledger.lock().unwrap_or_else(|e| e.into_inner());
        let e = l.lens.entry(self.path.clone()).or_insert((0, 0));
        e.1 = e.0;
        Ok(())
    }
}

impl StoreFs for RealFs {
    fn create_dir(&self, p: &Path) -> io::Result<()> {
        fs::create_dir(p)?;
        self.ledger().uncovered.push(DirOp::Created(p.to_path_buf()));
        Ok(())
    }

    fn create_new(&self, p: &Path) -> io::Result<Box<dyn StoreFile>> {
        let file = OpenOptions::new().write(true).create_new(true).open(p)?;
        let mut l = self.ledger();
        l.uncovered.push(DirOp::Created(p.to_path_buf()));
        l.lens.insert(p.to_path_buf(), (0, 0));
        Ok(Box::new(RealFile { file, path: p.to_path_buf(), ledger: self.ledger.clone() }))
    }

    fn open_append(&self, p: &Path) -> io::Result<Box<dyn StoreFile>> {
        let file = OpenOptions::new().append(true).open(p)?;
        let len = file.metadata()?.len();
        self.ledger().lens.insert(p.to_path_buf(), (len, len));
        Ok(Box::new(RealFile { file, path: p.to_path_buf(), ledger: self.ledger.clone() }))
    }

    fn lock(&self, p: &Path) -> io::Result<Box<dyn std::any::Any + Send>> {
        let file = OpenOptions::new().read(true).write(true).open(p)?;
        match file.try_lock() {
            Ok(()) => Ok(Box::new(file)),
            Err(fs::TryLockError::WouldBlock) => Err(io::Error::new(
                io::ErrorKind::WouldBlock,
                format!("{}: locked by another process (is this server already running?)", p.display()),
            )),
            Err(fs::TryLockError::Error(e)) => Err(e),
        }
    }

    fn read(&self, p: &Path) -> io::Result<Vec<u8>> {
        fs::read(p)
    }

    fn list(&self, dir: &Path) -> io::Result<Vec<String>> {
        let mut names = Vec::new();
        for e in fs::read_dir(dir)? {
            names.push(e?.file_name().to_string_lossy().into_owned());
        }
        names.sort();
        Ok(names)
    }

    fn exists(&self, p: &Path) -> bool {
        p.exists()
    }

    fn remove_file(&self, p: &Path) -> io::Result<()> {
        fs::remove_file(p)
    }

    fn remove_dir_all(&self, p: &Path) -> io::Result<()> {
        fs::remove_dir_all(p)
    }

    fn rename(&self, from: &Path, to: &Path) -> io::Result<()> {
        fs::rename(from, to)?;
        let mut l = self.ledger();
        if let Some(v) = l.lens.remove(from) {
            l.lens.insert(to.to_path_buf(), v);
        }
        l.uncovered.push(DirOp::Renamed {
            from: from.to_path_buf(),
            to: to.to_path_buf(),
            from_synced: false,
            to_synced: false,
        });
        Ok(())
    }

    fn truncate(&self, p: &Path, len: u64) -> io::Result<()> {
        let f = OpenOptions::new().write(true).open(p)?;
        f.set_len(len)?;
        f.sync_data()?;
        let mut l = self.ledger();
        if let Some(e) = l.lens.get_mut(p) {
            *e = (len, len);
        }
        Ok(())
    }

    fn sync_dir(&self, dir: &Path) -> io::Result<()> {
        File::open(dir)?.sync_all()?;
        let mut l = self.ledger();
        l.uncovered.retain_mut(|op| match op {
            DirOp::Created(p) => parent(p) != dir,
            DirOp::Renamed { from, to, from_synced, to_synced } => {
                *from_synced |= parent(from) == dir;
                *to_synced |= parent(to) == dir;
                !(*from_synced && *to_synced)
            }
        });
        Ok(())
    }

    fn powercut(&self) -> io::Result<()> {
        let mut l = self.ledger();
        for (p, (written, synced)) in &l.lens {
            if written > synced && p.exists() {
                OpenOptions::new().write(true).open(p)?.set_len(*synced)?;
            }
        }
        while let Some(op) = l.uncovered.pop() {
            match op {
                DirOp::Created(p) => {
                    if p.is_dir() {
                        let _ = fs::remove_dir_all(&p);
                    } else {
                        let _ = fs::remove_file(&p);
                    }
                }
                DirOp::Renamed { from, to, .. } => {
                    let _ = fs::rename(&to, &from);
                }
            }
        }
        l.lens.clear();
        Ok(())
    }
}

// ----------------------------------------------------------------- MemFs

enum Node {
    File { data: Vec<u8>, durable: Vec<u8> },
    Dir { entries: BTreeMap<String, u64>, durable: BTreeMap<String, u64> },
}

/// How a [`MemFs`] crash treats unsynced data.
#[derive(Clone, Copy, Debug)]
pub enum Crash {
    /// A killed process: the page cache survives; nothing is lost.
    Kill,
    /// A power cut: unsynced data and directory entries are lost.
    PowerCut,
    /// A power cut that also keeps part of each file's unsynced tail, the
    /// part a device happened to write (a torn write), with a stretch of
    /// zeros where a block was allocated but not written. Seeded.
    Torn(u64),
}

struct MemInner {
    nodes: HashMap<u64, Node>,
    next: u64,
    /// Mutating operations left before the simulated crash; `None`: no limit.
    budget: Option<u64>,
    dead: bool,
    ops: u64,
}

const ROOT: u64 = 0;

/// An in-memory filesystem that models crashes. Paths must be absolute.
/// Directories are nodes whose entries become durable only at
/// [`StoreFs::sync_dir`]; a renamed directory takes its subtree with it, as
/// on a real filesystem.
#[derive(Clone)]
pub struct MemFs {
    inner: Arc<Mutex<MemInner>>,
}

impl Default for MemFs {
    fn default() -> Self {
        Self::new()
    }
}

fn crashed() -> io::Error {
    io::Error::other("MemFs: crashed")
}

fn not_found(p: &Path) -> io::Error {
    io::Error::new(io::ErrorKind::NotFound, format!("{}: not found", p.display()))
}

fn split(p: &Path) -> io::Result<(PathBuf, String)> {
    let name = p
        .file_name()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "no file name"))?
        .to_string_lossy()
        .into_owned();
    Ok((parent(p), name))
}

impl MemInner {
    fn resolve(&self, p: &Path) -> Option<u64> {
        let mut cur = ROOT;
        for comp in p.components() {
            use std::path::Component;
            match comp {
                Component::RootDir => cur = ROOT,
                Component::Normal(s) => match self.nodes.get(&cur)? {
                    Node::Dir { entries, .. } => cur = *entries.get(&*s.to_string_lossy())?,
                    Node::File { .. } => return None,
                },
                _ => return None,
            }
        }
        Some(cur)
    }

    fn dir_entries(&mut self, p: &Path) -> io::Result<&mut BTreeMap<String, u64>> {
        let ino = self.resolve(p).ok_or_else(|| not_found(p))?;
        match self.nodes.get_mut(&ino) {
            Some(Node::Dir { entries, .. }) => Ok(entries),
            _ => Err(io::Error::other(format!("{}: not a directory", p.display()))),
        }
    }

    /// Spends one unit of the crash budget.
    fn op(&mut self) -> io::Result<()> {
        if self.dead {
            return Err(crashed());
        }
        self.ops += 1;
        if let Some(b) = self.budget.as_mut() {
            if *b == 0 {
                self.dead = true;
                return Err(crashed());
            }
            *b -= 1;
        }
        Ok(())
    }

    fn add(&mut self, p: &Path, node: Node) -> io::Result<u64> {
        let (dir, name) = split(p)?;
        let ino = self.next;
        let entries = self.dir_entries(&dir)?;
        if entries.contains_key(&name) {
            return Err(io::Error::new(io::ErrorKind::AlreadyExists, format!("{}", p.display())));
        }
        entries.insert(name, ino);
        self.next += 1;
        self.nodes.insert(ino, node);
        Ok(ino)
    }
}

/// A splitmix64 step, for the torn-write simulation.
fn mix(s: &mut u64) -> u64 {
    *s = s.wrapping_add(0x9E37_79B9_7F4A_7C15);
    let mut z = *s;
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^ (z >> 31)
}

impl MemFs {
    pub fn new() -> Self {
        let mut nodes = HashMap::new();
        nodes.insert(ROOT, Node::Dir { entries: BTreeMap::new(), durable: BTreeMap::new() });
        MemFs {
            inner: Arc::new(Mutex::new(MemInner { nodes, next: 1, budget: None, dead: false, ops: 0 })),
        }
    }

    fn st(&self) -> MutexGuard<'_, MemInner> {
        self.inner.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Lets `n` more mutating operations succeed; the next one fails, and so
    /// does every operation after it, until [`MemFs::crash`].
    pub fn set_budget(&self, n: Option<u64>) {
        self.st().budget = n;
    }

    /// Mutating operations performed so far (to size crash budgets).
    pub fn ops(&self) -> u64 {
        self.st().ops
    }

    /// Applies a crash and revives the filesystem with no budget.
    pub fn crash(&self, how: Crash) {
        let mut g = self.st();
        g.dead = false;
        g.budget = None;
        let seed = match how {
            Crash::Kill => return,
            Crash::PowerCut => None,
            Crash::Torn(s) => Some(s),
        };
        let mut rng = seed.unwrap_or(0) | 1;
        let mut ids: Vec<u64> = g.nodes.keys().copied().collect();
        ids.sort_unstable();
        for id in ids {
            match g.nodes.get_mut(&id).unwrap() {
                Node::Dir { entries, durable } => *entries = durable.clone(),
                Node::File { data, durable } => {
                    let mut keep = durable.clone();
                    if seed.is_some() && data.len() > durable.len() && data.starts_with(durable) {
                        let extra = data.len() - durable.len();
                        let n = (mix(&mut rng) % (extra as u64 + 1)) as usize;
                        keep.extend_from_slice(&data[durable.len()..durable.len() + n]);
                        if n > 0 && mix(&mut rng).is_multiple_of(3) {
                            // A block allocated but never written reads as zeros.
                            let z = (mix(&mut rng) % n as u64) as usize;
                            let len = keep.len();
                            for b in &mut keep[len - n + z..] {
                                *b = 0;
                            }
                        }
                    }
                    *data = keep.clone();
                    *durable = keep;
                }
            }
        }
    }

    /// The current length of a file (tests).
    pub fn len(&self, p: &Path) -> Option<usize> {
        let g = self.st();
        match g.nodes.get(&g.resolve(p)?)? {
            Node::File { data, .. } => Some(data.len()),
            Node::Dir { .. } => None,
        }
    }

    /// Overwrites a file's bytes, current and durable (tests: damage).
    pub fn poke(&self, p: &Path, data: Vec<u8>) {
        let mut g = self.st();
        let ino = g.resolve(p).expect("poke: no such file");
        if let Some(Node::File { data: d, durable }) = g.nodes.get_mut(&ino) {
            *d = data.clone();
            *durable = data;
        }
    }

    /// Creates the directories of an absolute path, durably (test setup).
    pub fn mkdir_p(&self, p: &Path) {
        let mut g = self.st();
        let mut cur = PathBuf::from("/");
        for comp in p.components().skip(1) {
            let next = cur.join(comp);
            if g.resolve(&next).is_none() {
                let ino = g.add(&next, Node::Dir { entries: BTreeMap::new(), durable: BTreeMap::new() }).unwrap();
                let pino = g.resolve(&cur).unwrap();
                if let Some(Node::Dir { durable, .. }) = g.nodes.get_mut(&pino) {
                    durable.insert(comp.as_os_str().to_string_lossy().into_owned(), ino);
                }
            }
            cur = next;
        }
    }
}

struct MemFile {
    fs: MemFs,
    ino: u64,
}

impl StoreFile for MemFile {
    fn write_all(&mut self, buf: &[u8]) -> io::Result<()> {
        let mut g = self.fs.st();
        g.op()?;
        match g.nodes.get_mut(&self.ino) {
            Some(Node::File { data, .. }) => {
                data.extend_from_slice(buf);
                Ok(())
            }
            _ => Err(io::Error::other("MemFile: gone")),
        }
    }

    fn sync_data(&mut self) -> io::Result<()> {
        let mut g = self.fs.st();
        g.op()?;
        match g.nodes.get_mut(&self.ino) {
            Some(Node::File { data, durable }) => {
                *durable = data.clone();
                Ok(())
            }
            _ => Err(io::Error::other("MemFile: gone")),
        }
    }
}

impl StoreFs for MemFs {
    fn create_dir(&self, p: &Path) -> io::Result<()> {
        let mut g = self.st();
        g.op()?;
        g.add(p, Node::Dir { entries: BTreeMap::new(), durable: BTreeMap::new() })?;
        Ok(())
    }

    fn create_new(&self, p: &Path) -> io::Result<Box<dyn StoreFile>> {
        let mut g = self.st();
        g.op()?;
        let ino = g.add(p, Node::File { data: Vec::new(), durable: Vec::new() })?;
        Ok(Box::new(MemFile { fs: self.clone(), ino }))
    }

    fn open_append(&self, p: &Path) -> io::Result<Box<dyn StoreFile>> {
        let g = self.st();
        if g.dead {
            return Err(crashed());
        }
        match g.resolve(p) {
            Some(ino) if matches!(g.nodes.get(&ino), Some(Node::File { .. })) => {
                Ok(Box::new(MemFile { fs: self.clone(), ino }))
            }
            _ => Err(not_found(p)),
        }
    }

    fn lock(&self, p: &Path) -> io::Result<Box<dyn std::any::Any + Send>> {
        if self.exists(p) {
            Ok(Box::new(()))
        } else {
            Err(not_found(p))
        }
    }

    fn read(&self, p: &Path) -> io::Result<Vec<u8>> {
        let g = self.st();
        if g.dead {
            return Err(crashed());
        }
        match g.resolve(p).and_then(|i| g.nodes.get(&i)) {
            Some(Node::File { data, .. }) => Ok(data.clone()),
            _ => Err(not_found(p)),
        }
    }

    fn list(&self, dir: &Path) -> io::Result<Vec<String>> {
        let g = self.st();
        if g.dead {
            return Err(crashed());
        }
        match g.resolve(dir).and_then(|i| g.nodes.get(&i)) {
            Some(Node::Dir { entries, .. }) => Ok(entries.keys().cloned().collect()),
            _ => Err(not_found(dir)),
        }
    }

    fn exists(&self, p: &Path) -> bool {
        self.st().resolve(p).is_some()
    }

    fn remove_file(&self, p: &Path) -> io::Result<()> {
        let mut g = self.st();
        g.op()?;
        let (dir, name) = split(p)?;
        g.dir_entries(&dir)?.remove(&name).map(|_| ()).ok_or_else(|| not_found(p))
    }

    fn remove_dir_all(&self, p: &Path) -> io::Result<()> {
        self.remove_file(p)
    }

    fn rename(&self, from: &Path, to: &Path) -> io::Result<()> {
        let mut g = self.st();
        g.op()?;
        let (fdir, fname) = split(from)?;
        let (tdir, tname) = split(to)?;
        let ino = g.dir_entries(&fdir)?.remove(&fname).ok_or_else(|| not_found(from))?;
        g.dir_entries(&tdir)?.insert(tname, ino);
        Ok(())
    }

    fn truncate(&self, p: &Path, len: u64) -> io::Result<()> {
        let mut g = self.st();
        g.op()?;
        let ino = g.resolve(p).ok_or_else(|| not_found(p))?;
        match g.nodes.get_mut(&ino) {
            Some(Node::File { data, durable }) => {
                data.truncate(len as usize);
                *durable = data.clone();
                Ok(())
            }
            _ => Err(not_found(p)),
        }
    }

    fn sync_dir(&self, dir: &Path) -> io::Result<()> {
        let mut g = self.st();
        g.op()?;
        let ino = g.resolve(dir).ok_or_else(|| not_found(dir))?;
        match g.nodes.get_mut(&ino) {
            Some(Node::Dir { entries, durable }) => {
                *durable = entries.clone();
                Ok(())
            }
            _ => Err(not_found(dir)),
        }
    }

    fn powercut(&self) -> io::Result<()> {
        self.crash(Crash::PowerCut);
        Ok(())
    }
}
