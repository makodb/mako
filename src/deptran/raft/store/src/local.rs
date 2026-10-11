//! The local-filesystem check (design §5 "Where", Decision 17).
//!
//! A store opens only on a filesystem type in [`LOCAL_TYPES`]. The mount
//! holding the data directory is found in `/proc/self/mountinfo` (std only:
//! no `statfs` binding), so a mistyped `MAKO_RAFT_DATA_DIR` can never write
//! the WAL into the NFS home.

use std::path::{Component, Path, PathBuf};

/// Filesystems a store may live on.
pub const LOCAL_TYPES: &[&str] = &["ext4", "xfs", "btrfs", "tmpfs"];

/// mountinfo escapes space, tab, newline and backslash as `\ooo`.
fn unescape(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'\\' && i + 3 < b.len() &&b[i + 1..i + 4].iter().all(|c| (b'0'..=b'7').contains(c)) {
            let v = (b[i + 1] - b'0') * 64 + (b[i + 2] - b'0') * 8 + (b[i + 3] - b'0');
            out.push(v);
            i += 4;
        } else {
            out.push(b[i]);
            i += 1;
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// The mount holding `path` (absolute, canonical): `(mount point, type,
/// source)`. The longest mount point that is a prefix of the path, by whole
/// components; of equal ones the last listed (it mounts over the others).
pub fn mount_of(path: &Path, mountinfo: &str) -> Option<(PathBuf, String, String)> {
    let mut best: Option<(usize, PathBuf, String, String)> = None;
    for line in mountinfo.lines() {
        let Some((left, right)) = line.split_once(" - ") else { continue };
        let lf: Vec<&str> = left.split(' ').collect();
        let rf: Vec<&str> = right.split(' ').collect();
        if lf.len() < 5 || rf.len() < 2 {
            continue;
        }
        let mp = PathBuf::from(unescape(lf[4]));
        if !path.starts_with(&mp) {
            continue;
        }
        let depth = mp.components().filter(|c| matches!(c, Component::Normal(_))).count();
        if best.as_ref().is_none_or(|b| depth >= b.0) {
            best = Some((depth, mp, rf[0].to_string(), unescape(rf[1])));
        }
    }
    best.map(|(_, mp, t, src)| (mp, t, src))
}

/// Checks that `dir` (which must exist) is on a local filesystem; returns
/// its type, or why the store refuses it.
pub fn check_local(dir: &Path) -> Result<String, String> {
    let canon = dir.canonicalize().map_err(|e| format!("{}: {e}", dir.display()))?;
    let info = std::fs::read_to_string("/proc/self/mountinfo").map_err(|e| format!("/proc/self/mountinfo: {e}"))?;
    verdict(&canon, &info)
}

/// [`check_local`]'s decision, given mountinfo's text (testable).
pub fn verdict(canon: &Path, mountinfo: &str) -> Result<String, String> {
    let (mp, fstype, src) =
        mount_of(canon, mountinfo).ok_or_else(|| format!("{}: no mount found in mountinfo", canon.display()))?;
    if LOCAL_TYPES.contains(&fstype.as_str()) {
        Ok(fstype)
    } else {
        Err(format!(
            "{} is on {} ({fstype} from {src}), not a local filesystem; a Raft store must be on one of {:?} \
             (set MAKO_RAFT_DATA_DIR, default /var/tmp/raft-wal-$USER)",
            canon.display(),
            mp.display(),
            LOCAL_TYPES
        ))
    }
}
