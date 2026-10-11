//! Snapshot images as files (design §3 "Snapshots: the file, then the
//! record").
//!
//! An image is written to `<S>-<T>.img.tmp`, synced, renamed to `<S>-<T>.img`
//! and its directory synced; only then is the record that names it queued,
//! so a crash in between leaves the previous snapshot and the longer log.
//! Each file carries a header (magic, index, term, length, CRC32C), so a
//! damaged image fails closed. A `.tmp` names no record and is deleted at
//! recovery; an image older than the one the base names is deleted at a
//! checkpoint.

use std::io;
use std::path::Path;

use crate::bytes::{put_u32, put_u64, Reader};
use crate::crash::crash_point;
use crate::crc::crc32c;
use crate::fs::StoreFs;

const MAGIC: &[u8; 8] = b"MKRFTIMG";
const HEADER: usize = 8 + 8 + 8 + 8 + 4;

pub fn name(index: u64, term: u64) -> String {
    format!("{index}-{term}.img")
}

/// The (index, term) an image name states.
pub fn parse_name(name: &str) -> Option<(u64, u64)> {
    let (s, t) = name.strip_suffix(".img")?.split_once('-')?;
    Some((s.parse().ok()?, t.parse().ok()?))
}

/// Writes the image durably and returns its file name.
pub fn write(fs: &dyn StoreFs, dir: &Path, index: u64, term: u64, bytes: &[u8]) -> io::Result<String> {
    let name = name(index, term);
    let tmp = dir.join(format!("{name}.tmp"));
    if fs.exists(&tmp) {
        fs.remove_file(&tmp)?;
    }
    let mut head = Vec::with_capacity(HEADER);
    head.extend_from_slice(MAGIC);
    put_u64(&mut head, index);
    put_u64(&mut head, term);
    put_u64(&mut head, bytes.len() as u64);
    put_u32(&mut head, crc32c(bytes));
    let mut f = fs.create_new(&tmp)?;
    f.write_all(&head)?;
    f.write_all(bytes)?;
    f.sync_data()?;
    drop(f);
    // rename replaces an existing image of the same name atomically.
    fs.rename(&tmp, &dir.join(&name))?;
    crash_point("image.rename");
    fs.sync_dir(dir)?;
    crash_point("image.dirsync");
    Ok(name)
}

/// Reads and checks an image: its bytes, or why it is not the named one.
pub fn read(fs: &dyn StoreFs, dir: &Path, name: &str) -> Result<Vec<u8>, String> {
    let (index, term) = parse_name(name).ok_or_else(|| format!("{name}: not an image name"))?;
    let path = dir.join(name);
    let mut b = fs.read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    if b.len() < HEADER || &b[..8] != MAGIC {
        return Err(format!("{}: not an image", path.display()));
    }
    let mut r = Reader::new(&b[8..HEADER]);
    let (i, t, len, crc) = (r.u64()?, r.u64()?, r.u64()?, r.u32()?);
    let body = &b[HEADER..];
    if (i, t) != (index, term) || body.len() as u64 != len || crc32c(body) != crc {
        return Err(format!("{}: damaged image", path.display()));
    }
    b.drain(..HEADER);
    Ok(b)
}

/// Deletes every `.tmp` (recovery) and every image below `keep_index`.
pub fn cleanup(fs: &dyn StoreFs, dir: &Path, keep_index: u64, tmp_too: bool) -> io::Result<usize> {
    let mut n = 0;
    for f in fs.list(dir)? {
        let doomed = (tmp_too && f.ends_with(".tmp")) || parse_name(&f).is_some_and(|(s, _)| s < keep_index);
        if doomed {
            crash_point("image.delete");
            fs.remove_file(&dir.join(&f))?;
            n += 1;
        }
    }
    if n > 0 {
        fs.sync_dir(dir)?;
    }
    Ok(n)
}
