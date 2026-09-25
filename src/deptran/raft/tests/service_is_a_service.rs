// The Raft service satisfies srpc's Service bound.
//
// This is the assertion stage 3e turns on. `trait Service: Send + Sync`
// (src/srpc/rpc/server.rs:183), so this compiles only if RaftRpcService --
// and through it RaftServerBase -- is Send + Sync. Before stage 3a removed
// `commo_`, it was not, and the Rust lane could not have dispatched to Raft
// at all.
//
// It is a compile-time test on purpose: there is nothing to run. What would
// regress is the bound, and the bound is checked by this file existing.
fn assert_service<T: srpc::server::Service>() {}

#[test]
fn raft_rpc_service_satisfies_srpcs_service_bound() {
    assert_service::<raft::service::RaftRpcService>();
}
