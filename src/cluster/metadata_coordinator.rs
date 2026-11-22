//! Runtime-only metadata persistence; this does not make application data durable.
//! Recovered coordinators expose inspection and journaled replies, never migration
//! continuation. The runtime must additionally fence all data serving on recovery.
#![cfg(not(verus_keep_ghost))]

use std::path::Path;

use crate::directory::RouteTable;
use crate::durable_coordinator::{DurableCoordinator, Error, Operation, Response};
use crate::durable_kv::{Admission, CommitResult, DurableKv, Namespace, ReadResult, WriteOp};
use crate::migration::{Coordinator, Request};
use crate::rocks_metadata::RocksKv;
use crate::types::Status;

// Journal keys are exactly 32 bytes. This separate, versioned key cannot alias
// any journal namespace. The value retains the caller's exact identity bytes.
const IDENTITY_KEY: &[u8] = b"mako/metadata/coordinator/identity/v1";
const IDENTITY_VERSION: &[u8] = b"MAKO-COORDINATOR-IDENTITY\x01";
// Each coordinator has its own exclusively owned per-role database path.
const NAMESPACE: Namespace = Namespace { owner: 0, coordinator: true };

enum State {
    Live(Coordinator),
    Persistent {
        actor: DurableCoordinator,
        kv: RocksKv,
        admission: Admission,
    },
}

/// Owns the actor and (when configured) its single-writer metadata store.
/// No mutable actor, backend, admission, or reactivation capability is exported.
pub struct CoordinatorState {
    state: State,
    recovered: bool,
    failed: bool,
}

impl CoordinatorState {
    /// `identity` must encode the exact deployment/catalog hello. Initialization
    /// requires a fresh store; recovery never silently creates or replaces one.
    pub fn new(
        nodes: Vec<u32>,
        tables: Vec<RouteTable>,
        path: Option<&Path>,
        initialize: bool,
        identity: &[u8],
    ) -> Result<Self, Status> {
        let Some(path) = path else {
            return Ok(Self {
                state: State::Live(Coordinator::new(nodes, tables)?),
                recovered: false,
                failed: false,
            });
        };
        let mut kv = RocksKv::open(path, initialize)?;
        if initialize {
            match kv.read(IDENTITY_KEY) {
                ReadResult::Absent => {},
                ReadResult::Found(_) => return Err(Status::Invalid),
                ReadResult::Io => return Err(Status::Io),
            }
            let size = IDENTITY_VERSION.len().checked_add(identity.len())
                .ok_or(Status::Exhausted)?;
            let mut value = Vec::with_capacity(size);
            value.extend_from_slice(IDENTITY_VERSION);
            value.extend_from_slice(identity);
            let result = kv.submit(vec![WriteOp {
                key: IDENTITY_KEY.to_vec(),
                value: Some(value),
            }]);
            if !matches!(result, CommitResult::Committed) {
                return Err(Status::Io);
            }
        } else {
            match kv.read(IDENTITY_KEY) {
                ReadResult::Found(value) => {
                    if value.strip_prefix(IDENTITY_VERSION) != Some(identity) {
                        return Err(Status::Invalid);
                    }
                },
                ReadResult::Absent => return Err(Status::Invalid),
                ReadResult::Io => return Err(Status::Io),
            }
        }
        let mut actor = if initialize {
            DurableCoordinator::create(&mut kv, NAMESPACE, nodes, tables)
        } else {
            DurableCoordinator::recover(&mut kv, NAMESPACE)
        }.map_err(error_status)?;
        // Admission only grants access to metadata. It is NOT permission to
        // serve data, resume migration, or acknowledge application TP completion.
        let admission = actor.activate(&mut kv).map_err(error_status)?;
        Ok(Self {
            state: State::Persistent { actor, kv, admission },
            recovered: !initialize,
            failed: false,
        })
    }

    /// Inspect only the admitted, sound actor. Failed private append candidates
    /// remain inaccessible even if their in-memory native operation succeeded.
    pub fn actor(&self) -> Result<&Coordinator, Status> {
        if self.failed {
            return Err(Status::Io);
        }
        match &self.state {
            State::Live(actor) => Ok(actor),
            State::Persistent { actor, admission, .. } => {
                actor.actor(admission).map_err(error_status)
            },
        }
    }

    /// The only mutation entrance, including administrative reply bookkeeping.
    pub fn execute(&mut self, op: Operation) -> Result<Response, Status> {
        if self.failed {
            return Err(Status::Io);
        }
        if self.recovered && !matches!(&op, Operation::Reply(_)) {
            return Err(Status::Retry);
        }
        match &mut self.state {
            State::Live(actor) => execute_live(actor, op),
            State::Persistent { actor, kv, admission } => {
                match actor.execute(kv, admission, op) {
                    Ok(response) => Ok(response),
                    Err(Error::Native(status)) => Err(status),
                    Err(Error::Journal(_)) | Err(Error::Inactive) => {
                        self.failed = true;
                        Err(Status::Io)
                    },
                }
            },
        }
    }

    pub fn recovered(&self) -> bool {
        self.recovered
    }

    pub fn failed(&self) -> bool {
        self.failed
    }
}

fn error_status(error: Error) -> Status {
    match error {
        Error::Native(status) => status,
        Error::Journal(_) | Error::Inactive => Status::Io,
    }
}

// Mirrors the checked durable coordinator's native dispatch without introducing
// a second journal or a parallel nonce/intent map in live-only mode.
fn execute_live(actor: &mut Coordinator, op: Operation) -> Result<Response, Status> {
    match op {
        Operation::Begin { nonce, source, destination, range } => {
            if let Some(plan) = actor.plan_for_nonce(nonce) {
                if plan.source != source || plan.destination != destination
                    || plan.range.table != range.table || plan.range.lo != range.lo
                    || plan.range.hi != range.hi
                {
                    return Err(Status::Invalid);
                }
                return Ok(Response::Begun(plan.generation));
            }
            actor.begin(nonce, source, destination, range).map(Response::Begun)
        },
        Operation::Request(request) => {
            let status = match request {
                Request::Freeze => actor.request_freeze(),
                Request::Final => actor.request_final(),
                Request::Retire => actor.request_retire(),
                Request::Commit => actor.commit(),
                Request::Abort => actor.abort(),
                Request::Finish => actor.finish(),
            };
            applied(status)
        },
        Operation::Receive { generation, owner, certificate } => {
            applied(actor.receive(generation, owner, certificate))
        },
        Operation::Reply(nonce) => actor.reply(nonce)
            .map(Response::Replied).ok_or(Status::Retry),
    }
}

fn applied(status: Status) -> Result<Response, Status> {
    if matches!(status, Status::Ok) {
        Ok(Response::Applied)
    } else {
        Err(status)
    }
}
