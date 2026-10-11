//! Named crash points (design §5 "Tests kill processes", §8).
//!
//! `MAKO_RAFT_CRASH=<point>[:<n>][:powercut]` arms one point: its n-th pass
//! (default the first) prints `crash <point>` to stderr and SIGKILLs the
//! process. With `:powercut` it first undoes, through the registered
//! filesystem's ledger, every write and directory change no sync covered, so
//! a real disk is left as a power cut would leave it ([`crate::fs::RealFs`]).
//! Not undone: an unsynced unlink (the ledger does not keep the bytes) and
//! anything RocksDB wrote (the base is behind its own flush, which waits).
//! The kill-test driver arms a point on one launch and checks what the
//! restart recovers. Unarmed, a crash point is one atomic load.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

use crate::fs::StoreFs;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CrashSpec {
    pub point: String,
    pub nth: u64,
    pub powercut: bool,
}

/// Parses `<point>[:<n>][:powercut]`.
pub fn parse(spec: &str) -> Result<CrashSpec, String> {
    let mut parts = spec.split(':');
    let point = parts.next().unwrap_or("").to_string();
    if point.is_empty() {
        return Err(format!("MAKO_RAFT_CRASH={spec:?}: no point"));
    }
    let mut nth = 1;
    let mut powercut = false;
    for p in parts {
        if p == "powercut" {
            powercut = true;
        } else if !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()) {
            nth = p.parse().map_err(|e| format!("MAKO_RAFT_CRASH={spec:?}: {e}"))?;
            if nth == 0 {
                return Err(format!("MAKO_RAFT_CRASH={spec:?}: n must be at least 1"));
            }
        } else {
            return Err(format!("MAKO_RAFT_CRASH={spec:?}: bad field {p:?}"));
        }
    }
    Ok(CrashSpec { point, nth, powercut })
}

fn spec() -> Option<&'static CrashSpec> {
    static SPEC: OnceLock<Option<CrashSpec>> = OnceLock::new();
    SPEC.get_or_init(|| match std::env::var("MAKO_RAFT_CRASH") {
        Ok(s) if !s.is_empty() => match parse(&s) {
            Ok(c) => Some(c),
            Err(e) => panic!("{e}"),
        },
        _ => None,
    })
    .as_ref()
}

static PASSES: AtomicU64 = AtomicU64::new(0);

fn powercut_fs() -> &'static Mutex<Option<Arc<dyn StoreFs>>> {
    static FS: OnceLock<Mutex<Option<Arc<dyn StoreFs>>>> = OnceLock::new();
    FS.get_or_init(|| Mutex::new(None))
}

/// Registers the filesystem whose ledger a `:powercut` crash applies.
pub fn set_powercut_fs(fs: Arc<dyn StoreFs>) {
    *powercut_fs().lock().unwrap_or_else(|e| e.into_inner()) = Some(fs);
}

extern "C" {
    fn kill(pid: i32, sig: i32) -> i32;
    fn getpid() -> i32;
}

const SIGKILL: i32 = 9;

/// Whether `MAKO_RAFT_CRASH` arms `name` (a writer that needs extra steps
/// only so a point can fire takes them only then).
pub fn armed(name: &str) -> bool {
    spec().is_some_and(|s| s.point == name)
}

/// A named crash point; a no-op unless `MAKO_RAFT_CRASH` arms `name`.
pub fn crash_point(name: &str) {
    let Some(s) = spec() else { return };
    if s.point != name || PASSES.fetch_add(1, Ordering::SeqCst) + 1 != s.nth {
        return;
    }
    if s.powercut {
        if let Some(fs) = powercut_fs().lock().unwrap_or_else(|e| e.into_inner()).as_ref() {
            let _ = fs.powercut();
        }
    }
    // One write(2): the C++ logger shares the descriptor, and eprintln!
    // writes "crash ", the name and the newline separately, so a log line
    // from another thread could land inside the line the driver looks for.
    let _ = std::io::Write::write_all(&mut std::io::stderr(), format!("crash {name}\n").as_bytes());
    // SAFETY: kill(2) on our own pid; it does not return.
    unsafe {
        kill(getpid(), SIGKILL);
    }
    std::process::abort();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses() {
        assert_eq!(parse("wal.write.done").unwrap(), CrashSpec { point: "wal.write.done".into(), nth: 1, powercut: false });
        assert_eq!(parse("wal.sync.done:3").unwrap().nth, 3);
        let c = parse("create.rename:2:powercut").unwrap();
        assert!(c.powercut && c.nth == 2);
        assert!(parse("x:powercut").unwrap().powercut);
        assert!(parse("").is_err());
        assert!(parse("x:0").is_err());
        assert!(parse("x:-1").is_err());
        assert!(parse("x:bogus").is_err());
    }
}
