//! S8a-0: does a C-linkage symbol survive clang's module purview?

#![allow(unsafe_code)]

/// Saved stack pointer. `#[repr(C)]` because the assembly indexes it.
#[repr(C)]
pub struct Ctx {
    pub sp: u64,
}

// The layout guard. Before 3bf2f547 this vanished from the C++.
const _: () = assert!(core::mem::size_of::<Ctx>() == 8);

// Rust supplies the symbol by assembling the shared file. On the C++
// side this drops — which is CORRECT: CMake assembles the same file.
// Rust's inline asm defaults to INTEL syntax; the shared file is AT&T
// (what the C++ toolchain assembles), so say so rather than maintaining
// two dialects of the same routine.
core::arch::global_asm!(include_str!("fiber_x86_64.S"), options(att_syntax));

extern "C" {
    /// Switch stacks. The seam: identical declaration on both sides.
    pub fn srpc_fiber_swap(from: *mut Ctx, to: *mut Ctx);
}

/// Raw pointers, not `&mut`, deliberately: Rust coerces `&mut T` to
/// `*mut T` implicitly at an FFI call, but C++ has no such conversion
/// from `T&` to `T*` — so a `&mut` wrapper transpiles to a hard error.
pub fn swap(from: *mut Ctx, to: *mut Ctx) {
    // @unsafe { the context switch itself }
    unsafe { srpc_fiber_swap(from, to) }
}
