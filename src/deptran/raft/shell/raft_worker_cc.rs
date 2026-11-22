pub const fn raft_worker_should_start_submit_thread(started: bool) -> bool {
    !started
}

pub const fn raft_worker_should_stop_submit_thread(started: bool) -> bool {
    started
}

pub const fn raft_worker_should_enqueue(started: bool) -> bool {
    started
}

pub const fn raft_worker_batch_limit(batch_size: i32) -> i32 {
    if batch_size < 1 {
        1
    } else {
        batch_size
    }
}

pub const fn raft_worker_wait_for_submit(submitted: i32, total: i32) -> bool {
    submitted < total
}

pub const fn raft_worker_queue_has_work(queue_empty: bool) -> bool {
    !queue_empty
}

pub const fn raft_worker_submit_loop_should_wake(queue_empty: bool) -> bool {
    !queue_empty
}

pub const fn raft_worker_submit_loop_should_stop(queue_empty: bool) -> bool {
    queue_empty
}

pub const fn raft_worker_submit_loop_should_take(batch_size: i32,
                                                  limit: i32) -> bool {
    batch_size < limit
}

pub const fn raft_worker_partition_matches(worker_partition: u32,
                                            requested_partition: u32) -> bool {
    worker_partition == requested_partition
}

pub const fn raft_worker_scheduler_available(has_scheduler: bool) -> bool {
    has_scheduler
}

pub const fn raft_worker_leader_flag_from_bool(is_leader: bool) -> i32 {
    if is_leader {
        1
    } else {
        0
    }
}

pub const fn raft_worker_should_notify_default_partition(
    callback_partitions_empty: bool,
) -> bool {
    callback_partitions_empty
}

pub const fn raft_worker_has_command_payload(has_value: bool) -> bool {
    has_value
}

pub const fn raft_worker_callback_available(has_callback: bool) -> bool {
    has_callback
}

pub const fn raft_worker_should_buffer_unreplayed(status: i32,
                                                   safety_fail_status: i32,
                                                   len: i32) -> bool {
    status == safety_fail_status && len > 0
}
