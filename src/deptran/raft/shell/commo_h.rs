pub const fn commo_append_entries_empty_from_cmd(has_cmd: bool) -> bool {
    !has_cmd
}

pub const fn commo_future_failed(error_code: i32) -> bool {
    error_code != 0
}

pub const fn commo_quorum_should_advance_term(candidate_term: i64,
                                               highest_term: i64) -> bool {
    candidate_term > highest_term
}
