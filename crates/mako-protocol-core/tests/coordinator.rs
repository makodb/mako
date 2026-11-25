//! Faults have observable row, lock and log effects. These are contract
//! implementations, not another coordinator or a simulation of STO/Raft.
use mako_protocol_core::coordinator::*;

struct MemoryEffects {
    fault: Option<(&'static str, i32, bool)>,
    events: Vec<&'static str>,
    local: u8,
    remote: u8,
    local_lock: bool,
    remote_lock: bool,
    local_validated: bool,
    remote_validated: bool,
    proposal: u32,
    remote_proposal: u32,
    dependency: u32,
    decision: u32,
    logged: bool,
    fenced: bool,
}
impl MemoryEffects {
    fn new() -> Self {
        Self {
            fault: None,
            events: vec![],
            local: 0,
            remote: 0,
            local_lock: false,
            remote_lock: false,
            local_validated: false,
            remote_validated: false,
            proposal: 2,
            remote_proposal: 20,
            dependency: 30,
            decision: 0,
            logged: false,
            fenced: false,
        }
    }
    fn status(&mut self, event: &'static str) -> i32 {
        self.events.push(event);
        self.fault.filter(|f| f.0 == event).map_or(OK, |f| f.1)
    }
    fn takes_effect(&self, event: &str, status: i32) -> bool {
        status == OK || self.fault.is_some_and(|f| f.0 == event && f.2)
    }
}
impl Effects for MemoryEffects {
    fn flush(&mut self) -> i32 {
        self.remote_lock = true;
        self.status("flush")
    }
    fn lock(&mut self) -> i32 {
        self.local_lock = true;
        self.status("lock")
    }
    fn reserve(&mut self, minimum: u32) -> Result<u32, i32> {
        let status = self.status(if minimum == 0 {
            "reserve"
        } else {
            "reserve_after"
        });
        if status != OK {
            return Err(status);
        }
        let value = self.proposal.max(minimum.saturating_add(1));
        if value > MAX_TIMESTAMP {
            return Err(CAPACITY);
        }
        self.proposal = value + 1;
        Ok(value)
    }
    fn remote_timestamp(&mut self) -> Result<u32, i32> {
        let status = self.status("remote_timestamp");
        if status == OK {
            Ok(self.remote_proposal)
        } else {
            Err(status)
        }
    }
    fn read_dependency(&self) -> u32 {
        self.dependency
    }
    fn choose(&mut self, value: u32) {
        assert!(value > self.dependency && value >= self.remote_proposal);
        self.decision = value;
    }
    fn validate_local(&mut self) -> i32 {
        let status = self.status("validate_local");
        self.local_validated = status == OK;
        status
    }
    fn validate_remote(&mut self) -> i32 {
        let status = self.status("validate_remote");
        self.remote_validated = status == OK;
        status
    }
    fn abort_remote(&mut self) -> i32 {
        assert_eq!(
            (self.local, self.remote),
            (0, 0),
            "rollback after installed effects"
        );
        let status = self.status("abort_remote");
        if status == OK {
            self.remote_lock = false;
        }
        status
    }
    fn finish_abort(&mut self) -> i32 {
        assert_eq!(self.local, 0);
        let status = self.status("finish_abort");
        if status == OK {
            self.local_lock = false;
        }
        status
    }
    fn install_local(&mut self) -> i32 {
        assert!(
            self.local_validated && self.remote_validated && self.local_lock && self.remote_lock
        );
        let status = self.status("install_local");
        if self.takes_effect("install_local", status) {
            self.local = 1;
        }
        status
    }
    fn install_remote(&mut self) -> i32 {
        assert_eq!(self.local, 1);
        let status = self.status("install_remote");
        if self.takes_effect("install_remote", status) {
            self.remote = 1;
            self.remote_lock = false;
        }
        status
    }
    fn append_log(&mut self) -> i32 {
        assert_eq!((self.local, self.remote), (1, 1));
        let status = self.status("append_log");
        if self.takes_effect("append_log", status) {
            self.logged = true;
        }
        status
    }
    fn finish_commit(&mut self) -> i32 {
        assert!(self.logged);
        let status = self.status("finish_commit");
        self.local_lock = false;
        status
    }
    fn quarantine(&mut self) {
        self.events.push("quarantine");
        self.fenced = true;
    }
}

#[test]
fn successful_commit_publishes_both_rows_and_log_above_every_dependency() {
    let mut effects = MemoryEffects::new();
    assert_eq!(coordinate(&mut effects), OK);
    assert_eq!(
        (effects.local, effects.remote, effects.decision),
        (1, 1, 31)
    );
    assert!(effects.logged && !effects.local_lock && !effects.remote_lock && !effects.fenced);
}

#[test]
fn every_preinstallation_refusal_leaves_rows_unchanged_and_closes_both_sides() {
    for point in [
        "flush",
        "lock",
        "reserve",
        "remote_timestamp",
        "reserve_after",
        "validate_local",
        "validate_remote",
    ] {
        for code in [RETRY, CAPACITY, FATAL, UNKNOWN] {
            let mut effects = MemoryEffects::new();
            effects.fault = Some((point, code, false));
            assert_eq!(coordinate(&mut effects), code, "{point}");
            assert_eq!((effects.local, effects.remote), (0, 0), "{point}");
            assert!(!effects.local_lock && !effects.remote_lock, "{point}");
            assert!(!effects.events.contains(&"install_local") && !effects.logged);
        }
    }
}

#[test]
fn failures_before_or_after_an_install_effect_are_unknown_and_never_rolled_back() {
    for point in ["install_local", "install_remote", "append_log"] {
        for code in [RETRY, CAPACITY, FATAL, UNKNOWN] {
            for after in [false, true] {
                let mut effects = MemoryEffects::new();
                effects.fault = Some((point, code, after));
                assert_eq!(coordinate(&mut effects), UNKNOWN, "{point}, after={after}");
                assert!(effects.fenced);
                assert!(
                    !effects.events.contains(&"abort_remote")
                        && !effects.events.contains(&"finish_abort")
                );
                if point != "install_local" || after {
                    assert_eq!(effects.local, 1);
                }
                if point == "append_log" || (point == "install_remote" && after) {
                    assert_eq!(effects.remote, 1);
                }
            }
        }
    }
}

#[test]
fn timestamp_exhaustion_does_not_wrap_or_install() {
    let mut effects = MemoryEffects::new();
    effects.dependency = MAX_TIMESTAMP;
    assert_eq!(coordinate(&mut effects), CAPACITY);
    assert_eq!((effects.local, effects.remote, effects.decision), (0, 0, 0));
    assert!(!effects.local_lock && !effects.remote_lock && !effects.logged);
}

#[test]
fn a_backend_close_cannot_relabel_installed_rows_as_a_retryable_abort() {
    for code in [RETRY, CAPACITY, UNKNOWN, 1, 2, 4] {
        let mut effects = MemoryEffects::new();
        effects.fault = Some(("finish_commit", code, false));
        assert_eq!(coordinate(&mut effects), UNKNOWN);
        assert!(effects.fenced && effects.logged);
        assert_eq!((effects.local, effects.remote), (1, 1));
    }
    for code in [FATAL, COMMITTED_ERROR] {
        let mut effects = MemoryEffects::new();
        effects.fault = Some(("finish_commit", code, false));
        assert_eq!(coordinate(&mut effects), code);
        assert_eq!((effects.local, effects.remote), (1, 1));
        assert!(effects.logged);
    }
}
