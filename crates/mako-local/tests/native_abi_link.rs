#![cfg(have_mako)]

// This is deliberately an integration target: it must consume the generated
// declarations exactly as an external Rust crate does, then force a relocation
// for every revision-0 function in the native archive.
#[test]
fn every_generated_abi_export_links_and_identity_matches() {
    mako_local_sys::mako_local_link_probe_all_exports!();
    mako_local::verify_abi().expect("generated Rust ABI must match linked native build");
}
