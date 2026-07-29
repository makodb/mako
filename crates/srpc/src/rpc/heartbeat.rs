//! Keepalive — the port of `src/rrr/rpc/heartbeat.cpp`.
//!
//! One ping is outstanding at a time. [`HeartbeatManager::should_send`]
//! says when the interval has elapsed; the caller sends and reports
//! back via [`HeartbeatManager::on_sent`]; a reply clears the state via
//! [`HeartbeatManager::on_pong`]. [`HeartbeatManager::check_timeout`]
//! is the periodic sweep that counts a missed pong and, after
//! `max_missed` of them, declares the connection dead — once. It stays
//! declared until [`HeartbeatManager::reset`].
//!
//! A missed ping does NOT immediately re-arm: `check_timeout` clears
//! the pending flag, so the next `should_send` is free to fire as soon
//! as the interval allows. That is the C++ behaviour, and it means the
//! effective detection time is roughly
//! `max_missed × max(interval, timeout)`.
//!
//! As with the circuit breaker, every clock-consulting method has an
//! `_at` twin so the state machine can be driven in tests without
//! sleeping.

use std::cell::Cell;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct HeartbeatConfig {
    pub enabled: bool,
    /// How often to ping.
    pub interval_ms: u32,
    /// How long to wait for a pong before counting it missed.
    pub timeout_ms: u32,
    /// Missed pongs before the connection is declared dead.
    pub max_missed: u32,
}

impl HeartbeatConfig {
    pub fn new() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: true,
            interval_ms: 10000,
            timeout_ms: 5000,
            max_missed: 3,
        }
    }

    pub fn defaults() -> HeartbeatConfig {
        HeartbeatConfig::new()
    }

    /// Detects failure fast, at the cost of more traffic.
    pub fn aggressive() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: true,
            interval_ms: 5000,
            timeout_ms: 2000,
            max_missed: 2,
        }
    }

    /// Tolerates long stalls before giving up.
    pub fn relaxed() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: true,
            interval_ms: 30000,
            timeout_ms: 15000,
            max_missed: 5,
        }
    }

    pub fn disabled() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: false,
            ..HeartbeatConfig::new()
        }
    }

    pub fn interval(&self) -> Duration {
        Duration::from_millis(self.interval_ms as u64)
    }

    pub fn timeout(&self) -> Duration {
        Duration::from_millis(self.timeout_ms as u64)
    }
}

impl Default for HeartbeatConfig {
    fn default() -> HeartbeatConfig {
        HeartbeatConfig::new()
    }
}

/// Keepalive state for one connection.
pub struct HeartbeatManager {
    config: HeartbeatConfig,
    /// `None` until the first ping — so "never sent" is representable
    /// rather than encoded as timestamp 0.
    last_send: Cell<Option<Instant>>,
    last_recv: Cell<Option<Instant>>,
    missed_count: Cell<u32>,
    pending_pong: Cell<bool>,
    timed_out: Cell<bool>,
}

impl HeartbeatManager {
    pub fn new(config: HeartbeatConfig) -> HeartbeatManager {
        HeartbeatManager {
            config,
            last_send: Cell::new(None),
            last_recv: Cell::new(None),
            missed_count: Cell::new(0),
            pending_pong: Cell::new(false),
            timed_out: Cell::new(false),
        }
    }

    pub fn config(&self) -> HeartbeatConfig {
        self.config
    }

    pub fn is_timed_out(&self) -> bool {
        self.timed_out.get()
    }

    pub fn missed_count(&self) -> u32 {
        self.missed_count.get()
    }

    pub fn is_pending_pong(&self) -> bool {
        self.pending_pong.get()
    }

    /// Whether a ping is due at `now`.
    ///
    /// The first call is due immediately: with nothing sent yet there is
    /// no evidence the peer is alive.
    pub fn should_send_at(&self, now: Instant) -> bool {
        if !self.config.enabled || self.timed_out.get() || self.pending_pong.get() {
            return false;
        }
        match self.last_send.get() {
            None => true,
            Some(sent) => now.saturating_duration_since(sent) >= self.config.interval(),
        }
    }

    pub fn should_send(&self) -> bool {
        self.should_send_at(Instant::now())
    }

    pub fn on_sent_at(&self, now: Instant) {
        if !self.config.enabled {
            return;
        }
        self.last_send.set(Some(now));
        self.pending_pong.set(true);
    }

    pub fn on_sent(&self) {
        self.on_sent_at(Instant::now());
    }

    /// A pong arrived: the peer is alive, so the missed streak and the
    /// timed-out verdict both clear.
    pub fn on_pong_at(&self, now: Instant) {
        if !self.config.enabled {
            return;
        }
        self.last_recv.set(Some(now));
        self.pending_pong.set(false);
        self.missed_count.set(0);
        self.timed_out.set(false);
    }

    pub fn on_pong(&self) {
        self.on_pong_at(Instant::now());
    }

    /// Periodic sweep. Returns `true` exactly once, on the sweep that
    /// declares the connection dead.
    pub fn check_timeout_at(&self, now: Instant) -> bool {
        if !self.config.enabled || self.timed_out.get() || !self.pending_pong.get() {
            return false;
        }
        let sent = match self.last_send.get() {
            Some(s) => s,
            None => return false,
        };
        if now.saturating_duration_since(sent) < self.config.timeout() {
            return false;
        }
        // The ping is written off; the next one may go out on schedule.
        self.pending_pong.set(false);
        let count = self.missed_count.get() + 1;
        self.missed_count.set(count);
        if count >= self.config.max_missed {
            self.timed_out.set(true);
            return true;
        }
        false
    }

    pub fn check_timeout(&self) -> bool {
        self.check_timeout_at(Instant::now())
    }

    /// How long until the next ping is due. Reports a full interval
    /// when no ping is pending-able (disabled, dead, or awaiting a
    /// pong), matching the C++ scheduler contract.
    pub fn time_until_next_ms_at(&self, now: Instant) -> u32 {
        if !self.config.enabled || self.timed_out.get() || self.pending_pong.get() {
            return self.config.interval_ms;
        }
        let sent = match self.last_send.get() {
            Some(s) => s,
            None => return 0,
        };
        let elapsed = now.saturating_duration_since(sent);
        let interval = self.config.interval();
        if elapsed >= interval {
            0
        } else {
            (interval - elapsed).as_millis() as u32
        }
    }

    pub fn time_until_next_ms(&self) -> u32 {
        self.time_until_next_ms_at(Instant::now())
    }

    /// Forget all keepalive state — used on reconnect.
    pub fn reset(&self) {
        self.last_send.set(None);
        self.last_recv.set(None);
        self.missed_count.set(0);
        self.pending_pong.set(false);
        self.timed_out.set(false);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mgr() -> HeartbeatManager {
        HeartbeatManager::new(HeartbeatConfig::new())
    }

    #[test]
    fn presets_match_the_documented_values() {
        let d = HeartbeatConfig::new();
        assert_eq!(d, HeartbeatConfig::defaults());
        assert_eq!(
            (d.interval_ms, d.timeout_ms, d.max_missed),
            (10000, 5000, 3)
        );

        let a = HeartbeatConfig::aggressive();
        assert_eq!((a.interval_ms, a.timeout_ms, a.max_missed), (5000, 2000, 2));

        let r = HeartbeatConfig::relaxed();
        assert_eq!(
            (r.interval_ms, r.timeout_ms, r.max_missed),
            (30000, 15000, 5)
        );

        assert!(!HeartbeatConfig::disabled().enabled);
        assert_eq!(d.interval(), Duration::from_millis(10000));
    }

    #[test]
    fn first_ping_is_due_immediately() {
        let m = mgr();
        let t = Instant::now();
        assert!(m.should_send_at(t), "nothing proves the peer is alive yet");
        assert_eq!(m.time_until_next_ms_at(t), 0);
    }

    #[test]
    fn interval_gates_the_next_ping() {
        let m = mgr();
        let t0 = Instant::now();
        m.on_sent_at(t0);
        assert!(m.is_pending_pong());
        assert!(!m.should_send_at(t0), "one ping outstanding at a time");

        m.on_pong_at(t0 + Duration::from_millis(10));
        assert!(!m.is_pending_pong());
        assert!(
            !m.should_send_at(t0 + Duration::from_millis(9_999)),
            "interval has not elapsed"
        );
        assert!(m.should_send_at(t0 + Duration::from_millis(10_000)));
    }

    #[test]
    fn time_until_next_counts_down() {
        let m = mgr();
        let t0 = Instant::now();
        m.on_sent_at(t0);
        m.on_pong_at(t0);
        assert_eq!(m.time_until_next_ms_at(t0), 10_000);
        assert_eq!(
            m.time_until_next_ms_at(t0 + Duration::from_millis(4_000)),
            6_000
        );
        assert_eq!(
            m.time_until_next_ms_at(t0 + Duration::from_millis(10_000)),
            0
        );
        assert_eq!(m.time_until_next_ms_at(t0 + Duration::from_secs(60)), 0);
    }

    #[test]
    fn a_pending_pong_reports_a_full_interval() {
        // The scheduler must not spin while a ping is outstanding.
        let m = mgr();
        let t0 = Instant::now();
        m.on_sent_at(t0);
        assert_eq!(
            m.time_until_next_ms_at(t0 + Duration::from_millis(9_000)),
            10_000
        );
    }

    #[test]
    fn missed_pongs_accumulate_then_declare_death_once() {
        let m = mgr();
        let mut t = Instant::now();
        let mut round = 0;
        while round < 2 {
            m.on_sent_at(t);
            t += Duration::from_millis(5_000); // the pong timeout
            assert!(!m.check_timeout_at(t), "not dead yet (round {round})");
            assert_eq!(m.missed_count(), round + 1);
            assert!(!m.is_pending_pong(), "the ping was written off");
            round += 1;
        }

        m.on_sent_at(t);
        t += Duration::from_millis(5_000);
        assert!(m.check_timeout_at(t), "third miss declares death");
        assert!(m.is_timed_out());

        // Reported exactly once, and the manager goes quiet afterwards.
        assert!(!m.check_timeout_at(t + Duration::from_secs(60)));
        assert!(!m.should_send_at(t + Duration::from_secs(60)));
    }

    #[test]
    fn a_pong_clears_the_missed_streak() {
        let m = mgr();
        let t0 = Instant::now();
        m.on_sent_at(t0);
        let t1 = t0 + Duration::from_millis(5_000);
        assert!(!m.check_timeout_at(t1));
        assert_eq!(m.missed_count(), 1);

        m.on_sent_at(t1);
        m.on_pong_at(t1 + Duration::from_millis(10));
        assert_eq!(m.missed_count(), 0, "a live peer resets the streak");
        assert!(!m.is_timed_out());
    }

    #[test]
    fn check_timeout_before_the_deadline_does_nothing() {
        let m = mgr();
        let t0 = Instant::now();
        m.on_sent_at(t0);
        assert!(!m.check_timeout_at(t0 + Duration::from_millis(4_999)));
        assert_eq!(m.missed_count(), 0);
        assert!(m.is_pending_pong(), "still waiting");
    }

    #[test]
    fn check_timeout_without_a_pending_ping_is_a_no_op() {
        let m = mgr();
        let t = Instant::now();
        assert!(!m.check_timeout_at(t));
        assert_eq!(m.missed_count(), 0);
    }

    #[test]
    fn aggressive_config_dies_after_two_misses() {
        let m = HeartbeatManager::new(HeartbeatConfig::aggressive());
        let mut t = Instant::now();
        m.on_sent_at(t);
        t += Duration::from_millis(2_000);
        assert!(!m.check_timeout_at(t));
        m.on_sent_at(t);
        t += Duration::from_millis(2_000);
        assert!(m.check_timeout_at(t), "two misses is the limit here");
    }

    #[test]
    fn disabled_manager_does_nothing() {
        let m = HeartbeatManager::new(HeartbeatConfig::disabled());
        let t = Instant::now();
        assert!(!m.should_send_at(t));
        m.on_sent_at(t);
        assert!(!m.is_pending_pong(), "state is not touched");
        assert!(!m.check_timeout_at(t + Duration::from_secs(3600)));
        assert!(!m.is_timed_out());
    }

    #[test]
    fn reset_re_arms_after_a_reconnect() {
        let m = mgr();
        let mut t = Instant::now();
        let mut round = 0;
        while round < 3 {
            m.on_sent_at(t);
            t += Duration::from_millis(5_000);
            m.check_timeout_at(t);
            round += 1;
        }
        assert!(m.is_timed_out());

        m.reset();
        assert!(!m.is_timed_out());
        assert_eq!(m.missed_count(), 0);
        assert!(!m.is_pending_pong());
        assert!(
            m.should_send_at(t),
            "due immediately, as on a fresh connection"
        );
    }
}
