// The core's log lines, as data (M7). A core call never logs: it pushes a
// record into its CoreOutput, and the shell prints the records through the
// Raft logger once the call returns (still under the guard, as the calls
// used to log). A record at a level above the output's threshold is dropped
// at the push, so a disabled debug line costs one comparison.

// srpc's levels (src/srpc/base/logging.rs), as the shell's logger numbers
// them: FATAL 0, ERROR 1, WARN 2, INFO 3, DEBUG 4.
pub const RAFT_LOG_ERROR: i32 = 1;
pub const RAFT_LOG_WARN: i32 = 2;
pub const RAFT_LOG_INFO: i32 = 3;
pub const RAFT_LOG_DEBUG: i32 = 4;

// The most arguments one line takes (the logger's own limit).
pub const CORE_LOG_MAX_ARGS: usize = 10;

// One argument of a log line. Integers are widened; strings are static.
#[derive(Clone, Copy)]
pub enum LogArg {
    U(u64),
    I(i64),
    B(bool),
    S(&'static str),
}

// `x.arg()`, so a call site does not spell the variant.
pub trait IntoLogArg {
    fn arg(self) -> LogArg;
}

impl IntoLogArg for u64 {
    fn arg(self) -> LogArg {
        LogArg::U(self)
    }
}

impl IntoLogArg for u32 {
    fn arg(self) -> LogArg {
        LogArg::U(self as u64)
    }
}

impl IntoLogArg for u16 {
    fn arg(self) -> LogArg {
        LogArg::U(self as u64)
    }
}

impl IntoLogArg for usize {
    fn arg(self) -> LogArg {
        LogArg::U(self as u64)
    }
}

impl IntoLogArg for i64 {
    fn arg(self) -> LogArg {
        LogArg::I(self)
    }
}

impl IntoLogArg for i32 {
    fn arg(self) -> LogArg {
        LogArg::I(self as i64)
    }
}

impl IntoLogArg for bool {
    fn arg(self) -> LogArg {
        LogArg::B(self)
    }
}

impl IntoLogArg for &'static str {
    fn arg(self) -> LogArg {
        LogArg::S(self)
    }
}

// One log line: level, format (the logger's `{}` / `{:x}` syntax) and its
// arguments, args[0..nargs].
#[derive(Clone, Copy)]
pub struct CoreLog {
    pub level: i32,
    pub fmt: &'static str,
    pub args: [LogArg; CORE_LOG_MAX_ARGS],
    pub nargs: usize,
}
