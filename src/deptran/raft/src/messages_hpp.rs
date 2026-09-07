#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub last_log_idx: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub last_log_term: i64,
    #[cfg_attr(any(), cpp_value_init)]
    pub candidate_site_id: u16,
    #[cfg_attr(any(), cpp_value_init)]
    pub current_term: i64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub max_ballot: i64,
    #[cfg_attr(any(), cpp_value_init)]
    pub vote_granted: bool,
}

// VoteDurable — sent by a voter once its vote has been persisted.
#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteDurableReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub term: i64,
    #[cfg_attr(any(), cpp_value_init)]
    pub voter_id: u16,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteDurableReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub acknowledged: bool,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct AppendEntriesReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_append_ok: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_current_term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_last_log_index: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_ack_type: u64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct EmptyAppendEntriesReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub slot: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub ballot: i64,
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_current_term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_site_id: u16,
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_prev_log_index: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_prev_log_term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_commit_index: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub trigger_election_now: bool,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct EmptyAppendEntriesReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_append_ok: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_current_term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_last_log_index: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_ack_type: u64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct AppendEntriesDurableReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub term: i64,
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_id: u16,
    #[cfg_attr(any(), cpp_value_init)]
    pub last_log_index: u64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct AppendEntriesDurableReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub acknowledged: bool,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct TimeoutNowReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub leader_site_id: u16,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct TimeoutNowReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub follower_term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub success: bool,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct NotifyRestartReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub restarted_site_id: u16,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct NotifyRestartReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub acknowledged: bool,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct InstallSnapshotReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub term_out: u64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct RemoveServerReq {
    #[cfg_attr(any(), cpp_value_init)]
    pub term: u64,
    #[cfg_attr(any(), cpp_value_init)]
    pub server_id: u64,
}
