// The election-timeout configuration, as one value. Every field is an
// environment override with a compiled-in default, read afresh on each call
// exactly as the four separate getters were.
#[repr(C)]
pub struct RaftElectionTimeouts {
    pub grace_period_us_: u64,
    pub preferred_us_: u64,
    pub non_preferred_grace_us_: u64,
    pub non_preferred_steady_us_: u64,
}

#[repr(C)]
pub struct RaftVoteOutcome {
    pub term_: i64,
    pub yes_: bool,
    pub no_: bool,
    pub n_voted_yes_: i32,
    pub n_voted_no_: i32,
    pub timeouted_: bool,
}

// One AppendEntries reply, read out of the wire response by
// raft_append_response_read for heartbeat phase 2 (server.cc).
#[repr(C)]
pub struct AppendRespView {
    pub completed_: bool,
    pub status_: bool,
    pub term_: u64,
    pub last_log_index_: u64,
}
