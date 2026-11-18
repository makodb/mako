use rocksdb_raft_service::{config::{check_scope, ClientConfig, Config}, wire::{Envelope, ErrorCode, Operation, Request, PROTOCOL_VERSION}};
use serde_json::json;

fn configured() -> Config {
    let peers = json!([{"id":1,"raft":"127.0.0.1:24101","client":"127.0.0.1:24001"}]);
    serde_json::from_value(json!({
        "node":1,"listen":"127.0.0.1:24001",
        "groups":[{"id":1,"data_path":"/tmp/group1/data","raft_path":"/tmp/group1/raft","raft_listen":"127.0.0.1:24101","peers":peers}],
        "routing":{"version":7,"shards":[{"id":1,"peers":peers},
            {"id":2,"peers":[{"id":2,"raft":"127.0.0.1:24202","client":"127.0.0.1:24002"}]}],
            "tables":[{"id":99,"name":"binary","boundaries":[
                {"start":[],"owner":1,"ownership_version":2},
                {"start":[128,0],"owner":2,"ownership_version":7}]}]}
    })).unwrap()
}
#[test]
fn remote_shards_are_valid_without_being_colocated() {
    let config = configured();
    config.validate().unwrap();
    let snapshot = config.snapshot();
    assert_eq!(snapshot.capture(99, &[127, 255]).unwrap().grant.owner, 1);
    assert_eq!(snapshot.capture(99, &[128, 0]).unwrap().grant.owner, 2);
    assert_eq!(snapshot.capture(99, &[255]).unwrap().grant.owner, 2);
}
#[test]
fn scans_reject_other_owners_even_when_limit_is_one() {
    let config = configured();
    let snapshot = config.snapshot();
    let inside = Operation::Scan { start: vec![], end: Some(vec![128, 0]), limit: 1 };
    check_scope(&snapshot, 99, 1, &inside).unwrap();
    let cross = Operation::ReverseScan { start: vec![], end: None, limit: 1 };
    assert_eq!(check_scope(&snapshot, 99, 1, &cross).unwrap_err().code, ErrorCode::CrossShard);
    let outside = Operation::Get { key: vec![128, 0] };
    assert_eq!(check_scope(&snapshot, 99, 1, &outside).unwrap_err().code, ErrorCode::CrossShard);
}
#[test]
fn nonexistent_owners_and_changed_replica_membership_are_rejected() {
    let mut config = configured();
    config.routing.tables[0].boundaries[1].owner = 17;
    assert!(config.validate().is_err());
    let mut config = configured();
    config.groups[0].peers[0].client = "127.0.0.1:24999".into();
    assert!(config.validate().is_err());
}
#[test]
fn binary_wire_and_limits_preserve_exact_key_identity() {
    let request = Request::Execute { handle: rocksdb_raft_service::wire::Handle { group:1,lane:0,nonce:42 },
        group:1,table:"binary".into(),operation:Operation::Put {key:vec![0,255,0],value:vec![255,0,128]} };
    let bytes = serde_json::to_vec(&Envelope::new(request)).unwrap();
    let decoded: Envelope = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(decoded.version, PROTOCOL_VERSION);
    match decoded.request { Request::Execute { operation: Operation::Put {key,value},.. } => {
        assert_eq!(key, vec![0,255,0]); assert_eq!(value, vec![255,0,128]);
    }, _ => panic!("wrong decoded operation") }
    let mut limits = configured().limits;
    limits.key_bytes = 2;
    assert_eq!(limits.operation(&Operation::Get {key:vec![0,255,0]}).unwrap_err().code, ErrorCode::TooLarge);
    assert_eq!(limits.operation(&Operation::Scan {start:vec![1],end:Some(vec![1]),limit:1}).unwrap_err().code, ErrorCode::Invalid);
}

#[test]
fn client_configuration_needs_only_remote_placement_and_client_limits() {
    let server = configured();
    let options = server.client_config();
    options.validate().unwrap();
    assert_eq!(options.limits.request_timeout_ms, server.limits.commit_ms + 2000);
    let encoded = serde_json::to_value(&options).unwrap();
    let object = encoded.as_object().unwrap();
    assert_eq!(object.len(), 3);
    assert!(object.contains_key("routing"));
    assert!(object.contains_key("limits"));
    assert!(object.contains_key("context_capacity"));
    let client: ClientConfig = serde_json::from_value(encoded.clone()).unwrap();
    client.validate().unwrap();
    assert_eq!(client.snapshot().capture(99, &[128, 0]).unwrap().grant.owner, 2);
    assert!(serde_json::from_value::<Config>(encoded).is_err());
    assert!(serde_json::from_value::<ClientConfig>(serde_json::to_value(server).unwrap()).is_err());
}

#[test]
fn client_context_and_transport_budgets_are_independent_of_server_workers() {
    let routing = configured().routing;
    let mut client: ClientConfig = serde_json::from_value(json!({
        "routing": routing, "limits": {"rpc_bytes":4096,"request_timeout_ms":750},
        "context_capacity":3
    })).unwrap();
    client.validate().unwrap();
    assert_eq!(client.context_capacity, 3);
    client.context_capacity = 0;
    assert!(client.validate().is_err());
    client.context_capacity = 3;
    client.limits.request_timeout_ms = 0;
    assert!(client.validate().is_err());
    client.limits.request_timeout_ms = 750;
    client.limits.rpc_bytes = 512;
    assert!(client.validate().is_err());
}
