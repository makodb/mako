//! Remote participant identity and delivery, independent of SRPC and storage.
use crate::{
    contracts::{CommitLog, Transport},
    wire::*,
};
use std::{collections::BTreeMap, sync::Arc, time::Duration};

struct Peer {
    incarnation: u64,
    sequence: u64,
    endpoint: String,
}
pub struct Remotes {
    cleanup_failure: Option<Error>,
    network: Arc<dyn Transport>,
    endpoints: Arc<BTreeMap<u32, String>>,
    replicas: BTreeMap<u32, ReplicaSet>,
    stream: Stream,
    identity: Identity,
    peers: BTreeMap<u32, Peer>,
    targets: BTreeMap<(u32, u16, Vec<u8>), Access>,
    log: Arc<dyn CommitLog>,
    timeout: Duration,
}
impl Remotes {
    fn select(&mut self, owner: u32) -> Result<()> {
        if !self.peers.contains_key(&owner) {
            let endpoint = if let Some(replicas) = self.replicas.get(&owner) {
                self.network.find_leader(owner, replicas, self.timeout)?
            } else {
                self.endpoints
                    .get(&owner)
                    .cloned()
                    .ok_or_else(|| Error::new(ErrorCode::Invalid, "missing shard endpoint"))?
            };
            let incarnation = match self.network.call(
                &endpoint,
                Message::OpenStream {
                    stream: self.stream,
                    nonce: self.identity.incarnation,
                },
                self.timeout,
            )? {
                Reply::OpenStream(result) => result?,
                Reply::Error(error) => return Err(error),
                _ => {
                    return Err(Error::new(
                        ErrorCode::Transport,
                        "invalid stream claim reply",
                    ))
                }
            };
            self.peers.insert(
                owner,
                Peer {
                    incarnation,
                    sequence: 0,
                    endpoint,
                },
            );
        }
        Ok(())
    }
    pub fn call(&mut self, owner: u32, action: Action) -> Result<Value> {
        self.select(owner)?;
        // A lost initial reply may leave a remote session requiring ABORT.
        let peer = self
            .peers
            .get_mut(&owner)
            .expect("selected participant endpoint");
        peer.sequence = peer.sequence.checked_add(1).ok_or_else(|| {
            Error::new(
                ErrorCode::Capacity,
                "participant request sequence exhausted",
            )
        })?;
        let identity = Identity {
            incarnation: peer.incarnation,
            sequence: peer.sequence,
            ..self.identity
        };
        let reply = self.network.call(
            &peer.endpoint,
            Message::Participant(Request {
                stream: self.stream,
                identity,
                action,
            }),
            self.timeout,
        )?;
        match reply {
            Reply::Participant(response) if response.identity == identity => response.result,
            Reply::Error(error) => Err(error),
            _ => Err(Error::new(
                ErrorCode::Transport,
                "reply does not match pending participant identity",
            )),
        }
    }
    fn all(&mut self, action: Action) -> Result<()> {
        let owners: Vec<_> = self.peers.keys().copied().collect();
        let mut failure = None;
        for owner in owners {
            // Send cleanup to every peer even when an earlier ABORT failed.
            if let Err(error) = self.call(owner, action.clone()) {
                failure.get_or_insert(error);
            }
        }
        failure.map_or(Ok(()), Err)
    }
}
impl Remotes {
    pub fn new(
        network: Arc<dyn Transport>,
        endpoints: Arc<BTreeMap<u32, String>>,
        replicas: BTreeMap<u32, ReplicaSet>,
        stream: Stream,
        identity: Identity,
        log: Arc<dyn CommitLog>,
        timeout: Duration,
    ) -> Self {
        Self {
            network,
            endpoints,
            replicas,
            stream,
            identity,
            log,
            timeout,
            peers: BTreeMap::new(),
            targets: BTreeMap::new(),
            cleanup_failure: None,
        }
    }
    pub fn identity(&self) -> Identity {
        self.identity
    }
    pub fn remote_identity(&mut self, owner: u32) -> Result<Identity> {
        self.select(owner)?;
        Ok(Identity {
            incarnation: self.peers[&owner].incarnation,
            ..self.identity
        })
    }
    pub fn take_cleanup_failure(&mut self) -> Option<Error> {
        self.cleanup_failure.take()
    }
    pub fn bind_target(&mut self, target: &Target, key: &[u8]) -> Result<()> {
        if self.targets.iter().any(|((owner, id, _), access)| {
            *owner == target.owner && *id == target.log_id && access.table != target.access.table
        }) {
            return Err(Error::new(
                ErrorCode::Invalid,
                "remote log ID identifies two physical tables",
            ));
        }
        let slot = self
            .targets
            .entry((target.owner, target.log_id, key.to_vec()))
            .or_insert_with(|| target.access.clone());
        if slot != &target.access {
            return Err(Error::new(
                ErrorCode::RouteChanged,
                "remote table route changed during transaction",
            ));
        }
        Ok(())
    }
}

impl Remotes {
    pub fn stage(
        &mut self,
        shard: u32,
        table: u16,
        mutation: Mutation,
        key: &[u8],
        value: &[u8],
    ) -> Result<()> {
        let access = self
            .targets
            .get(&(shard, table, key.to_vec()))
            .cloned()
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "remote table was not resolved"))?;
        self.call(
            shard,
            Action::Stage {
                writes: vec![Write {
                    access,
                    mutation,
                    key: key.to_vec(),
                    value: value.to_vec(),
                }],
            },
        )
        .map(|_| ())
    }
    pub fn timestamp(&mut self) -> Result<u32> {
        let mut maximum = 0;
        for owner in self.peers.keys().copied().collect::<Vec<_>>() {
            match self.call(owner, Action::Timestamp)? {
                Value::Timestamp(timestamp) => maximum = maximum.max(timestamp),
                _ => {
                    return Err(Error::new(
                        ErrorCode::Transport,
                        "missing participant timestamp",
                    ))
                }
            }
        }
        Ok(maximum)
    }
    pub fn validate(&mut self) -> Result<()> {
        self.all(Action::Validate)
    }
    pub fn install(&mut self, timestamp: u32) -> Result<()> {
        self.all(Action::Install { timestamp })
    }
    pub fn abort(&mut self) -> Result<()> {
        self.all(Action::Abort).map_err(|error| {
            let error = Error::new(
                ErrorCode::Unknown,
                format!("remote cleanup was not confirmed: {error}"),
            );
            self.cleanup_failure = Some(error.clone());
            error
        })
    }
    pub fn log(&mut self, bytes: &[u8], time_term: u32) -> Result<()> {
        self.log.append(bytes, time_term)
    }
}
