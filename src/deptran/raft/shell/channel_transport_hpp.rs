pub const fn channel_faults_drop_matches(drop_from: u16,
                                         drop_to: u16,
                                         from: u16,
                                         to: u16) -> bool {
    drop_from == from && drop_to == to
}

pub const fn channel_faults_partitions_block(from_partition: i32,
                                             to_partition: i32) -> bool {
    from_partition != to_partition
}

pub const fn channel_site_matches(lhs: u16, rhs: u16) -> bool {
    lhs == rhs
}
