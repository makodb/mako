#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(u8)]
pub enum SnapshotCompression {
    NONE = 0,
    SNAPPY = 1,
    ZSTD = 2,
}

#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(u8)]
pub enum SnapshotChecksumType {
    NONE = 0,
    CRC32 = 1,
    SHA256 = 2,
}

pub const fn snapshot_magic_valid(magic: u32) -> bool {
    magic == 0x504E4153
}

pub const fn snapshot_version_valid(version: u32) -> bool {
    version == 1
}

pub const fn snapshot_compression_supported(compression: u8) -> bool {
    compression == SnapshotCompression::NONE as u8
}

pub const fn snapshot_checksum_enabled(checksum_type: u8) -> bool {
    checksum_type == SnapshotChecksumType::CRC32 as u8
}

pub const fn snapshot_checksum_type_supported(checksum_type: u8) -> bool {
    checksum_type == SnapshotChecksumType::NONE as u8 ||
        checksum_type == SnapshotChecksumType::CRC32 as u8
}

pub const fn snapshot_checksum_size(checksum_type: u8) -> usize {
    if snapshot_checksum_enabled(checksum_type) {
        4
    } else {
        0
    }
}

pub const fn snapshot_header_size_valid(header_size: u32,
                                        expected_size: u32) -> bool {
    header_size == expected_size
}

pub const fn snapshot_payload_size_within_limit(data_size: u64,
                                                max_payload_size: u64) -> bool {
    data_size <= max_payload_size
}

pub const fn snapshot_serialized_size_fits(header_size: usize,
                                           data_size: usize,
                                           checksum_size: usize,
                                           max_size: usize) -> bool {
    header_size <= max_size &&
        data_size <= max_size - header_size &&
        checksum_size <= max_size - header_size - data_size
}

pub const fn snapshot_serialized_size_matches(header_size: usize,
                                              data_size: usize,
                                              checksum_size: usize,
                                              input_size: usize) -> bool {
    header_size <= input_size &&
        data_size <= input_size - header_size &&
        checksum_size == input_size - header_size - data_size
}

pub const fn snapshot_serialized_size_within_limit(input_size: usize,
                                                   max_size: usize) -> bool {
    input_size <= max_size
}

pub const fn snapshot_crc32_initial() -> u32 {
    0xFFFFFFFF
}

pub const fn snapshot_crc32_table_index(crc: u32, byte: u8) -> usize {
    ((crc ^ byte as u32) & 0xFF) as usize
}

pub const fn snapshot_crc32_update_from_table(crc: u32,
                                              table_value: u32) -> u32 {
    table_value ^ (crc >> 8)
}

pub const fn snapshot_crc32_finalize(crc: u32) -> u32 {
    crc ^ 0xFFFFFFFF
}

/// Fold `size` bytes at `data` into the CRC32 accumulator at `crc`.
///
/// # Safety
///
/// The caller must guarantee that `crc` points to one writable 32-bit
/// accumulator, that `data` is readable for `size` bytes, and that
/// `table` is readable for 256 32-bit entries. `data` MAY alias the
/// accumulator's own storage: that is deliberate, and is why each byte is
/// read before the accumulator is mutated instead of borrowing a slice.
pub const unsafe fn snapshot_crc32_update_buffer(crc: *mut u32,
                                                  data: *const u8,
                                                  size: usize,
                                                  table: *const u32) {
    let mut i: usize = 0;
    while i < size {
        let byte = unsafe { *data.add(i) };
        let table_index = snapshot_crc32_table_index(unsafe { *crc }, byte);
        let table_value = unsafe { *table.add(table_index) };
        let next_crc =
            snapshot_crc32_update_from_table(unsafe { *crc }, table_value);
        unsafe { *crc = next_crc };
        i += 1;
    }
}
