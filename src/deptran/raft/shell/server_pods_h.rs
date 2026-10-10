// The C types of the Raft kernel boundary. Canonical Rust: rustc compiles
// this for the Rust lanes; the C++ lane binds each struct to the native
// declaration in ../raft_kernel_pods.h (`cpp_native_type`), which the host
// includes too, so all three worlds share one layout.
//
// These must stay impl-free `#[repr(C)]` structs of scalar fields: that is
// both what `cpp_native_type` accepts and what makes the layout a C fact
// rather than a Rust one.

// The election-timeout configuration, as one value. Every field is an
// environment override with a compiled-in default, read afresh on each call
// exactly as the four separate getters were.
#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct RaftElectionTimeouts {
    pub grace_period_us_: u64,
    pub preferred_us_: u64,
    pub non_preferred_grace_us_: u64,
    pub non_preferred_steady_us_: u64,
}

#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct RaftVoteOutcome {
    pub term_: i64,
    pub yes_: bool,
    pub no_: bool,
    pub n_voted_yes_: i32,
    pub n_voted_no_: i32,
    pub timeouted_: bool,
}

// One AppendEntries reply, read out of the wire response by
// raft_append_response_read for heartbeat phase 2 (server.cc).
#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct AppendRespView {
    pub completed_: bool,
    pub status_: bool,
    pub term_: u64,
    pub last_log_index_: u64,
}

// The core's RaftServerBase as a kernel receives it: an address, never a
// layout. Kernels are declared over this rather than `*mut RaftServerBase`
// so their C declarations name only global types; RaftServerBase::handle()
// is the one cast.
#[repr(C)]
#[cfg_attr(any(), cpp_native_type)]
pub struct RaftServerHandle {
    _opaque: [u8; 0],
}
