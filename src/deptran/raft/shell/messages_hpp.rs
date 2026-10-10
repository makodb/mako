#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteReq {
    pub last_log_idx: u64,
    pub last_log_term: i64,
    pub candidate_site_id: u16,
    pub current_term: i64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct VoteReply {
    pub max_ballot: i64,
    pub vote_granted: bool,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct AppendEntriesReply {
    pub follower_append_ok: u64,
    pub follower_current_term: u64,
    pub follower_last_log_index: u64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct EmptyAppendEntriesReq {
    pub slot: u64,
    pub ballot: i64,
    pub leader_current_term: u64,
    pub leader_site_id: u16,
    pub leader_prev_log_index: u64,
    pub leader_prev_log_term: u64,
    pub leader_commit_index: u64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct EmptyAppendEntriesReply {
    pub follower_append_ok: u64,
    pub follower_current_term: u64,
    pub follower_last_log_index: u64,
}

#[cfg_attr(any(), cpp_no_auto_traits)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
#[repr(C)]
pub struct InstallSnapshotReply {
    pub term_out: u64,
}
