//! Item 3 of the boundary gates: forced concurrent histories over the C ABI
//! engine, checked by `mako-history`'s oracle. The scenarios are shared with
//! the native Rust STO engine through `mako-differential`.
#![cfg(have_mako)]

use mako_differential::history::forced_histories_pass_the_advertised_profile;
use mako_local::{features, LocalDb};

#[test]
fn forced_rw_ww_and_phantom_histories_pass_the_advertised_profile() {
    let capabilities = features().expect("query native features");
    forced_histories_pass_the_advertised_profile::<LocalDb>(capabilities);
}
