use super::client::GatewayClient;
use sto_core::protocol::{contracts::Transport, wire::*};
use mako_sharding::{gateway, types::NO_SNAPSHOT};
use std::{
    sync::{Arc, Mutex},
    time::Duration,
};

#[derive(Default)]
struct Receipts(Mutex<(Vec<GatewayRequest>, usize)>);
impl Transport for Receipts {
    fn call(&self, _: &str, message: Message, _: Duration) -> Result<Reply> {
        let Message::Gateway(request) = message else {
            panic!("gateway request expected")
        };
        let mut state = self.0.lock().unwrap();
        if state.0.is_empty() {
            state.1 += 1;
        } else {
            assert_eq!(
                state.0[0], request,
                "retry must retain exact identity, route and payload"
            );
        }
        state.0.push(request.clone());
        if state.0.len() == 1 {
            return Err(Error::new(
                ErrorCode::Transport,
                "reply lost after durable effect",
            ));
        }
        Ok(Reply::Gateway(GatewayResponse {
            client: request.client,
            sequence: request.sequence,
            snapshot_version: request
                .target
                .as_ref()
                .and_then(|t| t.access.route)
                .map_or(NO_SNAPSHOT, |r| r.snapshot_version),
            status: 0,
            outcome: gateway::COMMITTED,
            op_result: true,
            value: vec![],
        }))
    }
}

#[test]
fn gateway_loss_preserves_original_intent_until_exact_receipt_arrives() {
    let network = Arc::new(Receipts::default());
    let mut client = GatewayClient::with_identity(
        network.clone(),
        "node".into(),
        1 << 63,
        1,
        Duration::from_millis(5),
    )
    .unwrap();
    let target = Target {
        owner: 1,
        log_id: 1,
        access: Access {
            table: TableKey {
                table: 1,
                warehouse: 0,
            },
            route: Some(Route {
                owner: 1,
                ownership_version: 5,
                snapshot_version: 8,
            }),
        },
    };
    assert_eq!(
        client
            .put(target.clone(), b"key", b"stored")
            .unwrap_err()
            .code,
        ErrorCode::Transport
    );
    assert_eq!(
        client
            .put(target.clone(), b"key", b"different")
            .unwrap_err()
            .code,
        ErrorCode::Unknown
    );
    let mut moved = target.clone();
    moved.access.route.as_mut().unwrap().snapshot_version = 9;
    // A refreshed route cannot replace the retained route of this pending write.
    client.put(moved, b"key", b"stored").unwrap();
    let state = network.0.lock().unwrap();
    assert_eq!(state.0.len(), 2);
    assert_eq!(state.1, 1);
    assert_eq!(state.0[1].target, Some(target));
}
