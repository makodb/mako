//! Exclusive offline recovery and checkpoint conversion. No listeners, Raft
//! elections or new decisions run here. The existing log lock stays held until
//! the source is dropped. A caller must quiesce the entire replica group before
//! converting engines; this is not a rolling-upgrade protocol.
use crate::{durable::Disk, local, snapshot, Config, LocalConfig, Position, StateMachine};
use raft_store::{
    journal::{decode_command, RocksLog},
    RealFs, Record, SnapRef,
};
use std::path::{Path, PathBuf};

#[derive(Clone)]
pub enum Mode {
    Local(LocalConfig),
    Replicated(Config),
}
impl Mode {
    fn root(&self) -> &Path {
        match self {
            Self::Local(c) => &c.directory,
            Self::Replicated(c) => &c.directory,
        }
    }
    fn group(&self) -> u64 {
        match self {
            Self::Local(c) => c.group,
            Self::Replicated(c) => c.group,
        }
    }
    fn open(&self, destination: Option<&Path>) -> Result<RocksLog, String> {
        match self {
            Self::Local(config) => {
                let mut config = config.clone();
                config.create = destination.is_some();
                if let Some(path) = destination {
                    config.directory = path.into();
                }
                local::open_log(&config)
            }
            Self::Replicated(config) => {
                let mut config = config.clone();
                config.create = destination.is_some();
                if let Some(path) = destination {
                    config.directory = path.into();
                }
                config.peers.sort_by_key(|peer| peer.id);
                if config.peers.is_empty()
                    || !config.peers.iter().any(|peer| peer.id == config.node)
                    || config.node == u16::MAX
                    || config.peers.iter().any(|peer| peer.id == u16::MAX)
                    || config.peers.windows(2).any(|w| w[0].id == w[1].id)
                {
                    return Err("invalid offline Raft membership".into());
                }
                Disk::open(&config).map(Disk::into_log)
            }
        }
    }
}
pub struct Log {
    mode: Mode,
    disk: RocksLog,
}
impl Log {
    pub fn open(mode: Mode) -> Result<Self, String> {
        let disk = mode.open(None)?;
        if matches!(mode, Mode::Local(_))
            && (disk.journal.state.hard.term != 1
                || disk.journal.state.hard.commit != disk.journal.state.last())
        {
            return Err("invalid single-disk log authority".into());
        }
        Ok(Self { mode, disk })
    }
    pub fn committed(&self) -> Position {
        let index = self.disk.journal.state.hard.commit;
        Position {
            index,
            term: self.term(index).expect("validated journal commit"),
        }
    }
    fn term(&self, index: u64) -> Result<u64, String> {
        let state = &self.disk.journal.state;
        if index == state.snap_index {
            return Ok(state.snap_term);
        }
        index
            .checked_sub(state.snap_index + 1)
            .and_then(|i| state.entries.get(i as usize))
            .map(|entry| entry.0)
            .ok_or_else(|| "position outside retained log".into())
    }
    /// Restores the published checkpoint, then only the committed suffix. Saved
    /// prepares/decisions are application data; conversion must preserve them.
    pub fn recover(&self, machine: &dyn StateMachine) -> Result<Position, String> {
        let state = &self.disk.journal.state;
        let mut applied = machine.applied_position()?;
        if let Some(name) = &state.image {
            let bytes = raft_store::images::read(
                &RealFs::with_sync_writes(true),
                &self.mode.root().join("images"),
                name,
            )
            .map_err(|e| e.to_string())?;
            let reference: snapshot::Reference = crate::wire::decode(&bytes)?;
            let expected = Position {
                index: state.snap_index,
                term: state.snap_term,
            };
            if reference.position != expected || reference.group != self.mode.group() {
                return Err("offline snapshot authority mismatch".into());
            }
            let image = snapshot::Image::load(self.mode.root(), reference)?;
            if applied.index <= expected.index {
                machine.validate_checkpoint(&image.directory, expected)?;
                machine.publish_checkpoint(&image.directory, expected)?;
                applied = machine.applied_position()?;
                if applied != expected {
                    return Err("offline checkpoint publication mismatch".into());
                }
            }
        } else if state.snap_index != 0 {
            return Err("missing offline snapshot authority".into());
        }
        if applied.index < state.snap_index
            || applied.index > state.hard.commit
            || applied.term != self.term(applied.index)?
        {
            return Err("application prefix differs from offline log authority".into());
        }
        for index in applied.index + 1..=state.hard.commit {
            let (term, bytes) = &state.entries[(index - state.snap_index - 1) as usize];
            let position = Position { index, term: *term };
            machine.apply(position, decode_command(bytes)?)?;
            if machine.applied_position()? != position {
                return Err("offline replay did not advance exactly".into());
            }
        }
        Ok(self.committed())
    }
    /// The destination must be fresh and private to the converter. It is not
    /// usable until this returns success and the caller publishes its data/config.
    /// Hard state, vote, term and the entire uncommitted suffix stay unchanged.
    pub fn copy_checkpoint(
        &self,
        destination: &Path,
        machine: &dyn StateMachine,
    ) -> Result<Position, String> {
        if destination.exists() {
            return Err("conversion log destination already exists".into());
        }
        let position = self.committed();
        if position.index == 0 || machine.applied_position()? != position {
            return Err("conversion checkpoint must cover the exact committed cut".into());
        }
        let mut target = self.mode.open(Some(destination))?;
        self.disk.journal.copy_to(&mut target.journal)?;
        let image = snapshot::create(destination, self.mode.group(), position, machine, true)?;
        machine.validate_checkpoint(&image.directory, position)?;
        let name = raft_store::images::write(
            &RealFs::with_sync_writes(true),
            &destination.join("images"),
            position.index,
            position.term,
            &crate::wire::encode(&image.reference)?,
        )
        .map_err(|e| e.to_string())?;
        target.journal.persist(Record {
            snapshot: Some(SnapRef {
                index: position.index,
                term: position.term,
                image: Some(name),
                keep: true,
            }),
            ..Record::default()
        })?;
        target.journal.flush()?;
        if target.journal.state.hard != self.disk.journal.state.hard
            || target.journal.state.entries
                != self.disk.journal.state.entries
                    [(position.index - self.disk.journal.state.snap_index) as usize..]
        {
            return Err("conversion changed log authority or its uncommitted suffix".into());
        }
        Ok(position)
    }
    pub fn directory(&self) -> PathBuf {
        self.mode.root().to_owned()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use parking_lot::Mutex;
    use raft_store::{journal::encode_command, Hard};
    use std::{fs, io::Write};
    #[derive(Default)]
    struct Machine(Mutex<(Position, Vec<u8>)>);
    impl StateMachine for Machine {
        fn applied_position(&self) -> Result<Position, String> {
            Ok(self.0.lock().0)
        }
        fn apply(&self, p: Position, bytes: Option<&[u8]>) -> Result<(), String> {
            let mut state = self.0.lock();
            if p.index != state.0.index + 1 {
                return Err("gap".into());
            }
            state.0 = p;
            state.1.extend_from_slice(bytes.unwrap_or_default());
            Ok(())
        }
        fn create_checkpoint(&self, path: &Path, p: Position) -> Result<(), String> {
            let state = self.0.lock();
            if state.0 != p {
                return Err("cut mismatch".into());
            }
            fs::create_dir(path).map_err(|e| e.to_string())?;
            let mut file = fs::File::create(path.join("state")).map_err(|e| e.to_string())?;
            file.write_all(&crate::wire::encode(&*state)?)
                .and_then(|()| file.sync_all())
                .map_err(|e| e.to_string())?;
            fs::File::open(path)
                .and_then(|f| f.sync_all())
                .map_err(|e| e.to_string())
        }
        fn validate_checkpoint(&self, path: &Path, p: Position) -> Result<(), String> {
            let state: (Position, Vec<u8>) =
                crate::wire::decode(&fs::read(path.join("state")).map_err(|e| e.to_string())?)?;
            if state.0 != p {
                return Err("cut mismatch".into());
            }
            Ok(())
        }
        fn publish_checkpoint(&self, path: &Path, p: Position) -> Result<(), String> {
            self.validate_checkpoint(path, p)?;
            *self.0.lock() =
                crate::wire::decode(&fs::read(path.join("state")).map_err(|e| e.to_string())?)?;
            Ok(())
        }
    }
    #[test]
    fn conversion_preserves_raft_vote_term_and_uncommitted_suffix_without_applying_it() {
        let root = std::env::temp_dir().join(format!("raft-offline-{}", std::process::id()));
        fs::create_dir(&root).unwrap();
        let source = root.join("source");
        let config = Config {
            group: 900,
            node: 7,
            directory: source.clone(),
            peers: vec![7, 9, 11]
                .into_iter()
                .map(|id| crate::Peer {
                    id,
                    address: format!("unused:{id}"),
                })
                .collect(),
            ..Default::default()
        };
        let mode = Mode::Replicated(config.clone());
        let mut disk = mode.open(Some(&source)).unwrap();
        let hard = Hard {
            term: 4,
            vote: 9,
            commit: 2,
        };
        let tail = (4, encode_command(Some(b"uncommitted")));
        disk.journal
            .persist(Record {
                hard: Some(hard),
                replace_from: Some(1),
                entries: vec![
                    (2, encode_command(None)),
                    (3, encode_command(Some(b"committed"))),
                    tail.clone(),
                ],
                ..Default::default()
            })
            .unwrap();
        disk.journal.flush().unwrap();
        drop(disk);
        let log = Log::open(mode.clone()).unwrap();
        assert!(
            Log::open(mode).is_err(),
            "source lease excludes live conversion"
        );
        let machine = Machine::default();
        let cut = log.recover(&machine).unwrap();
        assert_eq!(cut, Position { index: 2, term: 3 });
        assert_eq!(machine.0.lock().1, b"committed");
        let target = root.join("target");
        log.copy_checkpoint(&target, &machine).unwrap();
        let converted = Log::open(Mode::Replicated(Config {
            directory: target,
            ..config
        }))
        .unwrap();
        assert_eq!(converted.disk.journal.state.hard, hard);
        assert_eq!(converted.disk.journal.state.entries, vec![tail]);
        let restored = Machine::default();
        assert_eq!(converted.recover(&restored).unwrap(), cut);
        assert_eq!(*restored.0.lock(), *machine.0.lock());
        drop(converted);
        drop(log);
        fs::remove_dir_all(root).unwrap();
    }
}
