//! Single-disk ordering over the same RocksDB log used by the Raft driver.
use crate::{AdmissionError, HostError, Position, Proposal, ProposalStatus, StateMachine, Status};
use parking_lot::{Condvar, Mutex, RwLock};
use raft_store::{
    journal::{decode_command, encode_command, RocksLog},
    Hard, Identity, RealFs, Record, SnapRef,
};
use std::{
    path::PathBuf,
    sync::{atomic::AtomicBool, Arc},
    time::Duration,
};

#[derive(Clone, Debug)]
pub struct LocalConfig {
    pub group: u64,
    pub directory: PathBuf,
    pub create: bool,
    pub sync_writes: bool,
}
struct Inner {
    disk: Mutex<RocksLog>,
    status: RwLock<Status>,
    machine: Arc<dyn StateMachine>,
    directory: PathBuf,
    group: u64,
}
#[derive(Clone)]
pub struct LocalHost(Arc<Inner>);
impl LocalHost {
    pub fn start(config: LocalConfig, machine: Arc<dyn StateMachine>) -> Result<Self, HostError> {
        let storage = HostError::Storage;
        let id = Identity {
            site: 0,
            partition: config.group as u32,
            fingerprint: config.group,
            format: 0x524c_0001,
        };
        let authority = format!("local-log/1\ngroup={}\n", config.group);
        let mut disk = RocksLog::open(
            &config.directory,
            id,
            authority.as_bytes(),
            config.create,
            config.sync_writes,
        )
        .map_err(storage)?;
        let mut applied = machine.applied_position().map_err(storage)?;
        if disk.journal.sequence == 0 {
            if applied != Position::default() {
                return Err(storage("applied data exists without its local command log; offline conversion required".into()));
            }
            disk.journal
                .persist(Record {
                    hard: Some(Hard {
                        term: 1,
                        vote: u16::MAX,
                        commit: 1,
                    }),
                    replace_from: Some(1),
                    entries: vec![(1, encode_command(None))],
                    ..Record::default()
                })
                .map_err(storage)?;
        }
        let state = &disk.journal.state;
        if let Some(name) = &state.image {
            let fs = RealFs::with_sync_writes(config.sync_writes);
            let bytes = raft_store::images::read(&fs, &config.directory.join("images"), name)
                .map_err(storage)?;
            let reference: crate::snapshot::Reference =
                crate::wire::decode(&bytes).map_err(storage)?;
            let expected = Position {
                index: state.snap_index,
                term: state.snap_term,
            };
            if reference.group != config.group || reference.position != expected {
                return Err(storage("local snapshot authority mismatch".into()));
            }
            let image =
                crate::snapshot::Image::load(&config.directory, reference).map_err(storage)?;
            if applied.index <= expected.index {
                machine
                    .validate_checkpoint(&image.directory, expected)
                    .map_err(storage)?;
                machine
                    .publish_checkpoint(&image.directory, expected)
                    .map_err(storage)?;
                applied = machine.applied_position().map_err(storage)?;
                if applied != expected {
                    return Err(storage("local checkpoint publication mismatch".into()));
                }
            }
        } else if state.snap_index != 0 {
            return Err(storage(
                "local snapshot has no application checkpoint".into(),
            ));
        }
        if state.hard.term != 1
            || state.hard.commit != state.last()
            || applied.index < state.snap_index
            || applied.index > state.hard.commit
            || (applied.index == 0 && applied.term != 0)
            || (applied.index > 0 && applied.term != 1)
        {
            return Err(storage(
                "local applied prefix disagrees with committed authority".into(),
            ));
        }
        for index in applied.index + 1..=state.hard.commit {
            let position = Position { index, term: 1 };
            let command = decode_command(&state.entries[(index - state.snap_index - 1) as usize].1)
                .map_err(storage)?;
            machine.apply(position, command).map_err(storage)?;
            if machine.applied_position().map_err(storage)? != position {
                return Err(storage(
                    "local application did not advance its prefix".into(),
                ));
            }
            applied = position;
        }
        let status = Status {
            term: 1,
            leader: Some(0),
            is_leader: true,
            committed: applied,
            applied,
            durable_sequence: disk.journal.sequence,
            failure: None,
        };
        Ok(Self(Arc::new(Inner {
            disk: Mutex::new(disk),
            status: RwLock::new(status),
            machine,
            directory: config.directory,
            group: config.group,
        })))
    }
    pub fn status(&self) -> Status {
        self.0.status.read().clone()
    }
    /// Publish a complete application checkpoint before discarding log entries.
    /// The same checked image/reference format is used by the Raft host. An I/O
    /// error leaves authority uncertain and fences this host until recovery.
    pub fn checkpoint(&self) -> Result<Position, HostError> {
        let mut disk = self.0.disk.lock();
        let status = self.status();
        if status.failure.is_some() || !status.is_leader {
            return Err(HostError::Unavailable(
                "local checkpoint unavailable".into(),
            ));
        }
        if disk.journal.state.snap_index == status.applied.index {
            return Ok(status.applied);
        }
        let result: Result<Position, String> = (|| {
            let image = crate::snapshot::create(
                &self.0.directory,
                self.0.group,
                status.applied,
                &*self.0.machine,
                true, // A compacted prefix must have durable checkpoint files.
            )?;
            self.0
                .machine
                .validate_checkpoint(&image.directory, status.applied)?;
            if !mako_protocol_core::recovery::may_trim(
                status.applied.index,
                image.reference.position.index,
                true,
                true,
            ) {
                return Err("checkpoint does not cover local log prefix".into());
            }
            let fs = RealFs::with_sync_writes(true);
            let name = raft_store::images::write(
                &fs,
                &self.0.directory.join("images"),
                status.applied.index,
                status.applied.term,
                &crate::wire::encode(&image.reference)?,
            )
            .map_err(|e| e.to_string())?;
            disk.journal.persist(Record {
                snapshot: Some(SnapRef {
                    index: status.applied.index,
                    term: status.applied.term,
                    image: Some(name),
                    keep: true,
                }),
                ..Record::default()
            })?;
            disk.journal.flush()?;
            Ok(status.applied)
        })();
        match result {
            Ok(position) => {
                self.0.status.write().durable_sequence = disk.journal.sequence;
                Ok(position)
            }
            Err(error) => {
                self.0.status.write().failure = Some(error.clone());
                Err(HostError::Storage(error))
            }
        }
    }
    pub fn read_barrier(
        &self,
        expected_term: u64,
        _timeout: Duration,
    ) -> Result<Position, HostError> {
        let _gate = self.0.disk.lock();
        let status = self.status();
        if status.failure.is_some() || !status.is_leader {
            return Err(HostError::Unavailable("local log unavailable".into()));
        }
        if expected_term != status.term {
            return Err(HostError::NotLeader);
        }
        Ok(status.applied)
    }
    pub fn propose(&self, expected_term: u64, bytes: Vec<u8>) -> Result<Proposal, AdmissionError> {
        if bytes.len() > crate::MAX_COMMAND_BYTES {
            return Err(AdmissionError::TooLarge);
        }
        let mut disk = self.0.disk.lock();
        let status = self.status();
        if let Some(failure) = status.failure {
            return Err(AdmissionError::Unavailable(failure));
        }
        if !status.is_leader || expected_term != 1 {
            return Err(AdmissionError::NotLeader {
                term: 1,
                leader: None,
            });
        }
        let index = disk
            .journal
            .state
            .last()
            .checked_add(1)
            .ok_or_else(|| AdmissionError::Unavailable("local log exhausted".into()))?;
        let position = Position { index, term: 1 };
        if let Err(error) = disk.journal.persist(Record {
            hard: Some(Hard {
                term: 1,
                vote: u16::MAX,
                commit: index,
            }),
            replace_from: Some(index),
            entries: vec![(1, encode_command(Some(&bytes)))],
            ..Record::default()
        }) {
            self.0.status.write().failure = Some(error);
            return Err(AdmissionError::Unknown);
        }
        let proposal = Proposal {
            position,
            completion: Arc::new((Mutex::new(ProposalStatus::Committed), Condvar::new())),
            committed: Arc::new(AtomicBool::new(true)),
        };
        {
            let mut status = self.0.status.write();
            status.committed = position;
            status.durable_sequence = disk.journal.sequence;
        }
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            self.0.machine.apply(position, Some(&bytes)).and_then(|()| {
                if self.0.machine.applied_position()? == position {
                    Ok(())
                } else {
                    Err("local application did not advance its prefix".into())
                }
            })
        }))
        .unwrap_or_else(|_| Err("local application panicked after durable commitment".into()));
        match result {
            Ok(()) => {
                self.0.status.write().applied = position;
                proposal.set(ProposalStatus::Applied);
            }
            Err(error) => {
                self.0.status.write().failure = Some(error.clone());
                proposal.set(ProposalStatus::StorageFailed(error));
            }
        }
        Ok(proposal)
    }
    pub fn shutdown(&self) {
        let _gate = self.0.disk.lock();
        let mut status = self.0.status.write();
        status.is_leader = false;
        status.failure.get_or_insert("local log closed".into());
    }
}
