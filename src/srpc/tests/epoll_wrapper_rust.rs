use srpc::epoll_wrapper::{PollMode, PollReady};

// The interest and readiness masks keep their C++ values.  The fd-keyed
// `Epoll` wrapper, the `Pollable` trait and the `epoll_remove_count`
// instrumentation this file also tested belonged to the retired 1 ms epoll
// loop and were deleted in S7b (docs/dev/lion-runtime-plan.md); the epoll
// seam that remains is the Lion OS backend, tested in epoll_backend_rust.rs
// and lion_os_backend_rust.rs, and PollableBase registrations are tested in
// pollable_proxy_rust.rs and pollthread_lion_rust.rs.
#[test]
fn poll_masks_match_the_cpp_surface() {
    assert_eq!(
        (PollMode::READ, PollMode::WRITE, PollMode::NO_CHANGE),
        (1, 2, -1)
    );
    assert_eq!(
        (PollReady::READABLE, PollReady::WRITABLE, PollReady::ERROR),
        (1, 2, 4)
    );
}
