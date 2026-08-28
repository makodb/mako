# Safe Masstree directory

The crate's default tests are Rust-only contract tests. They deliberately do
not link Mako: Cargo must not compile the template-heavy Masstree headers with
a configuration that can diverge from CMake.

The native integration test is enabled explicitly after CMake has built the
ABI shim as part of the normal `mako` target:

```sh
MAKO_MTREE_NATIVE_INTEGRATION=1 \
MAKO_MTREE_NATIVE_LIB_DIRS="/path/to/cmake-build:/path/to/cmake-build/src/masstree:/path/to/llvm/lib" \
MAKO_MTREE_NATIVE_LIBS="static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread" \
cargo test -p masstree --test native_integration
```

`MAKO_MTREE_NATIVE_LIB_DIRS` uses the platform path-list separator.
`MAKO_MTREE_NATIVE_LIBS` accepts the values understood by Rust's
`-l [kind=]name` option. A CMake-provided shared ABI target may instead reduce
the list to that one shared library. The exact static closure is
platform/build dependent; CMake remains authoritative.

Without `MAKO_MTREE_NATIVE_INTEGRATION=1`, even `cargo test --all-features`
does not require native symbols.

For fixed-width point-read batches, `Tree::get_fixed` is the default one-shot
API: it validates the tree and worker once, keeps one native structural/RCU
region around the lookup loop, and releases every native guard before it
returns. Its result `Vec` is resized in place and can be reused across calls.
Use `Tree::read_scope` and `ReadScope::get_fixed` only when several separate
calls must deliberately share one longer native read scope; ordinary worker
operations remain blocked until that scope is closed or dropped.
