pub const fn log_entry_slot_precedes(slot_id: u64, other_slot_id: u64) -> bool {
    slot_id < other_slot_id
}

pub const fn log_entry_scalar_fields_equal(slot_equal: bool,
                                            term_equal: bool,
                                            max_seen_equal: bool,
                                            max_accepted_equal: bool,
                                            committed_equal: bool,
                                            no_op_equal: bool) -> bool {
    slot_equal && term_equal && max_seen_equal && max_accepted_equal &&
        committed_equal && no_op_equal
}

pub const fn log_entry_bool_to_i8(value: bool) -> i8 {
    if value { 1 } else { 0 }
}

pub const fn log_entry_i8_to_bool(value: i8) -> bool {
    value != 0
}
