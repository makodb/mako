// raft-core: the Raft core that Verus checks (plan Phase 6). Plain Rust in
// the Verus subset -- no unsafe, FFI, locks, atomics, hashing, logging or
// clock reads (plan §3.1). The shell (src/deptran/raft/shell, rt/) holds the
// rest and calls in.
#![forbid(unsafe_code)]

// The platform: x86-64, where usize is 64 bits. In a module of its own so
// the allow covers only what the erased declaration expands to.
#[allow(unused_braces)]
mod platform {
    vstd::prelude::verus! {
        global size_of usize == 8;
    }
}

pub mod helpers;
pub mod logging;
pub mod log;
pub mod node;
pub mod heartbeat;
pub mod progress;
pub mod pending;
pub mod authority;
pub mod output;
pub mod election;
pub mod event;
// [M12] the proof side: Verus only (scripts/verus/verify_core.sh)
#[cfg(verus_keep_ghost)]
pub mod coupling;

pub use helpers::*;
pub use logging::*;
pub use log::*;
pub use node::*;
pub use heartbeat::*;
pub use progress::*;
pub use pending::*;
pub use authority::*;
pub use output::*;
pub use election::*;
pub use event::*;
