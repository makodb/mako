pub const fn rocksdb_log_storage_is_closed(is_open: bool) -> bool {
    !is_open
}

pub const fn rocksdb_log_storage_missing_db(has_db: bool) -> bool {
    !has_db
}

pub const fn rocksdb_log_range_valid(start: u64, end: u64) -> bool {
    start < end
}

pub const fn rocksdb_log_empty_from_size(size: usize) -> bool {
    size == 0
}

pub const fn rocksdb_log_value_present(has_value: bool) -> bool {
    has_value
}
