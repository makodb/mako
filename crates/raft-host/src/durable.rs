use crate::{wire::{Command, CommandCodec}, Config, Position};
use raft_core::{CoreOutput, RaftCore};
use raft_store::{base::{self, Base}, Hard, Identity, RealFs, Record, SavedState, SnapRef, StoreFs, Wal, WalOptions};
use std::any::Any;
use std::path::PathBuf;
use std::sync::Arc;

pub(crate) struct Disk {
    pub wal: Wal,
    pub sequence: u64,
    pub state: SavedState<Command>,
    pub fs: Arc<dyn StoreFs>,
    pub path: PathBuf,
    base: Box<dyn Base>,
    top: u64,
    _lock: Box<dyn Any + Send>,
}
impl Disk {
    pub fn open(cfg: &Config) -> Result<Self, String> {
        let fs: Arc<dyn StoreFs> = Arc::new(RealFs::with_sync_writes(cfg.sync_writes));
        // Both halves of the full-width group identity participate in WAL
        // identity. A separate immutable identity file records exact membership.
        let identity = Identity { site: cfg.node as u32, partition: cfg.group as u32,
            fingerprint: fingerprint(cfg), format: if cfg.sync_writes { 0x5248_0001 } else { 0x5248_0002 } };
        let sync_writes = cfg.sync_writes;
        let factory = move |p: &std::path::Path, create| -> std::io::Result<Box<dyn Base>> {
            Ok(Box::new(raft_store::rocks::RocksBase::open_with_sync_writes(p, create, sync_writes)?))
        };
        let opened = raft_store::open_store_with_base(fs.clone(), &cfg.directory, identity,
            WalOptions::default(), cfg.create, &CommandCodec, u16::MAX, Some(&factory))?;
        let legacy = format!("raft-host/1\ngroup={}\nnode={}\nmembers={:?}\n", cfg.group, cfg.node,
            cfg.peers.iter().map(|p| p.id).collect::<Vec<_>>());
        let exact = format!("{legacy}durability={}\n", if cfg.sync_writes { "sync" } else { "async" });
        let identity_path = cfg.directory.join("HOST_IDENTITY");
        if opened.created && !identity_path.exists() {
            let mut file = fs.create_new(&identity_path).map_err(|e| e.to_string())?;
            file.write_all(exact.as_bytes()).and_then(|_| file.sync_data()).map_err(|e| e.to_string())?;
            fs.sync_dir(&cfg.directory).map_err(|e| e.to_string())?;
        } else {
            let stored = fs.read(&identity_path).map_err(|e| e.to_string())?;
            // Pre-policy stores were always synchronous. Never accept their
            // identity in async mode, or an async identity in sync mode.
            if stored != exact.as_bytes() && !(cfg.sync_writes && stored == legacy.as_bytes()) {
                return Err("Raft host identity/membership/durability mismatch".into());
            }
        }
        let mut disk = Self { wal: opened.wal, sequence: opened.d, top: u64::MAX,
            state: opened.state, base: opened.base.expect("opened with a base"), fs,
            path: cfg.directory.clone(), _lock: opened.lock };
        // Fold recovered records that had reached WAL but not the base before
        // the crash. Async base reclamation is protected by its unsynced WAL;
        // sync mode requires completed durable base flushes before reclamation.
        let recovered = raft_store::wal::recover(&*disk.fs, &disk.path.join("wal"), &identity, opened.c)?;
        for (seq, bytes) in recovered.records { disk.fold(seq, &bytes)?; }
        Ok(disk)
    }
    fn fold(&mut self, sequence: u64, bytes: &[u8]) -> Result<(), String> {
        let raw = raft_store::record::decode(bytes, &raft_store::BytesCodec)?;
        let mut ops = Vec::new();
        base::ops_for(&raw, sequence, &mut self.top, &mut ops);
        self.base.write(&ops).map_err(|e| e.to_string())
    }
    pub fn persist(&mut self, core: &RaftCore<Command>, output: &mut CoreOutput,
        image: Option<String>) -> Result<(), String> {
        let Some(note) = output.take_persist() else { return Ok(()); };
        let mut record = Record::default();
        if note.hard_ { record.hard = Some(Hard { term: note.term_, vote: note.vote_, commit: note.commit_ }); }
        if let Some(s) = note.snap_ {
            record.snapshot = Some(SnapRef { index: s.index_, term: s.term_ as u64,
                image: Some(image.ok_or("snapshot lacks durable manifest reference")?), keep: s.keep_ });
        }
        if note.log_from_ != 0 {
            record.replace_from = Some(note.log_from_);
            for i in note.log_from_..=core.raft_log_.last_index() {
                let entry = core.raft_log_.get(i).ok_or("missing log entry in persistence note")?;
                record.entries.push((entry.term() as u64, entry.cmd().clone()));
            }
        }
        let mut bytes = Vec::new();
        raft_store::record::encode(&record, &CommandCodec, &mut bytes);
        let sequence = self.sequence.checked_add(1).ok_or("WAL sequence exhausted")?;
        self.wal.append(sequence, std::slice::from_ref(&bytes)).map_err(|e| e.to_string())?;
        self.sequence = sequence;
        self.state.apply(record)?;
        self.fold(sequence, &bytes)?;
        Ok(())
    }
    pub fn save_image(&self, position: Position, reference: &[u8]) -> Result<String, String> {
        raft_store::images::write(&*self.fs, &self.path.join("images"), position.index, position.term, reference)
            .map_err(|e| e.to_string())
    }
    pub fn checkpoint_base(&mut self) -> Result<(), String> {
        self.base.flush().map_err(|e| e.to_string())?;
        let directory = self.path.join("wal");
        for segment in raft_store::wal::covered_segments(&*self.fs, &directory, self.sequence, self.wal.rotated().get())
            .map_err(|e| e.to_string())? {
            self.fs.remove_file(&segment).map_err(|e| e.to_string())?;
        }
        self.fs.sync_dir(&directory).map_err(|e| e.to_string())
    }
}
fn fingerprint(cfg: &Config) -> u64 {
    let mut h = 0xcbf29ce484222325u64;
    for byte in cfg.group.to_le_bytes().into_iter().chain(cfg.peers.iter().flat_map(|p| p.id.to_le_bytes())) {
        h = (h ^ byte as u64).wrapping_mul(0x100000001b3);
    }
    h
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn persistent_store_refuses_durability_mode_changes() {
        let root = std::env::temp_dir().join(format!("raft-host-policy-{}-{}",
            std::process::id(), std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()));
        std::fs::create_dir(&root).unwrap();
        for sync_writes in [true, false] {
            let mut cfg = Config { group: 77, node: 1,
                peers: vec![crate::Peer { id: 1, address: "127.0.0.1:1".into() }],
                directory: root.join(if sync_writes { "sync" } else { "async" }),
                create: true, sync_writes, ..Config::default() };
            drop(Disk::open(&cfg).unwrap());
            cfg.create = false;
            drop(Disk::open(&cfg).unwrap());
            cfg.sync_writes = !sync_writes;
            assert!(Disk::open(&cfg).is_err(), "a store was silently opened under a different durability policy");
            cfg.sync_writes = sync_writes;
            drop(Disk::open(&cfg).unwrap());
        }
        std::fs::remove_dir_all(root).unwrap();
    }
}
