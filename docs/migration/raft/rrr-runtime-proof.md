# Proof: the compiled `rrr` artifact, and the rules it obeys

Every command below was run on this host against the tree at `80433a59d`
(branch `codex/raft-vote-site-id`) with the existing `build/` directory.
Nothing in the source tree was left modified; §C's experiment was reverted
and verified byte-identical.

Short version: the artifact is `build/src/rrr/librrr.a`; it is a C++ static
library built by clang from C++20 module files that the rusty-cpp transpiler
generates from the Rust sources on every build; and it obeys the C++ ABI and
C++ object model end to end, with no Rust runtime in it.

---

## A. The artifact

```
$ ls -la build/src/rrr/librrr.a
-rw-rw-r-- 1 zyang2 zyang2 30424548 Sep 21 10:57 build/src/rrr/librrr.a

$ file build/src/rrr/librrr.a
build/src/rrr/librrr.a: current ar archive

$ ar t build/src/rrr/librrr.a | wc -l
50
```

The 50 members break down exactly as the source tree does:

| members | what they are |
|---|---|
| 37 × `rrr.<module>.cppm.o` | one per canonical Rust module — `rrr.client.cppm.o`, `rrr.server.cppm.o`, `rrr.reactor.cppm.o`, … |
| 2 × `std.cppm.o`, `std.compat.cppm.o` | libc++'s own `std` module |
| 8 × `srpc_*.c.o` | the hand-written C kernels (fiber engine, sockets, timing, rand, io) |
| 2 × `fiber_context_{x86_64,aarch64}.S.o` | the stack-switch assembly |
| 1 × `epoll_platform_linux.cc.o` | the one hand-written `.cc` |

There is **no** `client.cpp.o`, `server.cpp.o` or `reactor.cpp.o`. The
hand-authored C++ carriers those names refer to were deleted, and
`src/rrr/CMakeLists.txt` fails the configure if any is reintroduced:

```cmake
foreach(_RRR_GOAL0_RETIRED_CARRIER IN LISTS RRR_GOAL0_RETIRED_CARRIER_SRC)
    if(EXISTS "${_RRR_GOAL0_RETIRED_CARRIER}")
        message(FATAL_ERROR
            "Goal-0 retired inline carrier was reintroduced: ...")
```

## B. The build rules, from ninja's own dependency graph

Three links. Each is `ninja -t query` output, not a reading of the CMake.

**B1. Rust source → generated C++.**

```
$ ninja -C build -t query src/rrr/goal0-crate-cpp/rrr.client.cppm
src/rrr/goal0-crate-cpp/rrr.client.cppm:
  input: CUSTOM_COMMAND
    /home/users/zyang2/mako/src/rrr/Cargo.toml
    /home/users/zyang2/mako/src/rrr/rust-modules.toml
    /home/users/zyang2/mako/src/rrr/src/lib.rs
    /home/users/zyang2/mako/src/rrr/base/basetypes.rs
    /home/users/zyang2/mako/src/rrr/rpc/client.rs
    ... (all 37 canonical .rs sources)
```

and the command that custom rule runs
(`ninja -t commands src/rrr/goal0-crate-cpp/rrr.client.cppm`):

```
third-party/rusty-cpp/target/release/rusty-cpp-transpiler \
    --crate       src/rrr/Cargo.toml \
    --output-dir  build/src/rrr/goal0-crate-cpp \
    --cxx-namespace       rrr \
    --flat-import-namespace rrr \
    --module-preamble     src/rrr/module-preambles.toml \
    --type-map            src/rrr/rust-type-map.toml \
    --cpp-module-index    src/rrr/cpp-module-index.toml
```

Note the output directory: `build/`. The generated C++ is not in the source
tree and is not in git.

**B2. Generated C++ → object file.**

```
$ ninja -C build -t query src/rrr/CMakeFiles/rrr.dir/goal0-crate-cpp/rrr.client.cppm.o
src/rrr/CMakeFiles/rrr.dir/goal0-crate-cpp/rrr.client.cppm.o:
  input: CXX_COMPILER__rrr_scanned_Release
    /home/users/zyang2/mako/build/src/rrr/goal0-crate-cpp/rrr.client.cppm
    | ... modmap, vec_port.vec.pcm, std_port.pcm, rrr.basetypes.pcm
```

and its command:

```
/home/users/zyang2/.local/opt/llvm/bin/clang++ -stdlib=libc++ -std=gnu++23 \
    ... -c build/src/rrr/goal0-crate-cpp/rrr.client.cppm \
    -o  src/rrr/CMakeFiles/rrr.dir/goal0-crate-cpp/rrr.client.cppm.o
```

`clang++`. `-std=gnu++23`. Not `rustc`.

**B3. Object files → archive.**

```
$ ninja -C build -t query src/rrr/librrr.a
src/rrr/librrr.a:
  input: CXX_STATIC_LIBRARY_LINKER__rrr_Release
    ... 50 object files
```

So the whole chain is:

```
src/rrr/rpc/client.rs
      │  rusty-cpp-transpiler --crate
      ▼
build/src/rrr/goal0-crate-cpp/rrr.client.cppm     (C++20 module interface)
      │  clang++ -std=gnu++23 -c
      ▼
build/src/rrr/CMakeFiles/rrr.dir/.../rrr.client.cppm.o
      │  ar
      ▼
build/src/rrr/librrr.a
```

## C. Live proof that the Rust is the source and the C++ is the output

Before:

```cpp
// build/src/rrr/goal0-crate-cpp/rrr.client.cppm:7716
void Client::set_timeout(uint64_t v) const {
    this->timeout_field.set(std::move(v));
}
```

One edit, to the **Rust only** (`src/rrr/rpc/client.rs:1435`):

```rust
-fn set_timeout(&self, v: u64) { self.timeout_field.set(v); }
+fn set_timeout(&self, v: u64) { self.timeout_field.set(if v == 0u64 { 1u64 } else { v }); }
```

Re-running the B1 command (`Done: 38 files transpiled, 0 errors`), with no
`.cpp`, `.cppm` or `.hpp` touched by hand:

```cpp
// build/src/rrr/goal0-crate-cpp/rrr.client.cppm:7716
void Client::set_timeout(uint64_t v) const {
    this->timeout_field.set((rusty::detail::deref_if_pointer_like(v)
        == static_cast<uint64_t>(0) ? static_cast<uint64_t>(1) : v));
}
```

Reverting `client.rs` and regenerating restores the file exactly:

```
$ diff -q before.cppm build/src/rrr/goal0-crate-cpp/rrr.client.cppm
(no output — byte-identical)
$ git status --porcelain
(no modified tracked files)
```

## D. What rules the artifact obeys: C++, not Rust

All against `A=build/src/rrr/librrr.a`.

**D1. Itanium C++ ABI mangling — 1,457 symbols.**

```
$ nm --defined-only $A | grep -c '_ZN3rrr'
1457
$ nm --defined-only $A | grep -oE '_ZN3rrr[A-Za-z0-9_]+' | head -3 | c++filt
rrr::epoll_open@rrr.epoll_wrapper()
rrr::epoll_add_impl@rrr.epoll_wrapper(int, int, int)
rrr::epoll_remove_impl@rrr.epoll_wrapper(int, int)
```

The `@rrr.epoll_wrapper` suffix is C++20 *module attachment* in the Itanium
ABI — these are C++ module-linkage symbols.

**D2. C++ object lifetime — 236 C++ destructors (D0 / D1 / D2 forms).**

```
$ nm --defined-only $A | grep -cE '_ZN3rrr[A-Za-z0-9_]*D[012]Ev'
236
$ ... | c++filt | head -2
rrr::BtCapture@rrr.debugging::~BtCapture()
rrr::AddrInfo@rrr.utils::~AddrInfo()
```

Rust `Drop` glue does not appear in a binary as `D0Ev`/`D1Ev`/`D2Ev`. These
are C++ destructors, run by C++ scope rules.

**D3. C++ virtual dispatch and RTTI.**

```
vtables  (_ZTV…) : 161
typeinfo (_ZTI/_ZTS): 450
$ nm --defined-only $A | grep -oE '_ZTVN3rrr[A-Za-z0-9_]+' | head -3 | c++filt
vtable for rrr::SourceBase@rrr.serializable
vtable for rrr::Deserialize@rrr.serializable
vtable for rrr::SinkBaseAdapter@rrr.serializable<rrr::BufferSink@rrr.serializable>
```

**D4. C++ exceptions and C++ static-initialization guards.**

```
$ nm $A | grep -c '__gxx_personality_v0'
44
$ nm $A | grep -oE '__cxa_[a-z_]+' | sort -u
__cxa_allocate_exception  __cxa_atexit  __cxa_begin_catch  __cxa_end_catch
__cxa_free_exception  __cxa_guard_abort  __cxa_guard_acquire  __cxa_guard_release
```

Itanium EH personality, C++ throw/catch machinery, C++ magic-static guards.
For contrast, the rustc-compiled Raft crate is built `panic = "abort"` and
has none of this.

**D5. It links the C++ standard library.**

```
$ nm -u $A | grep -oE '_ZNSt[0-9]+[A-Za-z_]+|_ZNKSt[0-9]+[A-Za-z_]+' | sort -u | head
_ZNKSt13runtime_error   _ZNKSt3__   _ZNSt11logic_errorC
_ZNSt11logic_errorD     _ZNSt12length_errorD  _ZNSt12out_of_rangeD
```

**D6. There is no Rust runtime in it.**

```
$ nm $A | grep -cE ' _R[A-Za-z0-9_]{8,}'          # Rust v0 mangled symbols
0
$ nm $A | grep -cE 'rust_begin_unwind|core::panicking|_ZN4core|rust_eh_personality'
0
```

## E. The semantics genuinely differ — measured, not asserted

**E1. A Rust trait becomes a C++ abstract class with a vptr.**

Rust (`src/rrr/misc/serializable.rs:41`):

```rust
pub trait SourceBase {
    unsafe fn read_bytes(&mut self, p: *mut u8, n: usize) -> usize;
}
pub type SourceProxy = Box<dyn SourceBase>;
```

Generated C++ (`rrr.serializable.cppm:597`):

```cpp
export class SourceBase {
public:
    virtual ~SourceBase() noexcept(false) {}
    virtual size_t read_bytes(uint8_t* p, size_t n) = 0;
    SourceBase(const SourceBase&) = delete;
    ...
};
export using SourceProxy = rusty::Box<SourceBase>;
```

A Rust `dyn Trait` is a fat pointer (data + vtable, two words, vtable not in
the object). The C++ version is a vptr **inside** the object, and the archive
carries `vtable for rrr::SourceBase@rrr.serializable`. Different object
model, same source.

Note also `noexcept(false)` on the destructor: a C++ destructor that may
throw. Rust's `Drop` cannot.

**E2. Every transpiled struct carries a field no Rust struct has.**

`vec_port.vec.cppm:4596`:

```cpp
export template<typename T, typename A = rusty::alloc::Global>
struct Vec {
    RawVec<T, A> buf;
    size_t len_field;
    mutable bool _rusty_forgotten = false;   // <-- no Rust equivalent
    Vec(const Vec& other) : Vec(other.clone()) {}   // deep copy; Rust Vec is not Copy
    ...
```

`_rusty_forgotten` is bookkeeping the C++ runtime needs because C++ has no
move-out-of-value; Rust's ownership tracking is in the compiler, not in the
object.

**E3. Layouts, measured on this host.**

C++ side by compile-time probe against the built BMI
(`template<size_t N> struct SIZE_IS; SIZE_IS<sizeof(T)> x;` and reading the
diagnostic), Rust side by `rustc -O` + `size_of`:

| type | C++ (transpiled) | Rust | same? |
|---|---|---|---|
| `Vec<i32>` | **48**, align 8 | **24**, align 8 | **no** |
| `Option<&T>` / `Option<T*>` | **16**, align 8 | **8**, align 8 | **no** — Rust's null-pointer niche has no C++ expression |
| `Arc<T>` | 8 | 8 | yes |
| `Box<T>` | 8 | 8 | yes |
| `Cell<i32>` | 4 | 4 | yes |
| `Option<i32>` | 8 | 8 | yes |

The divergence is specific rather than universal — several small types do
line up — but `Vec` is twice the size and `Option<&T>` is twice the size, and
both appear all through the RPC data structures. This is why the Raft
carriers in `rusty-rustc/src/lib.rs` have hand-pinned sizes with matching
`static_assert`s in `server.h`: where the two worlds must agree, they are
made to agree by hand, one type at a time.

## F. Reproducing the whole thing

```bash
A=build/src/rrr/librrr.a
ls -la $A && file $A && ar t $A | wc -l

ninja -C build -t query src/rrr/goal0-crate-cpp/rrr.client.cppm
ninja -C build -t commands src/rrr/goal0-crate-cpp/rrr.client.cppm | tail -1
ninja -C build -t query src/rrr/CMakeFiles/rrr.dir/goal0-crate-cpp/rrr.client.cppm.o
ninja -C build -t commands src/rrr/CMakeFiles/rrr.dir/goal0-crate-cpp/rrr.client.cppm.o | tail -1
ninja -C build -t query src/rrr/librrr.a

nm --defined-only $A | grep -c '_ZN3rrr'                       # 1457
nm --defined-only $A | grep -cE '_ZN3rrr[A-Za-z0-9_]*D[012]Ev' # 236
nm --defined-only $A | grep -cE ' [VD] _ZTV'                   # 161
nm $A | grep -c '__gxx_personality_v0'                         # 44
nm $A | grep -cE ' _R[A-Za-z0-9_]{8,}'                         # 0
nm $A | grep -cE 'rust_begin_unwind|core::panicking|_ZN4core'  # 0
```

For the layout probe, take the module map from any built rrr TU
(`grep '^-fmodule-file=' build/src/rrr/CMakeFiles/rrr.dir/goal0-crate-cpp/rrr.client.cppm.o.modmap`)
and compile with `-march=native` to match the BMI's target features.
