# Native SRPC development

SRPC is a native Rust library. Build it through Cargo, with the Lion Rust
runtime in `third-party/lion`. There is no C++ companion build or transpiler
provider in this checkout. The workspace toolchain pin applies.

Run builds and tests inside Mako's build container:

```sh
cargo test --locked --manifest-path src/srpc/Cargo.toml --workspace --all-targets
cargo test --locked --manifest-path src/srpc/Cargo.toml --workspace --doc
bash ci/native_rust.sh srpc
```

Rust owns RPC framing, marshalling, reactor policy, connection management,
retries, deadlines and service dispatch. Small C platform bindings own OS
layouts, errno, clock/entropy operations and context-switch storage. Architecture
assembly saves and restores registers. Cargo's `build.rs` compiles exactly the
sources in `scripts/native-kernel-sources.txt`.

`check_native_kernels.py` pins the reviewed native files and build consumer.
Its negative controls reject new C/C++ carriers, changed kernels and a restored
CMake build. Changes to native code require an actual review of ownership and
an explicit inventory update; there is no blind regeneration command.

`check_rust_independence.py` copies the native Cargo inputs to a clean directory,
rejects C++ build tools and runs the Rust workspace's tests and doctests. Keep
that gate independent of Mako's storage and legacy build directories.
`rust_source_audit` retains Rust AST policy checks. Historical `cpp_module`
names in the Rust source inventory are logical identifiers, not C++ providers.

Use `README.md` and `docs/srpc-book.md` for APIs. The C++ companion book and
older reports are historical records of the retired build.
