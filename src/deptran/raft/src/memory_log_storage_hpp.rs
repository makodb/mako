pub const fn memory_log_storage_is_usable(is_open: bool) -> bool {
    is_open
}

pub const fn memory_log_storage_range_valid(start: u64, end: u64) -> bool {
    start < end
}

pub const fn memory_log_storage_empty_from_size(size: usize) -> bool {
    size == 0
}

pub const fn memory_log_storage_index_or_zero(has_entry: bool,
                                              index: u64) -> u64 {
    if has_entry {
        index
    } else {
        0
    }
}

pub const fn memory_log_storage_metadata_found(found: bool) -> bool {
    found
}
