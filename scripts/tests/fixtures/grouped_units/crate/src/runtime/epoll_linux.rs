use crate::runtime::epoll::PollMode;

pub fn platform_mask(mode: PollMode) -> i32 {
    mode.0 | PollMode::WRITE.0
}
