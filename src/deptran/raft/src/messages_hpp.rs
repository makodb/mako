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
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct InstallSnapshotReply {
    #[cfg_attr(any(), cpp_value_init)]
    pub term_out: u64,
}
