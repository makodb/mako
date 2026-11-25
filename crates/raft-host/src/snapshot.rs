use crate::{Position, StateMachine};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::Arc;

pub(crate) const MANIFEST: &str = "RAFT-HOST-MANIFEST";
#[derive(Clone, Serialize, Deserialize)]
pub(crate) struct FileEntry {
    pub name: String,
    pub length: u64,
    pub digest: [u8; 32],
}
#[derive(Clone, Serialize, Deserialize)]
pub(crate) struct Manifest {
    pub group: u64,
    pub position: Position,
    pub files: Vec<FileEntry>,
}
#[derive(Clone, Serialize, Deserialize)]
pub(crate) struct Reference {
    pub group: u64,
    pub position: Position,
    pub generation: String,
    pub digest: [u8; 32],
}
#[derive(Clone)]
pub(crate) struct Image {
    pub reference: Reference,
    pub directory: PathBuf,
    lease: Arc<()>,
}
impl Image {
    pub fn exclusively_retained(&self) -> bool {
        Arc::strong_count(&self.lease) == 1
    }
    pub fn load(root: &Path, reference: Reference) -> Result<Self, String> {
        safe_name(&reference.generation)?;
        let image = Self {
            directory: root.join("checkpoints").join(&reference.generation),
            reference,
            lease: Arc::new(()),
        };
        image.validate()?;
        Ok(image)
    }
    pub fn manifest(&self) -> Result<Manifest, String> {
        let (_, digest) = hash_file(&self.directory.join(MANIFEST))?;
        if digest != self.reference.digest {
            return Err("checkpoint manifest digest mismatch".into());
        }
        let manifest: Manifest =
            bincode::deserialize_from(File::open(self.directory.join(MANIFEST)).map_err(err)?)
                .map_err(err)?;
        if manifest.group != self.reference.group || manifest.position != self.reference.position {
            return Err("checkpoint group/position mismatch".into());
        }
        Ok(manifest)
    }
    pub fn validate(&self) -> Result<Manifest, String> {
        let manifest = self.manifest()?;
        let mut names = std::collections::BTreeSet::new();
        for entry in &manifest.files {
            safe_name(&entry.name)?;
            if entry.name == MANIFEST || !names.insert(entry.name.clone()) {
                return Err("duplicate/reserved checkpoint file".into());
            }
            let actual = hash_file(&self.directory.join(&entry.name))?;
            if actual != (entry.length, entry.digest) {
                return Err(format!("checkpoint file {} corrupt", entry.name));
            }
        }
        let mut actual_names = std::collections::BTreeSet::new();
        inventory(&self.directory, &self.directory, &mut actual_names)?;
        actual_names.remove(MANIFEST);
        if actual_names != names {
            return Err("checkpoint has unmanifested files".into());
        }
        Ok(manifest)
    }
}
pub(crate) fn create(
    root: &Path,
    group: u64,
    position: Position,
    machine: &dyn StateMachine,
    sync_writes: bool,
) -> Result<Image, String> {
    let checkpoints = root.join("checkpoints");
    fs::create_dir_all(&checkpoints).map_err(err)?;
    if sync_writes {
        sync_dir(root)?;
    }
    // A failed attempt may leave a complete or partial unpublished directory.
    // Give every attempt its own name so recovery can retry the same cut without
    // deleting an artifact that might still be referenced by durable authority.
    let mut nonce = [0u8; 16];
    File::open("/dev/urandom")
        .and_then(|mut file| file.read_exact(&mut nonce))
        .map_err(err)?;
    let generation = format!(
        "{}-{}-{:x}",
        position.index,
        position.term,
        u128::from_ne_bytes(nonce)
    );
    let directory = checkpoints.join(&generation);
    if directory.exists() {
        return Err("checkpoint generation already exists".into());
    }
    machine.create_checkpoint(&directory, position)?;
    let mut files = Vec::new();
    collect(&directory, &directory, &mut files)?;
    files.sort_by(|a, b| a.name.cmp(&b.name));
    let manifest = Manifest {
        group,
        position,
        files,
    };
    let mut file = OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(directory.join(MANIFEST))
        .map_err(err)?;
    bincode::serialize_into(&mut file, &manifest).map_err(err)?;
    if sync_writes {
        file.sync_all().map_err(err)?;
        sync_tree(&directory)?;
        sync_dir(&checkpoints)?;
    }
    let (_, digest) = hash_file(&directory.join(MANIFEST))?;
    Ok(Image {
        directory,
        reference: Reference {
            group,
            position,
            generation,
            digest,
        },
        lease: Arc::new(()),
    })
}
fn collect(root: &Path, directory: &Path, files: &mut Vec<FileEntry>) -> Result<(), String> {
    for entry in fs::read_dir(directory).map_err(err)? {
        let entry = entry.map_err(err)?;
        let ty = entry.file_type().map_err(err)?;
        if ty.is_dir() {
            collect(root, &entry.path(), files)?;
        } else if ty.is_file() {
            let path = entry.path();
            let name = path
                .strip_prefix(root)
                .map_err(err)?
                .to_str()
                .ok_or("non UTF-8 checkpoint filename")?
                .to_owned();
            safe_name(&name)?;
            if name == MANIFEST {
                return Err("reserved checkpoint filename".into());
            }
            let (length, digest) = hash_file(&path)?;
            files.push(FileEntry {
                name,
                length,
                digest,
            });
        } else {
            return Err("checkpoint contains symlink or special file".into());
        }
    }
    Ok(())
}
pub(crate) fn safe_name(name: &str) -> Result<(), String> {
    if name.is_empty()
        || name.len() > 4096
        || !Path::new(name)
            .components()
            .all(|c| matches!(c, Component::Normal(_)))
    {
        return Err("invalid checkpoint relative filename".into());
    }
    Ok(())
}
pub(crate) fn hash_file(path: &Path) -> Result<(u64, [u8; 32]), String> {
    if !fs::symlink_metadata(path).map_err(err)?.is_file() {
        return Err("checkpoint path is not a regular file".into());
    }
    let mut file = File::open(path).map_err(err)?;
    let mut buffer = vec![0u8; 1 << 20];
    let mut digest = Sha256::new();
    let mut length = 0u64;
    loop {
        let count = file.read(&mut buffer).map_err(err)?;
        if count == 0 {
            break;
        }
        digest.update(&buffer[..count]);
        length = length
            .checked_add(count as u64)
            .ok_or("checkpoint file length overflow")?;
    }
    Ok((length, digest.finalize().into()))
}
struct ReceivingFile {
    name: String,
    handle: File,
    written: u64,
    digest: Sha256,
}
pub(crate) struct Incoming {
    pub image: Image,
    open_file: Option<ReceivingFile>,
    received: std::collections::BTreeMap<String, (u64, [u8; 32])>,
}
impl Incoming {
    pub fn begin(
        root: &Path,
        group: u64,
        position: Position,
        digest: [u8; 32],
    ) -> Result<Self, String> {
        let checkpoints = root.join("checkpoints");
        fs::create_dir_all(&checkpoints).map_err(err)?;
        let generation = format!("incoming-{}-{}", position.index, position.term);
        let directory = checkpoints.join(&generation);
        // The group never starts a transfer at/below its authoritative snapshot.
        // This path is therefore private and unreferenced, including after restart.
        if directory.exists() {
            fs::remove_dir_all(&directory).map_err(err)?;
        }
        fs::create_dir(&directory).map_err(err)?;
        Ok(Self {
            image: Image {
                directory,
                reference: Reference {
                    group,
                    position,
                    generation,
                    digest,
                },
                lease: Arc::new(()),
            },
            open_file: None,
            received: std::collections::BTreeMap::new(),
        })
    }
    pub fn chunk(
        &mut self,
        file: String,
        offset: u64,
        bytes: &[u8],
        last: bool,
    ) -> Result<(), String> {
        safe_name(&file)?;
        if self.open_file.is_none() {
            if offset != 0 {
                return Err("checkpoint chunk starts with a gap".into());
            }
            let path = self.image.directory.join(&file);
            fs::create_dir_all(path.parent().ok_or("checkpoint file missing parent")?)
                .map_err(err)?;
            let handle = OpenOptions::new()
                .create_new(true)
                .read(true)
                .write(true)
                .open(path)
                .map_err(err)?;
            self.open_file = Some(ReceivingFile {
                name: file.clone(),
                handle,
                written: 0,
                digest: Sha256::new(),
            });
        }
        let current = self.open_file.as_mut().unwrap();
        if current.name != file || current.written != offset {
            return Err("out-of-order checkpoint chunk".into());
        }
        current.handle.write_all(bytes).map_err(err)?;
        current.digest.update(bytes);
        current.written = current
            .written
            .checked_add(bytes.len() as u64)
            .ok_or("checkpoint length overflow")?;
        if last {
            let completed = self.open_file.take().unwrap();
            self.received.insert(
                completed.name,
                (completed.written, completed.digest.finalize().into()),
            );
            // Chunk acknowledgements mean volatile transfer progress, never
            // durable snapshot installation. Flush the entire private generation
            // in Finish, not on a final-chunk RPC with a heartbeat-sized deadline.
        }
        Ok(())
    }
    fn validate_received(&self) -> Result<(), String> {
        if self.open_file.is_some() {
            return Err("unfinished checkpoint file".into());
        }
        if self.received.get(MANIFEST).map(|entry| entry.1) != Some(self.image.reference.digest) {
            return Err("checkpoint manifest missing or corrupt".into());
        }
        let manifest = self.image.manifest()?;
        let mut names = std::collections::BTreeSet::new();
        for entry in &manifest.files {
            safe_name(&entry.name)?;
            if entry.name == MANIFEST || !names.insert(entry.name.clone()) {
                return Err("duplicate/reserved checkpoint file".into());
            }
            if self.received.get(&entry.name) != Some(&(entry.length, entry.digest)) {
                return Err(format!("checkpoint file {} missing or corrupt", entry.name));
            }
            let metadata =
                fs::symlink_metadata(self.image.directory.join(&entry.name)).map_err(err)?;
            if !metadata.is_file() || metadata.len() != entry.length {
                return Err(format!(
                    "checkpoint file {} changed during staging",
                    entry.name
                ));
            }
        }
        let mut actual = std::collections::BTreeSet::new();
        inventory(&self.image.directory, &self.image.directory, &mut actual)?;
        actual.remove(MANIFEST);
        if actual != names || self.received.len() != names.len() + 1 {
            return Err("checkpoint has missing or unmanifested files".into());
        }
        Ok(())
    }
    pub fn finish(self, machine: &dyn StateMachine, sync_writes: bool) -> Result<Image, String> {
        // Files remain private and have one writer. Hashing each received
        // chunk avoids a second whole-checkpoint read on the staging RPC.
        // Recovery still independently hashes every persisted image via load().
        self.validate_received()?;
        if sync_writes {
            sync_tree(&self.image.directory)?;
            sync_dir(
                self.image
                    .directory
                    .parent()
                    .ok_or("missing checkpoint root")?,
            )?;
        }
        machine.validate_checkpoint(&self.image.directory, self.image.reference.position)?;
        Ok(self.image)
    }
}
/// Send one file with a fixed memory bound, even when it exceeds the RPC frame
/// limit or the machine's address space. An empty file still gets a final chunk.
pub(crate) fn stream_file(
    path: &Path,
    chunk_bytes: usize,
    mut send: impl FnMut(u64, Vec<u8>, bool) -> Result<(), String>,
) -> Result<(), String> {
    let mut file = File::open(path).map_err(err)?;
    let length = file.seek(SeekFrom::End(0)).map_err(err)?;
    file.seek(SeekFrom::Start(0)).map_err(err)?;
    let mut offset = 0;
    loop {
        let count = (length - offset).min(chunk_bytes as u64) as usize;
        let mut bytes = vec![0u8; count];
        file.read_exact(&mut bytes).map_err(err)?;
        let end = offset + count as u64;
        send(offset, bytes, end == length)?;
        offset = end;
        if offset == length {
            return Ok(());
        }
    }
}
pub(crate) fn cleanup(root: &Path, keep: Option<&Image>, sync_writes: bool) -> Result<(), String> {
    let directory = root.join("checkpoints");
    if !directory.exists() {
        return Ok(());
    }
    for entry in fs::read_dir(&directory).map_err(err)? {
        let path = entry.map_err(err)?.path();
        if keep.is_none_or(|image| image.directory != path) {
            fs::remove_dir_all(path).map_err(err)?;
        }
    }
    if sync_writes {
        sync_dir(&directory)?;
    }
    Ok(())
}
fn sync_tree(directory: &Path) -> Result<(), String> {
    for entry in fs::read_dir(directory).map_err(err)? {
        let entry = entry.map_err(err)?;
        if entry.file_type().map_err(err)?.is_dir() {
            sync_tree(&entry.path())?;
        } else {
            File::open(entry.path())
                .map_err(err)?
                .sync_all()
                .map_err(err)?;
        }
    }
    sync_dir(directory)
}
fn sync_dir(path: &Path) -> Result<(), String> {
    File::open(path).map_err(err)?.sync_all().map_err(err)
}
fn err(e: impl std::fmt::Display) -> String {
    e.to_string()
}

fn inventory(
    root: &Path,
    directory: &Path,
    names: &mut std::collections::BTreeSet<String>,
) -> Result<(), String> {
    for entry in fs::read_dir(directory).map_err(err)? {
        let entry = entry.map_err(err)?;
        let ty = entry.file_type().map_err(err)?;
        if ty.is_dir() {
            inventory(root, &entry.path(), names)?;
        } else if ty.is_file() {
            names.insert(
                entry
                    .path()
                    .strip_prefix(root)
                    .map_err(err)?
                    .to_str()
                    .ok_or("non UTF-8 checkpoint path")?
                    .to_owned(),
            );
        } else {
            return Err("checkpoint contains symlink or special file".into());
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};
    static NEXT: AtomicU64 = AtomicU64::new(0);
    struct Directory(PathBuf);
    impl Directory {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "raft-checkpoint-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }
    }
    impl Drop for Directory {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    fn staged(root: &Path) -> Incoming {
        let position = Position {
            index: 1u64 << 40,
            term: 1u64 << 35,
        };
        let manifest = Manifest {
            group: 1u64 << 50,
            position,
            files: vec![FileEntry {
                name: "table.sst".into(),
                length: 4,
                digest: Sha256::digest(b"rows").into(),
            }],
        };
        let bytes = bincode::serialize(&manifest).unwrap();
        let digest = Sha256::digest(&bytes).into();
        let mut incoming = Incoming::begin(root, manifest.group, position, digest).unwrap();
        incoming.chunk(MANIFEST.into(), 0, &bytes, true).unwrap();
        incoming
    }
    #[test]
    fn corrupt_or_unmanifested_files_never_become_eligible_for_publication() {
        let root = Directory::new();
        let mut incoming = staged(&root.0);
        incoming
            .chunk("table.sst".into(), 0, b"rows", true)
            .unwrap();
        assert!(incoming.image.validate().is_ok());
        fs::write(incoming.image.directory.join("extra.sst"), b"untrusted").unwrap();
        assert!(incoming.image.validate().is_err());
        fs::remove_file(incoming.image.directory.join("extra.sst")).unwrap();
        fs::write(incoming.image.directory.join("table.sst"), b"evil").unwrap();
        assert!(incoming.image.validate().is_err());
    }
    #[test]
    fn incremental_file_hashes_validate_streamed_contents_and_inventory() {
        let root = Directory::new();
        let mut incoming = staged(&root.0);
        assert!(incoming.validate_received().is_err());
        incoming.chunk("table.sst".into(), 0, b"ro", false).unwrap();
        assert!(incoming.validate_received().is_err());
        incoming.chunk("table.sst".into(), 2, b"ws", true).unwrap();
        assert!(incoming.validate_received().is_ok());
        fs::write(incoming.image.directory.join("extra.sst"), b"unmanifested").unwrap();
        assert!(incoming.validate_received().is_err());
        drop(incoming);
        let mut corrupt = staged(&root.0);
        corrupt.chunk("table.sst".into(), 0, b"evil", true).unwrap();
        assert!(corrupt.validate_received().is_err());
    }
    #[test]
    fn chunk_protocol_rejects_gaps_reordering_and_path_escape() {
        let root = Directory::new();
        let mut incoming = staged(&root.0);
        assert!(incoming.chunk("../outside".into(), 0, b"x", true).is_err());
        assert!(incoming
            .chunk("table.sst".into(), 1, b"rows", true)
            .is_err());
        incoming.chunk("table.sst".into(), 0, b"ro", false).unwrap();
        assert!(incoming.chunk("table.sst".into(), 0, b"ro", false).is_err());
        assert!(incoming.chunk("other.sst".into(), 2, b"ws", true).is_err());
        incoming.chunk("table.sst".into(), 2, b"ws", true).unwrap();
        assert!(incoming.image.validate().is_ok());
        let stream = incoming.image.clone();
        assert!(!incoming.image.exclusively_retained());
        drop(stream);
        assert!(incoming.image.exclusively_retained());
    }
}
