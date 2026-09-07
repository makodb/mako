#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(u8)]
pub enum ReplicatedDBOp {
    PUT = 1,
    DELETE = 2,
    BATCH = 3,
}

pub const fn replicated_db_op_is_batch(op: u8) -> bool {
    op == ReplicatedDBOp::BATCH as u8
}

pub const fn replicated_db_has_command_payload(has_value: bool) -> bool {
    has_value
}

pub const fn replicated_db_should_skip_applied(index: u64,
                                               last_applied_index: u64) -> bool {
    index <= last_applied_index
}

pub const fn replicated_db_command_kind_matches(kind: i32,
                                                expected_kind: i32) -> bool {
    kind == expected_kind
}

pub const fn replicated_db_required_value_missing(has_value: bool) -> bool {
    !has_value
}

pub const fn replicated_db_commit_succeeded(commit_state: i32) -> bool {
    commit_state > 0
}

pub const fn replicated_db_commit_pending(commit_state: i32) -> bool {
    commit_state == 0
}

pub const fn replicated_db_commit_callback_state(rolled_back: bool) -> i32 {
    if rolled_back {
        -1
    } else {
        1
    }
}

pub const fn replicated_db_commit_failed(commit_state: i32) -> bool {
    commit_state < 0
}

pub const fn replicated_db_apply_reached(applied_index: u64,
                                         target_index: u64) -> bool {
    applied_index >= target_index
}

pub const fn replicated_db_wait_timed_out(elapsed_us: u64,
                                          timeout_us: u64) -> bool {
    elapsed_us >= timeout_us
}

pub const fn replicated_db_read_found(has_value_ptr: bool) -> bool {
    has_value_ptr
}

pub const fn replicated_db_is_leader(is_leader: bool) -> bool {
    is_leader
}

pub const fn replicated_db_snapshot_has_header(size: usize) -> bool {
    size >= 1
}

pub const fn replicated_db_snapshot_is_lz4(compression: u8,
                                           lz4_tag: u8) -> bool {
    compression == lz4_tag
}

pub const fn replicated_db_snapshot_is_uncompressed(compression: u8,
                                                    uncompressed_tag: u8) -> bool {
    compression == uncompressed_tag
}

pub const fn replicated_db_snapshot_has_bytes(offset: usize,
                                              needed: usize,
                                              total: usize) -> bool {
    needed <= total && offset <= total - needed
}

pub const fn replicated_db_snapshot_has_u64_bytes(offset: u64,
                                                  needed: u64,
                                                  total: u64) -> bool {
    needed <= total && offset <= total - needed
}

pub const fn replicated_db_snapshot_size_within_limit(size: usize,
                                                      limit: usize) -> bool {
    size <= limit
}

pub const fn replicated_db_snapshot_file_count_is_valid(count: u64,
                                                        limit: u64) -> bool {
    count > 0 && count <= limit
}

pub const fn replicated_db_snapshot_index_matches(actual: u64,
                                                  expected: u64) -> bool {
    actual == expected
}
