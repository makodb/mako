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

Use `Worker::rcu_scope` for a short transaction-shaped sequence that spans
multiple trees or includes inserts/scans. It retains only worker-wide native
RCU lifetime protection; each tree operation keeps its normal structural
admission but reuses the validated outer RCU region, so the scope is neither a
snapshot nor a structural lock. Calls outside such a scope retain their local
native RCU guard. The scope is RAII-owned, worker-affine, and must not cross
blocking work, I/O, or asynchronous suspension.

Packed range scans have two allocation policies. `Tree::scan_packed_chunk`
retains the simple owned result, while `Tree::scan_packed_chunk_reusing` fills
a caller-owned `PackedScanScratch` and returns a validated borrowed chunk. The
scratch grows its descriptor and key-arena buffers on demand and neither
allocates nor clears them on later calls at the same or smaller capacities.
The borrowed keys and resume key must be consumed or copied before reusing the
scratch. Both APIs have identical bounds, ordering, stop, and resume semantics.

## Hidden native fast lane

The safe facade may call statically linked `mako_mtree_*_trusted` entry points
while its owned wrappers retain handle lifetime and after it has validated
runtime/worker pairing, key and enum shape, slice relationships, and output
capacity. `Worker` is `!Send + !Sync`, so safe Rust statically preserves
current-thread ownership; the implementation performs a dynamic thread-ID
assertion only in debug builds. Caller-provided raw storage still has to meet
the documented lifetime and non-aliasing preconditions. These hidden symbols
are not part of the versioned public `mt_*` ABI, feature negotiation, or the
44-symbol export fingerprint. They remain behind the facade and must not be
called by application or foreign code; violating their preconditions may be
undefined behavior.

The native side still owns runtime poison and active-scope checks, structural
admission, RCU protection, C++ exception containment, and exact insertion
publication classification. Trusted scan decoding also validates every count,
offset, and length before creating Rust borrows. The fast lane removes repeated
boundary checks, not the concurrency, lifetime, or failure protocol.
