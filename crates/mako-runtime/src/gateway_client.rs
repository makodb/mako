//! External Rust client using the existing verified pending-request sequencer.
//! It can be built without the native-storage feature or any Masstree library.
use crate::{rpc::Network, wire::*};
use mako_sharding::{gateway as core, types::NO_SNAPSHOT};
use std::{io::Read, sync::Arc, time::Duration};

pub(crate) fn intent(request: &GatewayRequest) -> Result<core::Request> {
    if request.key.len() > 64 || request.value.len() > 8000 {
        return Err(Error::new(
            ErrorCode::Invalid,
            "gateway request exceeds byte bounds",
        ));
    }
    let mut result = core::Request {
        kind: request.kind,
        client: request.client,
        sequence: request.sequence,
        session: request.session,
        table: 0,
        ownership_version: 0,
        snapshot_version: NO_SNAPSHOT,
        owner: 0,
        physical_table: 0,
        route_known: false,
        fixed_coordinate: false,
        coordinate: Vec::new(),
        key: request.key.clone(),
        name: Vec::new(),
        value: request.value.clone(),
    };
    if let Some(target) = &request.target {
        result.table = target.access.table.table;
        result.owner = target.owner;
        result.physical_table = u32::from(target.log_id);
        result.route_known = true;
        result.fixed_coordinate = target.access.table.warehouse != 0;
        result.coordinate = if result.fixed_coordinate {
            target.access.table.warehouse.to_be_bytes().to_vec()
        } else {
            request.key.clone()
        };
        if let Some(route) = target.access.route {
            if route.owner != target.owner {
                return Err(Error::new(
                    ErrorCode::Invalid,
                    "inconsistent gateway route owner",
                ));
            }
            result.ownership_version = route.ownership_version;
            result.snapshot_version = route.snapshot_version;
        }
    }
    Ok(result)
}
fn request(prepared: &core::Request) -> Result<GatewayRequest> {
    let target =
        if prepared.route_known {
            let warehouse =
                if prepared.fixed_coordinate {
                    u32::from_be_bytes(prepared.coordinate.as_slice().try_into().map_err(|_| {
                        Error::new(ErrorCode::Invalid, "invalid warehouse coordinate")
                    })?)
                } else {
                    0
                };
            let route = if prepared.snapshot_version == NO_SNAPSHOT {
                None
            } else {
                Some(Route {
                    owner: prepared.owner,
                    ownership_version: prepared.ownership_version,
                    snapshot_version: prepared.snapshot_version,
                })
            };
            Some(Target {
                owner: prepared.owner,
                access: Access {
                    table: TableKey {
                        table: prepared.table,
                        warehouse,
                    },
                    route,
                },
                log_id: u16::try_from(prepared.physical_table)
                    .map_err(|_| Error::new(ErrorCode::Invalid, "invalid physical table ID"))?,
            })
        } else {
            None
        };
    Ok(GatewayRequest {
        client: prepared.client,
        sequence: prepared.sequence,
        session: prepared.session,
        kind: prepared.kind,
        target,
        key: prepared.key.clone(),
        value: prepared.value.clone(),
        forwarded: false,
    })
}

pub struct GatewayClient {
    network: Arc<Network>,
    endpoint: String,
    sequencer: core::Client,
    session: u64,
    timeout: Duration,
}
impl GatewayClient {
    pub fn connect(endpoint: String, timeout: Duration) -> Result<Self> {
        let mut nonce = [0; 8];
        std::fs::File::open("/dev/urandom")
            .and_then(|mut file| file.read_exact(&mut nonce))
            .map_err(|e| Error::new(ErrorCode::Fatal, e.to_string()))?;
        Self::with_identity(
            Arc::new(Network::default()),
            endpoint,
            u64::from_ne_bytes(nonce) | (1 << 63),
            1,
            timeout,
        )
    }
    pub fn with_identity(
        network: Arc<Network>,
        endpoint: String,
        client: u64,
        first: u64,
        timeout: Duration,
    ) -> Result<Self> {
        if timeout.is_zero() {
            return Err(Error::new(ErrorCode::Invalid, "zero gateway deadline"));
        }
        let sequencer = core::Client::new(client, first)
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "invalid gateway client identity"))?;
        Ok(Self {
            network,
            endpoint,
            sequencer,
            session: 0,
            timeout,
        })
    }
    pub fn resolve(&self, request: RouteRequest) -> Result<Target> {
        match self
            .network
            .call(&self.endpoint, Message::Route(request), self.timeout)?
        {
            Reply::Route(result) => result,
            Reply::Error(error) => Err(error),
            _ => Err(Error::new(
                ErrorCode::Transport,
                "unexpected route response",
            )),
        }
    }
    /// Unknown and in-flight responses retain the exact request. Calling again
    /// with the same intent resends that identity; a different intent is refused.
    pub fn execute(
        &mut self,
        kind: u32,
        target: Option<Target>,
        key: &[u8],
        value: &[u8],
    ) -> Result<GatewayResponse> {
        let proposed = GatewayRequest {
            client: 0,
            sequence: 0,
            session: self.session,
            kind,
            target,
            key: key.to_vec(),
            value: value.to_vec(),
            forwarded: false,
        };
        let prepared = self
            .sequencer
            .prepare(intent(&proposed)?)
            .map_err(|_| {
                Error::new(
                    ErrorCode::Unknown,
                    "pending gateway request differs or sequence exhausted",
                )
            })?
            .clone();
        let request = request(&prepared)?;
        let reply =
            match self
                .network
                .call(&self.endpoint, Message::Gateway(request), self.timeout)?
            {
                Reply::Gateway(reply) => reply,
                Reply::Error(error)
                    if matches!(
                        error.code,
                        ErrorCode::Busy
                            | ErrorCode::Closed
                            | ErrorCode::Invalid
                            | ErrorCode::Capacity
                    ) =>
                {
                    GatewayResponse {
                        client: prepared.client,
                        sequence: prepared.sequence,
                        snapshot_version: prepared.snapshot_version,
                        status: if error.code == ErrorCode::Busy { 4 } else { 2 },
                        outcome: core::ABORTED,
                        op_result: false,
                        value: Vec::new(),
                    }
                }
                Reply::Error(error) => return Err(error),
                _ => {
                    return Err(Error::new(
                        ErrorCode::Transport,
                        "unexpected gateway response",
                    ))
                }
            };
        if reply.client != prepared.client
            || reply.sequence != prepared.sequence
            || reply.snapshot_version != prepared.snapshot_version
        {
            return Err(Error::new(
                ErrorCode::Transport,
                "gateway response identity mismatch",
            ));
        }
        self.sequencer.complete(
            reply.client,
            reply.sequence,
            reply.snapshot_version,
            reply.outcome,
        );
        if reply.outcome == core::COMMITTED {
            match kind {
                core::BEGIN => self.session = prepared.session,
                core::COMMIT | core::ROLLBACK => self.session = 0,
                _ => (),
            }
        }
        Ok(reply)
    }
    pub fn get(&mut self, target: Target, key: &[u8]) -> Result<Option<Vec<u8>>> {
        let reply = self.execute(core::GET, Some(target), key, &[])?;
        terminal(&reply)?;
        Ok(if reply.op_result {
            Some(reply.value)
        } else {
            None
        })
    }
    pub fn put(&mut self, target: Target, key: &[u8], value: &[u8]) -> Result<()> {
        let reply = self.execute(core::PUT, Some(target), key, value)?;
        terminal(&reply)
    }
    pub fn insert(&mut self, target: Target, key: &[u8], value: &[u8]) -> Result<bool> {
        let reply = self.execute(core::INSERT, Some(target), key, value)?;
        terminal(&reply)?;
        Ok(reply.op_result)
    }
    pub fn remove(&mut self, target: Target, key: &[u8]) -> Result<bool> {
        let reply = self.execute(core::DELETE, Some(target), key, &[])?;
        terminal(&reply)?;
        Ok(reply.op_result)
    }
    /// Session grouping matches the existing API: each operation auto-commits.
    pub fn begin_session(&mut self) -> Result<()> {
        let reply = self.execute(core::BEGIN, None, &[], &[])?;
        terminal(&reply)
    }
    pub fn end_session(&mut self, rollback: bool) -> Result<()> {
        let reply = self.execute(
            if rollback {
                core::ROLLBACK
            } else {
                core::COMMIT
            },
            None,
            &[],
            &[],
        )?;
        terminal(&reply)
    }
}
fn terminal(reply: &GatewayResponse) -> Result<()> {
    match reply.outcome {
        core::COMMITTED => Ok(()),
        core::UNKNOWN => Err(Error::new(
            ErrorCode::Unknown,
            "gateway outcome is unresolved",
        )),
        core::IN_FLIGHT => Err(Error::new(
            ErrorCode::Busy,
            "gateway request is still in flight",
        )),
        core::ABORTED => Err(Error::new(
            if reply.status == 4 {
                ErrorCode::Busy
            } else {
                ErrorCode::Conflict
            },
            "gateway operation aborted",
        )),
        _ => Err(Error::new(ErrorCode::Invalid, "gateway request rejected")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpc::RpcServer;
    use parking_lot::Mutex;
    #[test]
    fn unresolved_sdk_request_retains_identity_and_original_routing_tuple() {
        let network = Arc::new(Network::default());
        let seen = Arc::new(Mutex::new(Vec::<GatewayRequest>::new()));
        let capture = seen.clone();
        let mut server = RpcServer::start(&network, "127.0.0.1:0", move |message, respond| {
            let Message::Gateway(request) = message else {
                panic!("expected gateway request");
            };
            let mut seen = capture.lock();
            seen.push(request.clone());
            respond(Reply::Gateway(GatewayResponse {
                client: request.client,
                sequence: request.sequence,
                snapshot_version: request
                    .target
                    .as_ref()
                    .and_then(|target| target.access.route)
                    .map_or(NO_SNAPSHOT, |route| route.snapshot_version),
                status: if seen.len() == 1 { 1 } else { 0 },
                outcome: if seen.len() == 1 {
                    core::UNKNOWN
                } else {
                    core::COMMITTED
                },
                op_result: true,
                value: Vec::new(),
            }));
        })
        .unwrap();
        let mut client = GatewayClient::with_identity(
            network,
            server.address().to_owned(),
            1 << 63,
            1,
            Duration::from_secs(3),
        )
        .unwrap();
        let target = Target {
            owner: 1,
            access: Access {
                table: TableKey {
                    table: 1,
                    warehouse: 0,
                },
                route: Some(Route {
                    owner: 1,
                    ownership_version: 3,
                    snapshot_version: 9,
                }),
            },
            log_id: 1,
        };
        assert_eq!(
            client
                .put(target.clone(), b"key", b"value")
                .unwrap_err()
                .code,
            ErrorCode::Unknown
        );
        assert_eq!(
            client
                .put(target.clone(), b"different", b"value")
                .unwrap_err()
                .code,
            ErrorCode::Unknown
        );
        assert_eq!(seen.lock().len(), 1);
        let mut refreshed = target.clone();
        refreshed.owner = 2;
        refreshed.access.route = Some(Route {
            owner: 2,
            ownership_version: 4,
            snapshot_version: 10,
        });
        client.put(refreshed, b"key", b"value").unwrap();
        assert_eq!(seen.lock()[1].target, Some(target));
        {
            let seen = seen.lock();
            assert_eq!((seen[0].sequence, seen[1].sequence), (1, 1));
        }
        client.begin_session().unwrap();
        client.end_session(true).unwrap();
        {
            let seen = seen.lock();
            assert_eq!((seen[2].sequence, seen[2].session), (2, 2));
        }
        server.shutdown();
    }
}
