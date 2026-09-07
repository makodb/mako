pub const fn raft_service_server_unavailable(has_server: bool,
                                             disconnected: bool,
                                             rpc_ready: bool) -> bool {
    !has_server || disconnected || !rpc_ready
}

pub const fn raft_service_poll_thread_available(found: bool,
                                                has_poll_thread: bool) -> bool {
    found && has_poll_thread
}

pub const fn raft_service_lease_is_open(state: u64,
                                        drain_bit: u64) -> bool {
    (state & drain_bit) == 0
}

pub const fn raft_service_lease_count(state: u64,
                                      count_mask: u64) -> u64 {
    state & count_mask
}
