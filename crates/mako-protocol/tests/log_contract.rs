#[path = "support/log_contract.rs"]
mod log_contract;
use mako_protocol::{contracts::CommitLog, journal::MemoryLog};

#[test]
fn memory_log_obeys_the_common_contract_without_restart_durability() {
    let log: Box<dyn CommitLog> = Box::new(MemoryLog::new(0));
    log_contract::check(&*log, false);
    drop(log);
    let fresh: Box<dyn CommitLog> = Box::new(MemoryLog::new(0));
    assert!(!fresh.durable());
    assert!(fresh.entries().unwrap().is_empty());
}
