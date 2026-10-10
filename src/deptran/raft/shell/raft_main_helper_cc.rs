#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(u8)]
pub enum RaftGroupMode {
    kSingleGroup = 0,
    kPerPartitionGroup = 1,
}

#[allow(dead_code)]
fn equals_ignore_case(lhs: &str, rhs: &str) -> bool {
    if lhs.len() != rhs.len() {
        return false;
    }
    let lhs_bytes = lhs.as_bytes();
    let rhs_bytes = rhs.as_bytes();
    let mut i: usize = 0;
    while i < lhs_bytes.len() {
        if unsafe { tolower(lhs_bytes[i] as i32) } !=
            unsafe { tolower(rhs_bytes[i] as i32) } {
            return false;
        }
        i += 1;
    }
    true
}

unsafe extern "C" {
    fn tolower(value: i32) -> i32;
}

#[allow(dead_code)]
fn is_raft_group_mode_arg(arg: &str) -> bool {
    arg == "--raft-groups" || arg.starts_with("--raft-groups=")
}
