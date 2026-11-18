//! Native checkpoint generations. The Raft host persists its snapshot authority
//! before calling publication; CURRENT selects the user-DB generation.
use crate::database::{durability, format_bytes, read_position, required_u64, Generation, FORMAT, GENERATION};
use crate::{mutex, Database, Error, Position, Result};
use mrx_core::Blobs;
use mrx_rocks::RocksBlobs;
use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Arc, atomic::Ordering};

#[derive(Clone, Debug)]
pub struct Checkpoint { pub group: u64, pub position: Position, pub path: PathBuf }
impl Database {
    pub fn checkpoint(&self, destination: &Path) -> Result<Checkpoint> {
        self.ensure_serving()?;
        let _gate = mutex(&self.apply_gate);
        let rocks = self.rocks();
        let position = read_position(&rocks)?;
        rocks.checkpoint(destination)?;
        write_identity(destination, self.group, position, self.sync_writes)?;
        sync_tree(destination)?;
        self.reclaim_generations_locked()?;
        Ok(Checkpoint { group: self.group, position, path: destination.to_owned() })
    }
    pub fn create_checkpoint(&self, destination: &Path, expected: Position) -> Result<()> {
        self.ensure_serving()?;
        let _gate = mutex(&self.apply_gate);
        let rocks = self.rocks();
        if read_position(&rocks)? != expected { return Err(Error::InvalidState("checkpoint position changed")); }
        rocks.checkpoint(destination)?;
        write_identity(destination, self.group, expected, self.sync_writes)?;
        sync_tree(destination)?;
        self.reclaim_generations_locked().map(|_| ())
    }
    /// Copy and fsync a private generation, validating native recovery and its
    /// exact identity. The transport artifact remains immutable and reusable.
    pub fn validate_checkpoint(&self, source: &Path, expected: Position) -> Result<()> {
        let _gate = mutex(&self.apply_gate);
        self.stage_checkpoint_locked(source, expected).map(|_| ())
    }
    fn stage_checkpoint_locked(&self, source: &Path, expected: Position) -> Result<String> {
        validate_identity(source, self.group, expected, self.sync_writes)?;
        let name = format!("install-{}-{}", expected.index, expected.term);
        let target = self.root.join(&name);
        if target.exists() {
            let current = self.generation.read().unwrap_or_else(|error| error.into_inner());
            if current.name == name {
                let actual = read_position(&current.rocks)?;
                if actual.index >= expected.index && (actual.index != expected.index || actual.term == expected.term) { return Ok(name); }
                return Err(Error::Corruption("published generation behind install identity".into()));
            }
            drop(current);
            validate_native(&target, self.group, expected, self.sync_writes)?;
            return Ok(name);
        }
        let temporary = self.root.join(format!("staging-{}-{}", expected.index, expected.term));
        // This path is exclusively our unselected staging generation. A crash
        // during copy leaves it disposable; no CURRENT can ever name staging.
        if temporary.exists() { fs::remove_dir_all(&temporary)?; }
        copy_tree(source, &temporary)?;
        validate_native(&temporary, self.group, expected, self.sync_writes)?;
        sync_tree(&temporary)?;
        fs::rename(&temporary, &target)?;
        sync_directory(&self.root)?;
        Ok(name)
    }
    /// Called after Raft snapshot authority is recorded. Only sync mode promises
    /// power-crash durability; async remains weak even after checkpoint work.
    pub fn publish_checkpoint(&self, source: &Path, expected: Position) -> Result<()> {
        let _gate = mutex(&self.apply_gate);
        let result = (|| {
            validate_identity(source, self.group, expected, self.sync_writes)?;
            let current = self.applied()?;
            if current.index > expected.index { return Ok(()); }
            if current == expected { return Ok(()); }
            if current.index == expected.index { return Err(Error::Corruption("checkpoint term mismatch".into())); }
            let name = self.stage_checkpoint_locked(source, expected)?;
            let rocks = Arc::new(RocksBlobs::open_existing_strict(&self.root.join(&name), durability(self.sync_writes))?);
            if read_position(&rocks)? != expected { return Err(Error::Corruption("staged position changed".into())); }
            let generation = required_u64(rocks.get(GENERATION)?, "checkpoint generation")?;
            let epoch = self.epoch.load(Ordering::Acquire).checked_add(1).ok_or(Error::VersionExhausted)?;
            // Host authority already exists. Persist the selected DB before
            // making it visible to any worker; restart uses this same selector.
            publish_manifest(&self.root, &name)?;
            let old = std::mem::replace(&mut *self.generation.write().unwrap_or_else(|error| error.into_inner()), Generation { name, rocks });
            mutex(&self.retired).push(old);
            self.epoch.store(epoch, Ordering::Release);
            self.range.generation.store(generation, Ordering::Release);
            mutex(&self.metadata).clear();
            self.reclaim_generations_locked()?;
            Ok(())
        })();
        if result.is_err() { self.fence(); }
        result
    }
    /// Reclaim unselected native generations after host recovery or during
    /// checkpoint maintenance. Raft's immutable artifacts live outside this
    /// root and remain recovery authority; a private staged copy can be rebuilt.
    /// Live native snapshots retain their generation's Arc until they close.
    pub fn reclaim_generations(&self) -> Result<usize> {
        let _gate = mutex(&self.apply_gate);
        self.reclaim_generations_locked()
    }
    fn reclaim_generations_locked(&self) -> Result<usize> {
        let current = self.generation.read().unwrap_or_else(|error| error.into_inner());
        let mut retired = mutex(&self.retired);
        // Closing RocksDB precedes unlinking its files. A sole owner here
        // proves that no transaction snapshot or native read handle remains.
        retired.retain(|generation| Arc::strong_count(&generation.rocks) > 1);
        let mut removed = 0;
        for entry in fs::read_dir(&self.root)? {
            let entry = entry?;
            let name = entry.file_name();
            let Some(name) = name.to_str() else { continue };
            if name == current.name || !owned_generation(name)
                || retired.iter().any(|generation| generation.name == name) { continue; }
            if !entry.file_type()?.is_dir() { return Err(Error::Corruption("non-directory generation".into())); }
            fs::remove_dir_all(entry.path())?;
            removed += 1;
        }
        if removed != 0 { sync_directory(&self.root)?; }
        Ok(removed)
    }
}
fn owned_generation(name: &str) -> bool {
    if name == "generation-0" { return true; }
    ["install-", "staging-"].iter().any(|prefix| name.strip_prefix(prefix)
        .and_then(|suffix| suffix.split_once('-'))
        .is_some_and(|(index, term)| index.parse::<u64>().is_ok() && term.parse::<u64>().is_ok()))
}
pub(crate) fn publish_manifest(root: &Path, name: &str) -> Result<()> {
    let temporary = root.join("CURRENT-next");
    let mut file = OpenOptions::new().create(true).truncate(true).write(true).open(&temporary)?;
    file.write_all(name.as_bytes())?;
    file.sync_all()?;
    fs::rename(&temporary, root.join("CURRENT"))?;
    sync_directory(root)
}
fn identity(group: u64, position: Position, sync_writes: bool) -> Vec<u8> {
    let mut bytes = if sync_writes { b"STO-CHECKPOINT-01".to_vec() } else { b"STO-CHECKPOINT-A1".to_vec() };
    bytes.extend_from_slice(&group.to_be_bytes());
    bytes.extend_from_slice(&position.index.to_be_bytes());
    bytes.extend_from_slice(&position.term.to_be_bytes());
    bytes
}
fn write_identity(path: &Path, group: u64, position: Position, sync_writes: bool) -> Result<()> {
    let mut file = OpenOptions::new().create_new(true).write(true).open(path.join("STO-CHECKPOINT"))?;
    file.write_all(&identity(group, position, sync_writes))?;
    file.sync_all()?;
    Ok(())
}
fn validate_identity(path: &Path, group: u64, position: Position, sync_writes: bool) -> Result<()> {
    if fs::read(path.join("STO-CHECKPOINT"))? != identity(group, position, sync_writes) { return Err(Error::Corruption("checkpoint identity or durability mode mismatch".into())); }
    Ok(())
}
fn validate_native(path: &Path, group: u64, position: Position, sync_writes: bool) -> Result<()> {
    validate_identity(path, group, position, sync_writes)?;
    if !path.join("CURRENT").is_file() { return Err(Error::Corruption("missing native checkpoint CURRENT".into())); }
    let rocks = RocksBlobs::open_strict(path, durability(sync_writes))?;
    if rocks.get(FORMAT)?.as_deref() != Some(format_bytes(group, sync_writes).as_slice()) || read_position(&rocks)? != position { return Err(Error::Corruption("native checkpoint identity mismatch".into())); }
    required_u64(rocks.get(GENERATION)?, "checkpoint generation")?;
    Ok(())
}
fn copy_tree(source: &Path, target: &Path) -> Result<()> {
    fs::create_dir(target)?;
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        let kind = entry.file_type()?;
        let destination = target.join(entry.file_name());
        if kind.is_dir() { copy_tree(&entry.path(), &destination)?; }
        else if kind.is_file() { fs::copy(entry.path(), destination)?; }
        else { return Err(Error::Corruption("checkpoint contains non-regular entry".into())); }
    }
    Ok(())
}
fn sync_tree(path: &Path) -> Result<()> {
    for entry in fs::read_dir(path)? {
        let entry = entry?;
        let kind = entry.file_type()?;
        if kind.is_dir() { sync_tree(&entry.path())?; }
        else if kind.is_file() { File::open(entry.path())?.sync_all()?; }
        else { return Err(Error::Corruption("checkpoint contains non-regular entry".into())); }
    }
    sync_directory(path)
}
fn sync_directory(path: &Path) -> Result<()> { File::open(path)?.sync_all()?; Ok(()) }
