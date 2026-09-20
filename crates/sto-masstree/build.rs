use std::env;

const ENABLE: &str = "MAKO_MTREE_NATIVE_INTEGRATION";

fn main() {
    println!("cargo:rustc-check-cfg=cfg(mtree_native_integration)");
    println!("cargo:rerun-if-env-changed={ENABLE}");
    if env::var(ENABLE).ok().as_deref() == Some("1") {
        // The `masstree` dependency's build script owns the corresponding
        // native search paths and link libraries. This package only needs the
        // same explicit integration-test cfg.
        println!("cargo:rustc-cfg=mtree_native_integration");
    }
}
