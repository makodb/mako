pub const fn raft_frame_can_register_lab_scheduler(n_replicas: u16,
                                                   expected_replicas: u16) -> bool {
    n_replicas < expected_replicas
}

pub const fn raft_frame_all_schedulers_created(n_replicas: u16,
                                               expected_replicas: u16) -> bool {
    n_replicas == expected_replicas
}

pub const fn raft_frame_should_create_test_fiber(site_id: u32) -> bool {
    site_id == 0
}

pub const fn raft_frame_more_commos_needed(n_commos: u16,
                                           expected_replicas: u16) -> bool {
    n_commos < expected_replicas
}

pub const fn raft_frame_is_lab_config(replica_protocol: i32,
                                      raft_protocol: i32,
                                      num_partitions: u32,
                                      partition_size: i32,
                                      local_server_count: usize) -> bool {
    replica_protocol == raft_protocol &&
        num_partitions == 1 &&
        partition_size == 5 &&
        local_server_count == 5
}

pub const fn raft_frame_lab_process_exit_code(is_lab_config: bool,
                                              test_result: i32) -> i32 {
    if is_lab_config && test_result != 0 {
        1
    } else {
        0
    }
}
