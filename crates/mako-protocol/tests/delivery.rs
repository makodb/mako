mod support;
use mako_protocol::{
    contracts::{Transport, VolatileLog},
    participant::Lane,
    remote::Remotes,
    wire::*,
};
use std::{collections::BTreeMap, sync::Arc, time::Duration};
use support::*;

fn remotes(network: Arc<Network>) -> Remotes {
    Remotes::new(
        network,
        Arc::new(BTreeMap::from([(0, "a".into()), (1, "b".into())])),
        BTreeMap::new(),
        request(1, 1, Action::Abort).stream,
        request(1, 1, Action::Abort).identity,
        Arc::new(VolatileLog),
        Duration::from_millis(10),
    )
}
#[test]
fn lost_install_reply_replays_exact_receipt_without_repeating_effects() {
    let net = Network::new(&["a"]);
    net.call(
        "a",
        Message::OpenStream {
            stream: request(1, 1, Action::Abort).stream,
            nonce: 9,
        },
        Duration::ZERO,
    )
    .unwrap();
    for (n, action) in [(1, put(b"committed\0\xff")), (2, Action::Validate)] {
        assert!(matches!(
            net.call(
                "a",
                Message::Participant(request(1, n, action)),
                Duration::ZERO
            )
            .unwrap(),
            Reply::Participant(Response { result: Ok(_), .. })
        ));
    }
    let install = request(1, 3, Action::Install { timestamp: 20 });
    net.fault(Delivery::DropReply);
    assert!(net
        .call(
            "a",
            Message::Participant(install.clone()),
            Duration::from_millis(10)
        )
        .is_err());
    net.fault(Delivery::Duplicate);
    let result = net
        .call("a", Message::Participant(install.clone()), Duration::ZERO)
        .unwrap();
    assert!(matches!(
        result,
        Reply::Participant(Response { result: Ok(_), .. })
    ));
    let mut altered = install;
    altered.action = Action::Install { timestamp: 21 };
    assert!(matches!(
        net.call("a", Message::Participant(altered), Duration::ZERO)
            .unwrap(),
        Reply::Participant(Response { result: Err(_), .. })
    ));
    let world = net.0.lock().unwrap();
    let data = world.lanes["a"].engine().data.lock().unwrap();
    assert_eq!(data.installed, 1);
    assert_eq!(data.rows[b"c".as_slice()], b"committed\0\xff");
    assert_eq!(world.now, 16);
}
#[test]
fn lost_stage_reply_still_enrolls_peer_and_cleanup_visits_every_peer() {
    let net = Arc::new(Network::new(&["a", "b"]));
    let mut remote = remotes(net.clone());
    net.fault(Delivery::DropReply);
    assert!(remote.call(0, put(b"a")).is_err());
    remote.call(1, put(b"b")).unwrap();
    net.fault(Delivery::DropRequest);
    assert_eq!(remote.abort().unwrap_err().code, ErrorCode::Unknown);
    assert!(remote.take_cleanup_failure().is_some());
    let world = net.0.lock().unwrap();
    assert_eq!(
        world.lanes["b"].phase(),
        mako_protocol::admission::Phase::Aborted
    );
    for lane in world.lanes.values() {
        assert!(lane.engine().data.lock().unwrap().rows.is_empty());
    }
    assert!(world.lanes["a"].engine().pin.is_some());
}
#[test]
fn stale_reply_is_rejected_and_changed_target_cannot_rebind_an_attempt() {
    let net = Arc::new(Network::new(&["a", "b"]));
    let mut remote = remotes(net.clone());
    net.fault(Delivery::StaleReply);
    assert_eq!(
        remote.call(0, put(b"a")).unwrap_err().code,
        ErrorCode::Transport
    );
    let target = Target {
        owner: 0,
        access: access(),
        log_id: 3,
    };
    remote.bind_target(&target, b"c").unwrap();
    let mut moved = target;
    moved.access.route = Some(Route {
        owner: 1,
        ownership_version: 1,
        snapshot_version: 1,
    });
    assert_eq!(
        remote.bind_target(&moved, b"c").unwrap_err().code,
        ErrorCode::RouteChanged
    );
    remote.abort().unwrap();
}
#[test]
fn delayed_abort_cannot_cancel_successor_and_retired_lease_cannot_execute() {
    let mut lane = Lane::new(Engine::new());
    lane.claim(9, 101).unwrap();
    lane.process(request(1, 1, put(b"discard"))).result.unwrap();
    let delayed = request(1, 2, Action::Abort);
    lane.process(delayed.clone()).result.unwrap();
    lane.process(request(2, 1, put(b"new"))).result.unwrap();
    let before = lane.engine().data.lock().unwrap().calls;
    assert!(lane.process(delayed).result.is_err());
    assert_eq!(lane.engine().data.lock().unwrap().calls, before);
    lane.process(request(2, 2, Action::Abort)).result.unwrap();
    lane.claim(10, 102).unwrap();
    assert!(lane.process(request(3, 1, put(b"stale"))).result.is_err());
    assert_eq!(lane.engine().data.lock().unwrap().calls, before + 1);
}
#[test]
fn uncertain_install_survives_lane_destruction_and_prevents_migration_drain() {
    let mut engine = Engine::new();
    engine.fail_install = true;
    let data = engine.data.clone();
    let epochs = engine.epochs.clone();
    let mut lane = Lane::new(engine);
    lane.claim(9, 101).unwrap();
    lane.process(request(1, 1, put(b"installed")))
        .result
        .unwrap();
    let cut = epochs.cut().unwrap();
    assert!(!epochs.drained(cut).unwrap());
    lane.process(request(1, 2, Action::Validate))
        .result
        .unwrap();
    let install = request(1, 3, Action::Install { timestamp: 20 });
    let response = lane.process(install.clone());
    assert_eq!(
        response.result.as_ref().unwrap_err().code,
        ErrorCode::Unknown
    );
    assert_eq!(lane.process(install), response);
    assert!(lane.claim(10, 102).is_err());
    drop(lane);
    assert!(!epochs.drained(cut).unwrap());
    assert_eq!(data.lock().unwrap().rows[b"c".as_slice()], b"installed");
    assert_eq!(data.lock().unwrap().installed, 1);
}
