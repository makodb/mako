pub const fn raft_test_server_index_is_valid(index: i32,
                                              server_count: i32) -> bool {
    index >= 0 && index < server_count
}

pub const fn raft_test_wrapped_server_index(index: i32,
                                             offset: i32,
                                             server_count: i32) -> i32 {
    let mut wrapped = (index + offset) % server_count;
    if wrapped < 0 {
        wrapped += server_count;
    }
    wrapped
}

pub const fn raft_test_connected_term_moved_on(disconnected: bool,
                                               current_term: u64,
                                               observed_term: u64) -> bool {
    !disconnected && current_term > observed_term
}

pub const fn raft_test_wait_leader_is_invalid(disconnected: bool,
                                               is_leader: bool,
                                               current_term: u64,
                                               expected_term: u64) -> bool {
    disconnected || !is_leader || current_term != expected_term
}

pub const fn raft_test_should_record_agreement_command(command_kind: i32,
                                                        agreement_kind: i32) -> bool {
    command_kind == agreement_kind
}
