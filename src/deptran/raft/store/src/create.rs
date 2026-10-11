//! Opening a server's store: atomic creation, the lock, recovery (design §4
//! "Recovery", Decision 10).
//!
//! ```text
//! <root>/<site>-<partition>/            the store
//!     LOCK                              flock()ed while a server runs
//!     wal/00000000000000000001.seg
//! <root>/<site>-<partition>.creating/   only while being created
//! ```
//! A store is built whole in the `.creating` side directory, synced, and
//! renamed into place; only the rename makes it exist. A crash while creating
//! leaves at most a `.creating`, which a creating launch deletes and redoes,
//! and which any other launch refuses with a reason.

use std::any::Any;
use std::ffi::OsString;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use crate::base::{self, Base, Op, KEY_C, KEY_ID};
use crate::crash::crash_point;
use crate::fs::StoreFs;
use crate::record::{self, Codec};
use crate::segment::Identity;
use crate::state::SavedState;
use crate::wal::{self, Wal, WalOptions};

/// `<root>/<site>-<partition>`.
pub fn store_path(root: &Path, site: u32, partition: u32) -> PathBuf {
    root.join(format!("{site}-{partition}"))
}

fn side_path(store: &Path) -> PathBuf {
    let mut name: OsString = store.file_name().unwrap_or_default().to_owned();
    name.push(".creating");
    store.with_file_name(name)
}

/// Opens a store's base at a path; `true`: create it (only inside a store
/// being created). Production passes `RocksBase::open`.
pub type BaseFactory = dyn Fn(&Path, bool) -> io::Result<Box<dyn Base>>;

/// An open store: the WAL writer, the state its records fold to, and the
/// lock that keeps a second server off it.
pub struct Opened<P> {
    /// The base, with records 1..=c in it (design §3); `None` without one.
    pub base: Option<Box<dyn Base>>,
    pub c: u64,
    pub wal: Wal,
    pub state: SavedState<P>,
    /// The last durable record.
    pub d: u64,
    /// This launch created the store.
    pub created: bool,
    /// What recovery repaired, for the log.
    pub repairs: Vec<String>,
    pub lock: Box<dyn Any + Send>,
}

fn io_ctx(p: &Path) -> impl Fn(io::Error) -> String + '_ {
    move |e| format!("{}: {e}", p.display())
}

fn create<P>(fs: &Arc<dyn StoreFs>, store: &Path, id: Identity, opts: WalOptions, no_vote: u16,
             base_factory: Option<&BaseFactory>) -> Result<Opened<P>, String> {
    let side = side_path(store);
    let parent = store.parent().unwrap_or(Path::new("/")).to_path_buf();
    let e = io_ctx(&side);
    if fs.exists(&side) {
        fs.remove_dir_all(&side).map_err(&e)?;
        fs.sync_dir(&parent).map_err(io_ctx(&parent))?;
    }
    fs.create_dir(&side).map_err(&e)?;
    let mut lock_file = fs.create_new(&side.join("LOCK")).map_err(&e)?;
    lock_file.sync_data().map_err(&e)?;
    drop(lock_file);
    let lock = fs.lock(&side.join("LOCK")).map_err(&e)?;
    let side_wal = side.join("wal");
    fs.create_dir(&side_wal).map_err(&e)?;
    fs.create_dir(&side.join("images")).map_err(&e)?;
    drop(Wal::create(fs.clone(), &side_wal, id, opts).map_err(&e)?);
    if let Some(factory) = base_factory {
        let mut b = factory(&side.join("base"), true).map_err(&e)?;
        b.write(&[Op::Put(KEY_ID.to_vec(), base::identity_bytes(&id)),
                  Op::Put(KEY_C.to_vec(), 0u64.to_le_bytes().to_vec())]).map_err(&e)?;
        b.flush().map_err(&e)?;
    }
    fs.sync_dir(&side).map_err(&e)?;
    crash_point("create.files");
    fs.rename(&side, store).map_err(io_ctx(store))?;
    crash_point("create.rename");
    fs.sync_dir(&parent).map_err(io_ctx(&parent))?;
    let wal = Wal::resume(fs.clone(), &store.join("wal"), id, opts, 1).map_err(io_ctx(store))?;
    let base = match base_factory {
        Some(factory) => Some(factory(&store.join("base"), false).map_err(io_ctx(store))?),
        None => None,
    };
    Ok(Opened { base, c: 0, wal, state: SavedState::new(no_vote), d: 0, created: true, repairs: Vec::new(), lock })
}

fn recover<P>(
    fs: &Arc<dyn StoreFs>,
    store: &Path,
    id: Identity,
    opts: WalOptions,
    codec: &dyn Codec<P>,
    no_vote: u16,
    base_factory: Option<&BaseFactory>,
) -> Result<Opened<P>, String> {
    let e = io_ctx(store);
    let lock = fs.lock(&store.join("LOCK")).map_err(&e)?;
    let side = side_path(store);
    let parent = store.parent().unwrap_or(Path::new("/")).to_path_buf();
    let mut repairs = Vec::new();
    if fs.exists(&side) {
        // Both exist only if creation was retried around a finished store.
        fs.remove_dir_all(&side).map_err(io_ctx(&side))?;
        repairs.push(format!("deleted the leftover {}", side.display()));
    }
    // A creation killed between its rename and the parent's sync leaves a
    // store a process kill keeps but a power cut loses. Make it durable
    // before this server acknowledges anything it writes.
    fs.sync_dir(&parent).map_err(io_ctx(&parent))?;
    let wal_dir = store.join("wal");
    // The base holds records 1..=c (design §3); the WAL the rest. A base that
    // does not open fails closed with RocksDB's reason: never repaired,
    // never recreated empty.
    let (base, c, mut state) = match base_factory {
        Some(factory) => {
            crash_point("recover.base");
            let b = factory(&store.join("base"), false).map_err(|e| format!("{}: base: {e}", store.display()))?;
            let (c, raw) = base::load(&*b, &id, no_vote).map_err(|w| format!("{}: {w}", store.display()))?;
            let mut state = SavedState::new(no_vote);
            state.hard = raw.hard;
            state.snap_index = raw.snap_index;
            state.snap_term = raw.snap_term;
            state.image = raw.image;
            for (i, (term, bytes)) in raw.entries.into_iter().enumerate() {
                let p = codec.decode(&bytes)
                    .map_err(|w| format!("{}: base entry {}: {w}", store.display(), raw.snap_index + 1 + i as u64))?;
                state.entries.push((term, p));
            }
            (Some(b), c, state)
        }
        None => (None, 0, SavedState::new(no_vote)),
    };
    let rec = wal::recover(&**fs, &wal_dir, &id, c)?;
    repairs.extend(rec.repairs);
    for (seq, bytes) in rec.records {
        let r = record::decode(&bytes, codec).map_err(|why| format!("{}: record {seq}: {why}", wal_dir.display()))?;
        state.apply(r).map_err(|why| format!("{}: record {seq}: {why}", wal_dir.display()))?;
    }
    // Snapshot images (design §3-§4): a `.tmp` names no record, and an image
    // below the state's snapshot is superseded.
    let images = store.join("images");
    if !fs.exists(&images) {
        fs.create_dir(&images).map_err(&e)?;
        fs.sync_dir(store).map_err(&e)?;
    }
    let gone = crate::images::cleanup(&**fs, &images, state.snap_index, true).map_err(&e)?;
    if gone > 0 {
        repairs.push(format!("deleted {gone} stale snapshot image file(s)"));
    }
    crash_point("recover.segment");
    let wal = Wal::reopen(fs.clone(), &wal_dir, id, opts, rec.d).map_err(&e)?;
    Ok(Opened { base, c, wal, state, d: rec.d, created: false, repairs, lock })
}

/// Opens the store at `store` (see [`store_path`]). With `create` (this
/// launch creates the cluster, `MAKO_RAFT_CREATE=1`) a new store is made,
/// and an existing one is refused; without it an existing store is
/// recovered, and a missing one is refused (B6: a server that lost its
/// state must not rejoin as a fresh one). Every refusal says why.
pub fn open_store<P>(
    fs: Arc<dyn StoreFs>,
    store: &Path,
    id: Identity,
    opts: WalOptions,
    create_new: bool,
    codec: &dyn Codec<P>,
    no_vote: u16,
) -> Result<Opened<P>, String> {
    open_store_with_base(fs, store, id, opts, create_new, codec, no_vote, None)
}

/// [`open_store`], with a base (design §3).
#[allow(clippy::too_many_arguments)]
pub fn open_store_with_base<P>(
    fs: Arc<dyn StoreFs>,
    store: &Path,
    id: Identity,
    opts: WalOptions,
    create_new: bool,
    codec: &dyn Codec<P>,
    no_vote: u16,
    base_factory: Option<&BaseFactory>,
) -> Result<Opened<P>, String> {
    let exists = fs.exists(store);
    if create_new {
        if exists {
            // A creation killed after its rename left a whole store that
            // has never recorded anything: opening it is the creation's
            // result, and loses nothing. A store holding any record is
            // refused: a creating launch never discards state (B6).
            let o = recover(&fs, store, id, opts, codec, no_vote, base_factory)?;
            if o.d == 0 && o.c == 0 {
                return Ok(Opened { created: true, ..o });
            }
            return Err(format!(
                "{}: a store with {} record(s) exists; MAKO_RAFT_CREATE=1 creates only (unset it to recover this one)",
                store.display(), o.d
            ));
        }
        return create(&fs, store, id, opts, no_vote, base_factory);
    }
    if exists {
        return recover(&fs, store, id, opts, codec, no_vote, base_factory);
    }
    if fs.exists(&side_path(store)) {
        return Err(format!(
            "{}: creation interrupted (only {} exists); relaunch with MAKO_RAFT_CREATE=1",
            store.display(),
            side_path(store).display()
        ));
    }
    Err(format!(
        "{}: no store; a server starts empty only when MAKO_RAFT_CREATE=1 (this launch creates the cluster)",
        store.display()
    ))
}
