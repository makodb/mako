use rrr::logging::{log_level_tag, Log, LOG_LEVEL_S};
use std::sync::atomic::Ordering;

#[test]
fn severity_constants_tags_and_filter_state_are_exact() {
    assert_eq!(Log::FATAL, 0);
    assert_eq!(Log::ERROR, 1);
    assert_eq!(Log::WARN, 2);
    assert_eq!(Log::INFO, 3);
    assert_eq!(Log::DEBUG, 4);

    assert_eq!(log_level_tag(0), "F ");
    assert_eq!(log_level_tag(1), "E ");
    assert_eq!(log_level_tag(2), "W ");
    assert_eq!(log_level_tag(3), "I ");
    assert_eq!(log_level_tag(4), "D ");
    assert_eq!(log_level_tag(-1), "? ");
    assert_eq!(log_level_tag(5), "? ");

    Log::set_level(Log::WARN);
    assert_eq!(Log::level_now(), Log::WARN);
    assert_eq!(LOG_LEVEL_S.load(Ordering::Relaxed), Log::WARN);
    Log::set_level(Log::DEBUG);
}
