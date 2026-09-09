pub const fn raft_service_server_unavailable(has_server: bool,
                                             disconnected: bool,
                                             rpc_ready: bool) -> bool {
    !has_server || disconnected || !rpc_ready
}
