pub const fn file_snapshot_advance_offset(offset: usize,
                                          size: usize) -> usize {
    offset.wrapping_add(size)
}

pub const fn file_snapshot_reader_bytes_to_read(data_size: usize,
                                                offset: usize,
                                                buffer_size: usize) -> usize {
    if offset >= data_size {
        0
    } else {
        let remaining = data_size.wrapping_sub(offset);
        if buffer_size < remaining {
            buffer_size
        } else {
            remaining
        }
    }
}

pub const fn file_snapshot_io_chunk_size(remaining: usize,
                                         max_io_size: usize) -> usize {
    if remaining < max_io_size {
        remaining
    } else {
        max_io_size
    }
}

pub const fn file_snapshot_write_fits_limit(offset: usize,
                                            size: usize,
                                            limit: usize) -> bool {
    offset <= limit && size <= limit - offset
}

pub const fn file_snapshot_reader_is_complete(valid: bool,
                                              data_size: usize,
                                              offset: usize) -> bool {
    valid && offset >= data_size
}

pub const fn file_snapshot_should_prune(snapshot_index: u64,
                                        keep_after_index: u64) -> bool {
    snapshot_index < keep_after_index
}

pub const fn file_snapshot_has_latest(snapshot_count: usize) -> bool {
    snapshot_count > 0
}

pub const fn file_snapshot_retention_required(snapshot_count: usize,
                                              max_snapshots: usize) -> bool {
    snapshot_count > max_snapshots
}
