#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum NotifyRestartStatus {
    ACKNOWLEDGED = 0,
    PENDING = 1,
}

pub const fn commo_append_entries_empty_from_cmd(has_cmd: bool) -> bool {
    !has_cmd
}

pub const fn commo_append_entries_reply_lost(ok: u64,
                                             term: u64,
                                             last_log_index: u64) -> bool {
    ok == 0 && term == 0 && last_log_index == 0
}

pub const fn commo_append_entries_done_from_reply(ok: u64,
                                                  term: u64,
                                                  last_log_index: u64) -> bool {
    !commo_append_entries_reply_lost(ok, term, last_log_index)
}

pub const fn commo_future_failed(error_code: i32) -> bool {
    error_code != 0
}

pub const fn commo_notify_restart_is_pending(status: NotifyRestartStatus) -> bool {
    (status as i32) == (NotifyRestartStatus::PENDING as i32)
}

pub const fn commo_retry_has_pending_sites(pending_count: usize) -> bool {
    pending_count != 0
}

pub const fn commo_quorum_should_record_voter(voter_id: u16) -> bool {
    voter_id != 0
}

pub const fn commo_quorum_should_advance_term(candidate_term: i64,
                                               highest_term: i64) -> bool {
    candidate_term > highest_term
}
