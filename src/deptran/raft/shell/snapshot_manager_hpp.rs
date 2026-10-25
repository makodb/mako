pub const fn snapshot_metadata_is_valid(last_included_index: u64) -> bool {
    last_included_index > 0
}
