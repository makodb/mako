//! Native connection convenience for the backend-independent gateway client.
use crate::{rpc::Network, wire::*};
#[cfg(feature = "runtime-storage")]
pub(crate) use crate::gateway_protocol::intent;
#[cfg(test)]
use mako_sharding::{gateway as core, types::NO_SNAPSHOT};
use std::{io::Read, sync::Arc, time::Duration};

pub struct GatewayClient(crate::gateway_protocol::GatewayClient);
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
        crate::gateway_protocol::GatewayClient::with_identity(
            network, endpoint, client, first, timeout,
        )
        .map(Self)
    }
}
impl std::ops::Deref for GatewayClient {
    type Target = crate::gateway_protocol::GatewayClient;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
impl std::ops::DerefMut for GatewayClient {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
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
