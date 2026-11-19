//! Verus verification entry point for srpc.
//!
//! Each `#[path]` below points at a REAL srpc source file (no copy). This crate
//! forces `--cfg verus` (see ../.cargo/config.toml), so under `cargo verus
//! verify` the in-place `#[cfg(verus)]` specs those files carry are activated
//! and checked. Add a line here as more leaf modules gain specs.

#[allow(dead_code, unused_imports)]
#[path = "../../misc/stat.rs"]
mod stat;

#[allow(dead_code, unused_imports, non_camel_case_types)]
#[path = "../../base/basetypes.rs"]
mod basetypes;

#[allow(dead_code, unused_imports, non_camel_case_types)]
#[path = "../../rpc/errors.rs"]
mod errors;

#[allow(dead_code, unused_imports, non_upper_case_globals)]
#[path = "../../rpc/internal_protocol.rs"]
mod internal_protocol;

#[allow(dead_code, unused_imports, non_upper_case_globals)]
#[path = "../../rpc/frame_codec.rs"]
mod frame_codec;

// Verify-only proofs about the real internal_protocol functions above. Lives
// here, not in the srpc crate, so it can use in-body bit-vector proof code that
// the C++ transpiler does not carry.
mod internal_protocol_proofs;
mod errors_proofs;
mod basetypes_proofs;
mod frame_codec_proofs;

#[allow(unsafe_code)]
mod _stubs {
    #[unsafe(no_mangle)] pub extern "C" fn srpc_clock_monotonic_us() -> u64 { 0 }
    #[unsafe(no_mangle)] pub extern "C" fn srpc_clock_realtime_coarse_us() -> u64 { 0 }
    #[unsafe(no_mangle)] pub extern "C" fn srpc_gettimeofday_us() -> u64 { 0 }
    #[unsafe(no_mangle)] pub extern "C" fn srpc_sleep_us(_us: u64) {}
}

fn main() {}
