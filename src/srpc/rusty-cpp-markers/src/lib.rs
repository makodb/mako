//! Rustc-visible inert attributes consumed by rusty-cpp code generation.

use proc_macro::TokenStream;

/// Request direct C++ inheritance for an interface-trait implementation.
#[proc_macro_attribute]
pub fn cpp_inherit(_attribute: TokenStream, item: TokenStream) -> TokenStream {
    item
}
