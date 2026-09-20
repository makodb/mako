# Exact build and validation record

Both binaries were rebuilt from clean implementation commit
`6936abb30000d45c310226ceee0f6f7796b611ad` on `zoo-002`. The authoritative
C++ translation-unit invocation is in [`cpp-compile-command.json`](cpp-compile-command.json).
Its final optimization flag is `-O2` (it follows the earlier CMake `-O3`), with
`-march=native`, debug information, `NDEBUG`, and frame pointers.

The Rust binary was built with:

```text
CARGO_PROFILE_RELEASE_OPT_LEVEL=2
CARGO_PROFILE_RELEASE_DEBUG=1
CARGO_PROFILE_RELEASE_LTO=fat
CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1
RUSTFLAGS=-Dwarnings -C target-cpu=native -C force-frame-pointers=yes \
  -C link-arg=-Wl,-rpath,/home/users/shuai/.linuxbrew/opt/llvm@22/lib \
  -C link-arg=-Wl,--defsym=__rust_alloc_error_handler_should_panic=0
cargo build --locked --release -p sto-masstree --bin sto_masstree_compare
```

Native integration used:

```text
MAKO_MTREE_NATIVE_INTEGRATION=1
MAKO_MTREE_NATIVE_LIB_DIRS=/var/tmp/mako-sto-bench-20260827.J2TZj6/build:/var/tmp/mako-sto-bench-20260827.J2TZj6/build/src/masstree:/home/users/shuai/.linuxbrew/opt/llvm@22/lib
MAKO_MTREE_NATIVE_LIBS=static=mako,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread
```

The same-source/native-build validation commands were:

```text
ctest --test-dir /var/tmp/mako-sto-bench-20260827.J2TZj6/build \
  --output-on-failure -R '^(test_mtree_abi|test_mtree_abi_c11_header)$'
cargo test --locked -p masstree --test native_integration
cargo test --locked -p sto-masstree --test native_integration
```

All passed: two native ABI tests, four safe-Masstree integration tests, and six
transactional-Masstree integration tests. Cargo compiled separate debug test
binaries; the parent directory's smoke run exercised the exact release
benchmark binaries. Logs, linkage, ELF notes, toolchain versions, input hashes,
and the pre-run smoke are retained alongside this file or in the parent
artifact directory.
