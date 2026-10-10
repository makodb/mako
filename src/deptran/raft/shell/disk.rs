// Raft disk persistence, the shell's half (docs/verus/disk-persistence.md §3;
// docs/verus/disk-persistence-plan.md P3-P5).
//
// Compiled in every build and used only in disk builds: the server holds a
// DiskShell only when `cfg!(feature = "raft_disk")` (a constant the compiler
// folds), so a memory build pays nothing. Under mtx_, each step's persist note
// becomes one record on the store's queue (record_from_note); the store's
// flusher thread writes, syncs and publishes them (raft-store). At shutdown,
// MAKO_RAFT_DISK_VERIFY=1 replays the WAL and compares it with the core.

use std::any::Any;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use raft_core::PersistNote;
use raft_store::applier::{Applier, ApplierConfig};
use raft_store::flusher::FlusherConfig;
use raft_store::{
    crash, local, open_store_with_base, store_path, BaseFactory, Codec, Durable, DurableState, Flusher, Hard, HeldReplies,
    Identity, RealFs, Record, RecordQueue, SavedState, StoreFs, WalOptions,
};

use crate::server_h::{RaftCore, RaftLog};

/// The payload a record carries: a handle clone of the logged command
/// (a refcount bump, made under mtx_; encoded on the flusher thread).
pub type Payload = rusty::RaftCommand;

extern "C" {
    fn raft_snapshot_manager_with_latest(
        manager: *const rusty::RaftSnapshotManagerPtr, ctx: *mut core::ffi::c_void,
        emit: unsafe extern "C" fn(*mut core::ffi::c_void, u64, u64, *const u8, usize)) -> bool;
    fn raft_snapshot_manager_from_bytes(out: *mut rusty::RaftSnapshotManagerPtr, index: u64, term: u64,
                                        data: *const u8, len: usize) -> bool;
    fn raft_int_event_set(event: *const rusty::RaftIntEventPtr, value: i32);
    fn raft_int_event_wait_timeout(event: *const rusty::RaftIntEventPtr, timeout_us: u64);
    fn raft_command_has_value(cmd: *const rusty::RaftCommand) -> bool;
    fn raft_command_encode(cmd: *const rusty::RaftCommand,
                           ctx: *mut core::ffi::c_void,
                           emit: unsafe extern "C" fn(*mut core::ffi::c_void, *const u8, usize));
    fn raft_command_from_bytes(bytes: *const u8, len: usize, out: *mut rusty::RaftCommand) -> bool;
}

unsafe extern "C" fn vec_emit(ctx: *mut core::ffi::c_void, bytes: *const u8, len: usize) {
    let out: &mut Vec<u8> = unsafe { &mut *(ctx as *mut Vec<u8>) };
    out.extend_from_slice(unsafe { core::slice::from_raw_parts(bytes, len) });
}

/// A command as the WAL stores it: a has-value byte, then (if it has one)
/// its wire bytes, the same bytes an AppendEntries send carries.
pub struct ShellCodec;

impl Codec<Payload> for ShellCodec {
    fn encode(&self, cmd: &Payload, out: &mut Vec<u8>) {
        let p = cmd as *const rusty::RaftCommand;
        // SAFETY: `cmd` is a live command handle; the kernels only read it,
        // and the command is never modified once logged (its save is const).
        if !unsafe { raft_command_has_value(p) } {
            out.push(0);
            return;
        }
        out.push(1);
        unsafe { raft_command_encode(p, out as *mut Vec<u8> as *mut core::ffi::c_void, vec_emit) };
    }

    fn decode(&self, bytes: &[u8]) -> Result<Payload, String> {
        match bytes.split_first() {
            Some((0, [])) => Ok(Payload::default()),
            Some((1, wire)) => {
                let mut cmd = Payload::default();
                // SAFETY: `wire` is checksummed bytes the encoder wrote;
                // `cmd` is a default (empty) carrier the kernel fills.
                if unsafe { raft_command_from_bytes(wire.as_ptr(), wire.len(), &mut cmd) } {
                    Ok(cmd)
                } else {
                    Err("undecodable command".into())
                }
            }
            _ => Err("bad command framing".into()),
        }
    }
}

/// The disk build's settings, from the environment (plan §1).
#[derive(Clone, Debug)]
pub struct DiskParams {
    pub data_dir: PathBuf,
    pub delay: Duration,
    pub segment_bytes: u64,
    pub create: bool,
    pub verify: bool,
    /// The applier's checkpoint thresholds (plan P7; design Decision 13).
    pub checkpoint_bytes: u64,
    pub checkpoint_secs: u64,
}

fn env_u64(name: &str, default: u64) -> Result<u64, String> {
    match std::env::var(name) {
        Ok(v) if !v.is_empty() => {
            if !v.bytes().all(|b| b.is_ascii_digit()) {
                return Err(format!("{name}={v:?} is not a whole number"));
            }
            v.parse().map_err(|e| format!("{name}={v:?}: {e}"))
        }
        _ => Ok(default),
    }
}

fn env_flag(name: &str) -> Result<bool, String> {
    match std::env::var(name).as_deref() {
        Ok("1") => Ok(true),
        Ok("0") | Ok("") | Err(_) => Ok(false),
        Ok(v) => Err(format!("{name}={v:?}: expected 1 or 0")),
    }
}

impl DiskParams {
    pub fn from_env() -> Result<DiskParams, String> {
        let user = std::env::var("USER").unwrap_or_else(|_| "raft".into());
        let data_dir = match std::env::var("MAKO_RAFT_DATA_DIR") {
            Ok(d) if !d.is_empty() => PathBuf::from(d),
            _ => PathBuf::from(format!("/var/tmp/raft-wal-{user}")),
        };
        let segment_bytes = env_u64("MAKO_RAFT_SEGMENT_BYTES", 64 << 20)?;
        if segment_bytes < 4096 {
            return Err(format!("MAKO_RAFT_SEGMENT_BYTES={segment_bytes} is below 4096"));
        }
        let checkpoint_bytes = env_u64("MAKO_RAFT_CHECKPOINT_BYTES", 256 << 20)?;
        if checkpoint_bytes <= segment_bytes {
            // Only closed segments are deleted: at or below one segment the
            // open one alone would keep the threshold crossed (Decision 13).
            return Err(format!("MAKO_RAFT_CHECKPOINT_BYTES={checkpoint_bytes} must exceed the segment size {segment_bytes}"));
        }
        Ok(DiskParams {
            data_dir,
            delay: Duration::from_micros(env_u64("MAKO_RAFT_FLUSH_DELAY_US", 0)?),
            checkpoint_bytes,
            checkpoint_secs: env_u64("MAKO_RAFT_CHECKPOINT_SECS", 10)?,
            segment_bytes,
            create: env_flag("MAKO_RAFT_CREATE")?,
            verify: env_flag("MAKO_RAFT_DISK_VERIFY")?,
        })
    }
}

/// The store's identity: this server, its partition, its configuration.
pub fn identity(site: u16, partition: u32, members: &[u16]) -> Identity {
    // FNV-1a over the sorted member ids.
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for m in members {
        for b in m.to_le_bytes() {
            h ^= u64::from(b);
            h = h.wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
    Identity { site: u32::from(site), partition, fingerprint: h, format: 1 }
}

/// A note, as the record the WAL stores: the hard state if it changed, and
/// the log from the note's lowest write on (each entry's term and a handle
/// clone). Called under mtx_, right after the step that left the note.
pub fn record_from_note(note: &PersistNote, log: &RaftLog) -> Record<Payload> {
    let mut rec = Record::default();
    if note.hard_ {
        rec.hard = Some(Hard { term: note.term_, vote: note.vote_, commit: note.commit_ });
    }
    if note.log_from_ != 0 {
        rec.replace_from = Some(note.log_from_);
        let last = log.last_index();
        let mut i = note.log_from_;
        while i <= last {
            let e = log.get(i).expect("a logged index below the tail");
            rec.entries.push((e.term() as u64, e.cmd().clone()));
            i += 1;
        }
    }
    rec
}

/// Posts a job to the fibers' owner thread that sets the event (the server
/// builds it over its wake-job kernel).
pub type FiberWake = Box<dyn Fn(rusty::RaftIntEventPtr) + Send + Sync>;

/// Fibers waiting for the WAL: (id, tail, event). The flusher wakes those
/// its publish covers. `wait` checks the durable number under this list's
/// mutex and the flusher takes it only after publishing, so a waiter is
/// never left for a later flush (as HeldReplies).
#[derive(Default)]
pub struct FiberWaiters {
    list: Mutex<Vec<(u64, u64, rusty::RaftIntEventPtr)>>,
    next: std::sync::atomic::AtomicU64,
}

impl FiberWaiters {
    fn ready(&self, seq: u64) -> Vec<rusty::RaftIntEventPtr> {
        let mut g = self.list.lock().unwrap_or_else(|e| e.into_inner());
        let mut out = Vec::new();
        g.retain(|(_, tail, ev)| {
            if *tail <= seq {
                out.push(ev.clone());
                false
            } else {
                true
            }
        });
        out
    }
}

/// The base every disk build keeps (plan P7): RocksDB, at `<store>/base`.
#[cfg(feature = "raft_disk")]
fn base_factory() -> Option<Box<BaseFactory>> {
    Some(Box::new(|p: &Path, create: bool| {
        raft_store::rocks::RocksBase::open(p, create).map(|b| Box::new(b) as Box<dyn raft_store::base::Base>)
    }))
}

#[cfg(not(feature = "raft_disk"))]
fn base_factory() -> Option<Box<BaseFactory>> {
    None
}

/// What recovery found, for the shell's Restore (P5).
pub struct Recovered {
    pub state: SavedState<Payload>,
    pub d: u64,
    pub repairs: Vec<String>,
}

/// A server's open store: its record queue and durable state, the flusher
/// thread, and the lock that keeps a second server off it.
pub struct DiskShell {
    pub queue: Arc<RecordQueue<Payload>>,
    pub durable: Arc<DurableState>,
    /// Replies waiting for their records (plan P4); the flusher sends them.
    pub held: Arc<HeldReplies>,
    /// Fibers waiting for their records (the tick, the campaign).
    waiters: Arc<FiberWaiters>,
    flusher: Mutex<Option<Flusher>>,
    applier: Mutex<Option<Applier>>,
    pub store: PathBuf,
    pub params: DiskParams,
    pub id: Identity,
    /// The commit index recovery restored (0 for a new store): no campaign
    /// starts until the state machine has re-applied through it (plan P5),
    /// as Mako's apply callback is chosen by role.
    pub recovered_commit: std::sync::atomic::AtomicU64,
    recovered: Mutex<Option<Recovered>>,
    /// The filesystem the store goes through (registered for `:powercut`
    /// crash points, so image writes are undone with the WAL's).
    fs: Arc<dyn StoreFs>,
    /// MAKO_RAFT_KILLTEST_DIR (plan P6): `<dir>/<site>.reveal`, where each
    /// output that leaves writes what it shows just before it leaves, and
    /// recovery writes what it restored; scripts/raft_kill/check.py
    /// requires every `recovered` line to cover the reveals before it.
    pub reveals: Option<Reveals>,
    _lock: Mutex<Box<dyn Any + Send>>,
}

/// The kill test's evidence file, shared with the held replies' sends.
pub type Reveals = Arc<std::fs::File>;

/// One evidence line, written through at once (SIGKILL loses no line that
/// returned). A failed write only loses evidence.
pub fn reveal_to(file: &Option<Reveals>, line: &str) {
    use std::io::Write;
    if let Some(f) = file {
        let _ = (&**f).write_all(format!("{line}\n").as_bytes());
    }
}

/// `reveal <kind> <term> <vote> <commit> <last> <tail>`: what an output
/// shows of the sender's state (`-` where it shows nothing), and the WAL
/// sequence it waited for.
pub fn reveal_line(kind: &str, term: u64, vote: Option<u16>, commit: Option<u64>,
                   last: Option<u64>, tail: u64) -> String {
    fn opt<T: ToString>(x: Option<T>) -> String {
        x.map_or_else(|| "-".to_string(), |v| v.to_string())
    }
    format!("reveal {kind} {term} {} {} {} {tail}", opt(vote), opt(commit), opt(last))
}

/// What a reply the core decided shows (kind, term, vote granted, last index
/// acknowledged), parked on the thread that handles the request -- the poll
/// thread, inside the service's dispatch_held -- for the service to move
/// into the held reply, which reveals it just before the reply leaves. Only
/// a decided answer is parked: a refusal at the gate or a dropped request
/// shows nothing of this server's state (a refused vote carries the
/// candidate's term).
pub type Parked = (&'static str, u64, Option<u16>, Option<u64>);

thread_local! {
    static PARKED: std::cell::Cell<Option<Parked>> = const { std::cell::Cell::new(None) };
}

/// The parked reveal, if a handler on this thread left one.
pub fn take_parked() -> Option<Parked> {
    PARKED.with(|c| c.take())
}

fn open_reveals(site: u16) -> Result<Option<Reveals>, String> {
    let dir = match std::env::var("MAKO_RAFT_KILLTEST_DIR") {
        Ok(d) if !d.is_empty() => PathBuf::from(d),
        _ => return Ok(None),
    };
    std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let path = dir.join(format!("{site}.reveal"));
    let f = std::fs::OpenOptions::new().create(true).append(true).open(&path)
        .map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(Some(Arc::new(f)))
}

impl DiskShell {
    /// Opens (or, with MAKO_RAFT_CREATE=1, creates) the store for this
    /// server and starts the flusher. Returns the shell and what recovery
    /// found (`None` for a new store). Every refusal says why.
    pub fn open(params: DiskParams, site: u16, partition: u32, members: &[u16], no_vote: u16,
                wake: FiberWake) -> Result<DiskShell, String> {
        std::fs::create_dir_all(&params.data_dir)
            .map_err(|e| format!("{}: {e}", params.data_dir.display()))?;
        local::check_local(&params.data_dir)?;
        // Before any thread starts: a refusal after them would leave them
        // running on a store the server has given up.
        let reveals = open_reveals(site)?;
        let real = RealFs::new();
        let fs: Arc<dyn StoreFs> = Arc::new(real.clone());
        crash::set_powercut_fs(fs.clone());
        let id = identity(site, partition, members);
        let store = store_path(&params.data_dir, u32::from(site), partition);
        let opts = WalOptions { segment_bytes: params.segment_bytes };
        let factory = base_factory();
        let mut opened = open_store_with_base(fs.clone(), &store, id, opts, params.create, &ShellCodec, no_vote,
                                              factory.as_deref())?;
        let d = opened.d;
        let start = Durable { seq: d, last: opened.state.last(), commit: opened.state.hard.commit };
        let queue = Arc::new(RecordQueue::new(d + 1));
        let durable = Arc::new(DurableState::new(start));
        let held = Arc::new(HeldReplies::new(durable.clone()));
        let (applier, tap) = match opened.base.take() {
            Some(b) => {
                let (a, t) = Applier::spawn(b, opened.c, fs.clone(), store.join("wal"), id, durable.clone(),
                                            ApplierConfig { checkpoint_bytes: params.checkpoint_bytes,
                                                            checkpoint_secs: params.checkpoint_secs,
                                                            queue: 64 });
                (Some(a), Some(t))
            }
            None => (None, None),
        };
        let release = held.clone();
        let waiters = Arc::new(FiberWaiters::default());
        let wake_waiters = waiters.clone();
        let flusher = Flusher::spawn(
            opened.wal,
            queue.clone(),
            Arc::new(ShellCodec),
            durable.clone(),
            FlusherConfig { delay: params.delay, tap },
            Box::new(move |d: Durable| {
                release.release(d.seq);
                for ev in wake_waiters.ready(d.seq) {
                    wake(ev);
                }
            }),
        );
        let recovered = if opened.created {
            None
        } else {
            Some(Recovered { state: opened.state, d, repairs: opened.repairs })
        };
        let shell = DiskShell {
            queue,
            durable,
            held,
            waiters,
            flusher: Mutex::new(Some(flusher)),
            applier: Mutex::new(applier),
            store,
            params,
            id,
            recovered_commit: std::sync::atomic::AtomicU64::new(0),
            recovered: Mutex::new(recovered),
            fs,
            reveals,
            _lock: Mutex::new(opened.lock),
        };
        if opened.created {
            shell.reveal("created");
        }
        Ok(shell)
    }

    /// One evidence line (MAKO_RAFT_KILLTEST_DIR; nothing when unset).
    pub fn reveal(&self, line: &str) {
        reveal_to(&self.reveals, line);
    }

    /// Queues one step's note; returns the record's sequence number. Under
    /// mtx_.
    pub fn push_note(&self, note: &PersistNote, log: &RaftLog) -> u64 {
        self.queue.push(record_from_note(note, log))
    }

    /// The tail an output produced now must wait for: the last record
    /// queued (an upper bound on its own section's).
    pub fn tail(&self) -> u64 {
        self.queue.last_seq()
    }

    /// Waits, from a fiber on the poll thread, until records 1..=tail are
    /// durable or `stop` says to give up; the flusher's publish wakes it
    /// through its event, and `recheck_us` bounds each wait so `stop` is
    /// seen. Returns whether the tail is durable.
    pub fn wait_durable(&self, tail: u64, recheck_us: u64, stop: &dyn Fn() -> bool) -> bool {
        let t0 = raft_store::stats::now_us();
        loop {
            if self.durable.seq() >= tail {
                if raft_store::stats::on() {
                    let now = raft_store::stats::now_us();
                    raft_store::stats::add(raft_store::stats::FIBER_WAIT, now - t0);
                    let p = self.durable.published_us.load(std::sync::atomic::Ordering::Acquire);
                    if p > t0 {
                        raft_store::stats::add(raft_store::stats::WAKE, now.saturating_sub(p));
                    }
                }
                return true;
            }
            if stop() {
                return false;
            }
            let event = rusty::raft_new_int_event();
            unsafe { raft_int_event_set(&event, 0) };
            let id = self.waiters.next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            {
                let mut g = self.waiters.list.lock().unwrap_or_else(|e| e.into_inner());
                if self.durable.seq() >= tail {
                    return true;
                }
                g.push((id, tail, event.clone()));
            }
            unsafe { raft_int_event_wait_timeout(&event, recheck_us) };
            self.waiters.list.lock().unwrap_or_else(|e| e.into_inner()).retain(|w| w.0 != id);
        }
    }

    /// Closes the queue and joins the flusher, which writes what is queued
    /// first. Idempotent. After every producer has stopped.
    pub fn stop(&self) {
        let f = self.flusher.lock().unwrap_or_else(|e| e.into_inner()).take();
        if let Some(f) = f {
            self.queue.close();
            f.join();
            raft_store::stats::report(&self.store.display().to_string());
        }
        // The applier stops where its base is: the WAL keeps every record
        // after c, which the next start folds (design §3); RocksDB's close
        // flushes its memtables.
        let a = self.applier.lock().unwrap_or_else(|e| e.into_inner()).take();
        if let Some(a) = a {
            if let Err(why) = a.join() {
                eprintln!("[RAFT-DISK] {}: the base stopped: {why}", self.store.display());
            }
        }
    }

    /// MAKO_RAFT_DISK_VERIFY: the WAL's replay against the core. Term, vote,
    /// commit, the log's bounds, and each entry's term and command bytes.
    pub fn verify(&self, core: &RaftCore) -> Result<String, String> {
        let fs = &*self.fs;
        let wal_dir = self.store.join("wal");
        let no_vote = raft_core::RAFT_SERVER_INVALID_SITE_ID;
        // The base's records 1..=c, then the WAL's after them.
        let (c, mut state) = match base_factory() {
            Some(f) => {
                let b = f(&self.store.join("base"), false).map_err(|e| format!("base: {e}"))?;
                raft_store::base::load(&*b, &self.id, no_vote)?
            }
            None => (0, SavedState::new(no_vote)),
        };
        let rec = raft_store::wal::recover(fs, &wal_dir, &self.id, c)?;
        for (seq, bytes) in rec.records.iter() {
            let r = raft_store::record::decode(bytes, &raft_store::BytesCodec)
                .map_err(|e| format!("record {seq}: {e}"))?;
            state.apply(r).map_err(|e| format!("record {seq}: {e}"))?;
        }
        let hard = (core.current_term_, core.vote_for_, core.commit_index_);
        let disk = (state.hard.term, state.hard.vote, state.hard.commit);
        if hard != disk {
            return Err(format!("hard state: disk {disk:?}, core {hard:?}"));
        }
        let (base, last) = (core.raft_log_.base(), core.raft_log_.last_index());
        if state.snap_index + 1 != base || state.last() != last {
            return Err(format!("log: disk {}..={}, core {base}..={last}", state.snap_index + 1, state.last()));
        }
        for i in base..=last {
            let e = core.raft_log_.get(i).expect("index within the core's log");
            let (t, bytes) = &state.entries[(i - base) as usize];
            let mut mine = Vec::new();
            ShellCodec.encode(e.cmd(), &mut mine);
            if *t != e.term() as u64 || *bytes != mine {
                return Err(format!("entry {i}: disk term {t}, core term {}; bytes equal: {}",
                                   e.term(), *bytes == mine));
            }
        }
        Ok(format!("ok records={} last={last} commit={}", rec.d, core.commit_index_))
    }
}

struct ImageWrite<'a> {
    fs: &'a dyn StoreFs,
    dir: &'a Path,
    out: Option<std::io::Result<(u64, u64, String)>>,
}

unsafe extern "C" fn write_image_emit(ctx: *mut core::ffi::c_void, index: u64, term: u64, data: *const u8,
                                      len: usize) {
    let w: &mut ImageWrite<'_> = unsafe { &mut *(ctx as *mut ImageWrite<'_>) };
    let bytes = if len == 0 { &[][..] } else { unsafe { core::slice::from_raw_parts(data, len) } };
    w.out = Some(raft_store::images::write(w.fs, w.dir, index, term, bytes).map(|n| (index, term, n)));
}

impl DiskShell {
    /// Plan P8: the snapshot store's latest image, written durably to
    /// `<store>/images` before the record naming it is queued. An I/O error
    /// aborts, as a WAL error does (design Decision 14).
    ///
    /// # Safety
    /// `manager` is the server's live snapshot-manager carrier.
    pub unsafe fn write_latest_image(&self, manager: *const rusty::RaftSnapshotManagerPtr)
        -> Option<(u64, u64, String)> {
        let dir = self.store.join("images");
        let mut w = ImageWrite { fs: &*self.fs, dir: &dir, out: None };
        let found = unsafe {
            raft_snapshot_manager_with_latest(manager, &mut w as *mut ImageWrite<'_> as *mut core::ffi::c_void,
                                              write_image_emit)
        };
        if !found {
            return None;
        }
        match w.out {
            Some(Ok(x)) => Some(x),
            Some(Err(e)) => panic!("raft-store: snapshot image write failed in {}: {e}", dir.display()),
            None => None,
        }
    }

    /// Plan P8: a snapshot store holding the recovered image, for Setup's
    /// snapshot recovery to load (it keeps an injected store).
    pub fn image_store(&self, index: u64, term: u64, name: &str) -> Result<rusty::RaftSnapshotManagerPtr, String> {
        let bytes = raft_store::images::read(&*self.fs, &self.store.join("images"), name)?;
        let mut m = rusty::RaftSnapshotManagerPtr::default();
        if !unsafe { raft_snapshot_manager_from_bytes(&mut m, index, term, bytes.as_ptr(), bytes.len()) } {
            return Err(format!("{name}: the snapshot store refused the image"));
        }
        Ok(m)
    }

    /// Where the recovered state waits for Restore (None for a new store).
    pub fn recovered_slot(&self) -> std::sync::MutexGuard<'_, Option<Recovered>> {
        self.recovered.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Parks what a reply the core decided shows (see [`Parked`]); nothing
    /// unless the kill test collects evidence.
    pub fn park(&self, p: Parked) {
        if self.reveals.is_some() {
            PARKED.with(|c| c.set(Some(p)));
        }
    }

    /// The recovered state, kept between opening the store (before snapshot
    /// recovery) and Restore (after Configure).
    pub fn take_recovered(&self) -> Option<Recovered> {
        self.recovered.lock().unwrap_or_else(|e| e.into_inner()).take()
    }
}

