pub const fn raft_quorum_reached(received: i32, needed: i32) -> bool {
    received >= needed
}

pub const fn raft_quorum_majority_count(total: usize) -> usize {
    (total / 2) + 1
}

pub const fn raft_quorum_count_reached(count: usize, quorum: usize) -> bool {
    count >= quorum
}

pub const fn raft_quorum_count_below(count: usize, quorum: usize) -> bool {
    count < quorum
}
