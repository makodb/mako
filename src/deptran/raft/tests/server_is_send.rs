// RaftServerBase must be Send + Sync, and this is where that is checked.
//
// WHY IT MATTERS. The Rust srpc lane will only dispatch to a `Service`, and
// `trait Service: Send + Sync` (src/srpc/rpc/server.rs). Until this compiled,
// the Raft server could not be one: forty-seven of its forty-eight fields
// were fine and `commo_: *mut rusty::Communicator` was not. That field is
// gone -- the communicator now lives in a C++-side table keyed by server
// identity (RaftServerBase::set_commo, and commo_of in server.cc).
//
// WHAT THIS DOES AND DOES NOT PROVE. `Send` here is earned: the raw pointer
// that the compiler could see, and would have rejected, is no longer a field,
// and nothing was asserted with `unsafe impl` to get past it. But several
// remaining fields are OPAQUE CARRIERS -- fixed-size byte arrays standing in
// for C++ objects (a shared_ptr, a std::thread, a std::function, a mutex) --
// and a byte array is unconditionally Send + Sync, so for those the compiler
// is agreeing with a shape rather than with a type. Their C++ originals do
// have defined cross-thread semantics (atomic refcounts, a real mutex), which
// is why this is a reasonable thing to assert; janus::Communicator, whose
// peers_ and partition_peers_ are unguarded std::maps, did not, which is why
// it was removed rather than asserted over.
fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

#[test]
fn raft_server_base_is_send_and_sync() {
    assert_send::<raft::server_h::RaftServerBase>();
    assert_sync::<raft::server_h::RaftServerBase>();
}
