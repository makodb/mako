//! Level-filtered logging — the port of `src/rrr/base/logging.cpp`.
//!
//! Same shape as the C++ side: an atomic level, a decorated line
//! (`<tag>[<file>:<line>] <timestamp> | <message>`), and a sink. Three
//! pieces the C++ file needed micro-kernels for are ordinary Rust here:
//! `log_basename` is `str::rsplit`, `log_time_now` is arithmetic on
//! [`crate::base::time::wall_us`], and `log_level_tag` is a `match`.
//!
//! Call sites use the macros ([`log_info!`] and friends), which capture
//! `file!()`/`line!()` and — importantly — do not format their
//! arguments unless the level is enabled.

use super::time;
use std::sync::atomic::{AtomicI32, Ordering};

/// Severity, ordered as the C++ enum: lower is more severe, and a
/// message is emitted when its level is `<=` the configured level.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
#[repr(i32)]
pub enum Level {
    Fatal = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4,
}

impl Level {
    pub fn tag(self) -> &'static str {
        match self {
            Level::Fatal => "F",
            Level::Error => "E",
            Level::Warn => "W",
            Level::Info => "I",
            Level::Debug => "D",
        }
    }

    pub fn from_i32(v: i32) -> Option<Level> {
        match v {
            0 => Some(Level::Fatal),
            1 => Some(Level::Error),
            2 => Some(Level::Warn),
            3 => Some(Level::Info),
            4 => Some(Level::Debug),
            _ => None,
        }
    }
}

static LEVEL: AtomicI32 = AtomicI32::new(Level::Info as i32);

pub fn set_level(level: Level) {
    LEVEL.store(level as i32, Ordering::Relaxed);
}

pub fn level_now() -> i32 {
    LEVEL.load(Ordering::Relaxed)
}

/// Whether `level` would be emitted. The macros check this before
/// formatting, so disabled logging costs one relaxed load.
pub fn enabled(level: Level) -> bool {
    (level as i32) <= level_now()
}

/// `src/rrr/rpc/client.cpp` -> `client.cpp` (the C++ `log_basename`).
fn basename(path: &str) -> &str {
    match path.rsplit_once('/') {
        Some((_, base)) => base,
        None => path,
    }
}

/// `YYYY-MM-DD HH:MM:SS.mmm` in UTC.
///
/// Written out rather than delegated to `localtime_r`: `chrono` is a
/// dependency this crate does not take, and the civil-from-days
/// conversion is a dozen lines of arithmetic. UTC rather than local
/// time is a deliberate change — log timestamps that shift with the
/// host's timezone are worse for correlating a distributed trace.
fn timestamp(now_us: u64) -> String {
    let secs = now_us / 1_000_000;
    let millis = (now_us % 1_000_000) / 1000;

    let days = (secs / 86400) as i64;
    let sod = secs % 86400;
    let (hh, mm, ss) = (sod / 3600, (sod % 3600) / 60, sod % 60);

    // Howard Hinnant's civil_from_days, shifted to a March-based year.
    let z = days + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = (z - era * 146097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };

    format!("{y:04}-{m:02}-{d:02} {hh:02}:{mm:02}:{ss:02}.{millis:03}")
}

/// Emit one decorated line. Prefer the macros; this is what they call.
pub fn log_line(level: Level, file: &str, line: u32, msg: &str) {
    if !enabled(level) {
        return;
    }
    let out = format!(
        "{}[{}:{}] {} | {}",
        level.tag(),
        basename(file),
        line,
        timestamp(time::wall_us()),
        msg
    );
    sink_write(&out);
}

/// Where decorated lines go.
///
/// One whole line per call, so `eprintln!` is both the idiomatic form
/// and enough: taking an explicit `stderr().lock()` guard only pays off
/// across repeated writes.
fn sink_write(line: &str) {
    eprintln!("{line}");
}

#[macro_export]
macro_rules! log_debug {
    ($($arg:tt)*) => {
        if $crate::base::log::enabled($crate::base::log::Level::Debug) {
            $crate::base::log::log_line(
                $crate::base::log::Level::Debug, file!(), line!(), &format!($($arg)*));
        }
    };
}

#[macro_export]
macro_rules! log_info {
    ($($arg:tt)*) => {
        if $crate::base::log::enabled($crate::base::log::Level::Info) {
            $crate::base::log::log_line(
                $crate::base::log::Level::Info, file!(), line!(), &format!($($arg)*));
        }
    };
}

#[macro_export]
macro_rules! log_warn {
    ($($arg:tt)*) => {
        if $crate::base::log::enabled($crate::base::log::Level::Warn) {
            $crate::base::log::log_line(
                $crate::base::log::Level::Warn, file!(), line!(), &format!($($arg)*));
        }
    };
}

#[macro_export]
macro_rules! log_error {
    ($($arg:tt)*) => {
        if $crate::base::log::enabled($crate::base::log::Level::Error) {
            $crate::base::log::log_line(
                $crate::base::log::Level::Error, file!(), line!(), &format!($($arg)*));
        }
    };
}

/// Logs at FATAL and aborts, like the C++ `Log_fatal`.
#[macro_export]
macro_rules! log_fatal {
    ($($arg:tt)*) => {{
        $crate::base::log::log_line(
            $crate::base::log::Level::Fatal, file!(), line!(), &format!($($arg)*));
        std::process::abort();
    }};
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn level_ordering_matches_cpp() {
        assert!((Level::Fatal as i32) < (Level::Error as i32));
        assert!((Level::Error as i32) < (Level::Warn as i32));
        assert!((Level::Warn as i32) < (Level::Info as i32));
        assert!((Level::Info as i32) < (Level::Debug as i32));
        assert_eq!(Level::from_i32(2), Some(Level::Warn));
        assert_eq!(Level::from_i32(9), None);
        assert_eq!(Level::Warn.tag(), "W");
    }

    #[test]
    fn filtering_follows_the_configured_level() {
        // Serialized with the other level-mutating test by running the
        // whole check under one setting at a time.
        set_level(Level::Warn);
        assert!(enabled(Level::Fatal));
        assert!(enabled(Level::Error));
        assert!(enabled(Level::Warn));
        assert!(!enabled(Level::Info));
        assert!(!enabled(Level::Debug));

        set_level(Level::Debug);
        assert!(enabled(Level::Debug));
        set_level(Level::Info);
    }

    #[test]
    fn basename_strips_directories() {
        assert_eq!(basename("src/rrr/rpc/client.cpp"), "client.cpp");
        assert_eq!(basename("client.cpp"), "client.cpp");
        assert_eq!(basename(""), "");
        assert_eq!(basename("a/b/"), "");
    }

    #[test]
    fn timestamp_formatting() {
        // 2021-01-01 00:00:00.000 UTC = 1609459200 s.
        assert_eq!(timestamp(1_609_459_200_000_000), "2021-01-01 00:00:00.000");
        // A leap day, with sub-second precision.
        assert_eq!(timestamp(1_582_934_896_123_000), "2020-02-29 00:08:16.123");
        assert_eq!(timestamp(0), "1970-01-01 00:00:00.000");
        // Milliseconds truncate rather than round.
        assert_eq!(timestamp(1_609_459_200_999_999), "2021-01-01 00:00:00.999");
    }

    #[test]
    fn line_decoration_shape() {
        // log_line writes to stderr, so assert the decoration by
        // rebuilding it the same way the function does.
        let ts = timestamp(1_609_459_200_000_000);
        let out = format!(
            "{}[{}:{}] {} | {}",
            Level::Info.tag(),
            basename("src/rrr/rpc/client.cpp"),
            42,
            ts,
            "hello"
        );
        assert_eq!(out, "I[client.cpp:42] 2021-01-01 00:00:00.000 | hello");
    }
}
