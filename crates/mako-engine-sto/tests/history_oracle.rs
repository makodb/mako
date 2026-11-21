//! Forced concurrent histories over the native Rust STO engine, checked by
//! `mako-history`'s strict-serializability oracle. The scenarios are the
//! ones the C ABI engine runs, through `mako-differential`.

use mako_differential::history::forced_histories_pass_the_advertised_profile;
use mako_engine_sto::{features, LocalDb};

#[test]
fn forced_rw_ww_and_phantom_histories_pass_the_advertised_profile() {
    let capabilities = features().expect("engine capabilities");
    forced_histories_pass_the_advertised_profile::<LocalDb>(capabilities);
}
