use crate::{
    wire::{Command, CommandCodec},
    Config, Position,
};
use raft_core::{CoreOutput, RaftCore};
use raft_store::{
    journal::RocksLog, Codec, Hard, Identity, RealFs, Record, SavedState, SnapRef, StoreFs,
};
use std::{path::PathBuf, sync::Arc};

pub(crate) struct Disk {
    pub sequence: u64,
    pub state: SavedState<Command>,
    pub fs: Arc<dyn StoreFs>,
    pub path: PathBuf,
    log: RocksLog,
}
impl Disk {
    pub(crate) fn into_log(self) -> RocksLog {
        self.log
    }
    pub fn open(cfg: &Config) -> Result<Self, String> {
        let fs: Arc<dyn StoreFs> = Arc::new(RealFs::with_sync_writes(cfg.sync_writes));
        let identity = Identity {
            site: cfg.node as u32,
            partition: cfg.group as u32,
            fingerprint: fingerprint(cfg),
            format: 0x524c_0001,
        };
        let mut authority = format!(
            "raft-host/2\ngroup={}\nnode={}\nmembers={:?}\n",
            cfg.group,
            cfg.node,
            cfg.peers.iter().map(|peer| peer.id).collect::<Vec<_>>()
        );
        if cfg.initial_state != [0;32] {
            authority.push_str(&format!("initial_state={:02x?}\n", cfg.initial_state));
        }
        let log = RocksLog::open(
            &cfg.directory,
            identity,
            authority.as_bytes(),
            cfg.create,
            cfg.sync_writes,
        )?;
        let raw = &log.journal.state;
        let mut state = SavedState::new(u16::MAX);
        state.hard = raw.hard;
        state.snap_index = raw.snap_index;
        state.snap_term = raw.snap_term;
        state.image = raw.image.clone();
        for (term, bytes) in &raw.entries {
            state.entries.push((*term, CommandCodec.decode(bytes)?));
        }
        Ok(Self {
            sequence: log.journal.sequence,
            state,
            fs,
            path: cfg.directory.clone(),
            log,
        })
    }
    pub fn persist(
        &mut self,
        core: &RaftCore<Command>,
        output: &mut CoreOutput,
        image: Option<String>,
    ) -> Result<(), String> {
        let Some(note) = output.take_persist() else {
            return Ok(());
        };
        let mut record = Record::default();
        if note.hard_ {
            record.hard = Some(Hard {
                term: note.term_,
                vote: note.vote_,
                commit: note.commit_,
            });
        }
        if let Some(s) = note.snap_ {
            record.snapshot = Some(SnapRef {
                index: s.index_,
                term: s.term_ as u64,
                image: Some(image.ok_or("snapshot lacks durable manifest reference")?),
                keep: s.keep_,
            });
        }
        if note.log_from_ != 0 {
            record.replace_from = Some(note.log_from_);
            for i in note.log_from_..=core.raft_log_.last_index() {
                let entry = core
                    .raft_log_
                    .get(i)
                    .ok_or("missing log entry in persistence note")?;
                record
                    .entries
                    .push((entry.term() as u64, entry.cmd().clone()));
            }
        }
        let mut bytes = Vec::new();
        raft_store::record::encode(&record, &CommandCodec, &mut bytes);
        let raw = raft_store::record::decode(&bytes, &raft_store::BytesCodec)?;
        self.log.journal.persist(raw)?;
        self.sequence = self.log.journal.sequence;
        self.state.apply(record)?;
        Ok(())
    }
    pub fn save_image(&self, position: Position, reference: &[u8]) -> Result<String, String> {
        raft_store::images::write(
            &*self.fs,
            &self.path.join("images"),
            position.index,
            position.term,
            reference,
        )
        .map_err(|error| error.to_string())
    }
    pub fn checkpoint_base(&mut self) -> Result<(), String> {
        self.log.journal.flush()
    }
}
fn fingerprint(cfg: &Config) -> u64 {
    let mut h = 0xcbf29ce484222325u64;
    for byte in cfg
        .group
        .to_le_bytes()
        .into_iter()
        .chain(cfg.peers.iter().flat_map(|p| p.id.to_le_bytes()))
    {
        h = (h ^ byte as u64).wrapping_mul(0x100000001b3);
    }
    if cfg.initial_state != [0;32] {
        for byte in cfg.initial_state { h = (h ^ u64::from(byte)).wrapping_mul(0x100000001b3); }
    }
    h
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn persistent_store_refuses_durability_mode_changes() {
        let root = std::env::temp_dir().join(format!(
            "raft-host-policy-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&root).unwrap();
        for sync_writes in [true, false] {
            let mut cfg = Config {
                group: 77,
                node: 1,
                peers: vec![crate::Peer {
                    id: 1,
                    address: "127.0.0.1:1".into(),
                }],
                directory: root.join(if sync_writes { "sync" } else { "async" }),
                create: true,
                sync_writes,
                ..Config::default()
            };
            drop(Disk::open(&cfg).unwrap());
            cfg.create = false;
            drop(Disk::open(&cfg).unwrap());
            cfg.sync_writes = !sync_writes;
            assert!(
                Disk::open(&cfg).is_err(),
                "a store was silently opened under a different durability policy"
            );
            cfg.sync_writes = sync_writes;
            drop(Disk::open(&cfg).unwrap());
            cfg.initial_state = [7;32];
            assert!(Disk::open(&cfg).is_err(), "initial application identity changed on reopen");
            cfg.directory = root.join(if sync_writes { "seeded-sync" } else { "seeded-async" });
            cfg.create = true;
            drop(Disk::open(&cfg).unwrap());
            cfg.initial_state = [8;32];
            assert!(Disk::open(&cfg).is_err(), "different adopted contents reused a journal");
        }
        std::fs::remove_dir_all(root).unwrap();
    }
}
