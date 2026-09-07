pub const fn memory_snapshot_advance_offset(offset: usize,
                                            size: usize) -> usize {
    offset.wrapping_add(size)
}

pub const fn memory_snapshot_reader_bytes_to_read(payload_size: usize,
                                                  offset: usize,
                                                  buffer_size: usize) -> usize {
    let remaining = payload_size.wrapping_sub(offset);
    if buffer_size < remaining {
        buffer_size
    } else {
        remaining
    }
}

pub const fn memory_snapshot_reader_is_complete(payload_size: usize,
                                                offset: usize) -> bool {
    offset >= payload_size
}
