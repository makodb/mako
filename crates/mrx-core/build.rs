use std::env;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rustc-check-cfg=cfg(mrx_sanitize_thread)");
    // Like crossbeam-utils, bridge the compiler's sanitizer cfg for stable
    // builds: direct cfg(sanitize = "thread") still requires a nightly feature.
    let sanitizers = env::var("CARGO_CFG_SANITIZE").unwrap_or_default();
    if sanitizers.split(',').any(|sanitizer| sanitizer == "thread")
        || env::var("TARGET").unwrap_or_default() == "x86_64-unknown-linux-gnutsan"
    {
        println!("cargo:rustc-cfg=mrx_sanitize_thread");
    }
}
