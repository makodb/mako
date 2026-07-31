# S8a-0 — the seam probe

Answers the one question every later S8 decision rests on: **does a
C-linkage symbol survive clang's module purview, and does its absence
fail loudly?**

Result (2026-07-30, clang 22): **yes to both.**

```
DIRECTION 1 — with the shared .S:
  C++ SIDE OK: switched stacks and returned (mark=c0ffee)

DIRECTION 2 — without it:
  undefined reference to `srpc_fiber_swap'
  clang++: error: linker command failed with exit code 1
```

Both directions matter. A passing link proves the seam works; only the
*failing* link proves it is loud, and loudness is the entire point — it
is what converts the transpiler's silent `asm!` drop (which otherwise
yields wrong-code: the probe's `add_via_asm(2,3)` returned 2) into a
build error.

## One `.S`, no twin

The plan originally called for assembly in Rust `global_asm!` plus a
hand-maintained C++ twin, kept in sync by `offset_of!` static-asserts
that turned out not to lower. This probe uses a different shape:

```
src/fiber_x86_64.S          <- ONE source of truth
  Rust: global_asm!(include_str!(...), options(att_syntax))
  C++:  the build assembles the same file
  both: extern "C" { fn srpc_fiber_swap(from: *mut Ctx, to: *mut Ctx); }
```

`global_asm!` dropping on the C++ side stops being a bug and becomes
correct — the build supplies the symbol there. There is no second copy
of the assembly, so there is nothing to drift.

## Two things this cost, worth remembering

**Rust inline asm defaults to INTEL syntax.** The shared file is AT&T
(what the C++ toolchain assembles), so `options(att_syntax)` is
required. Without it the errors point into `<inline asm>` at
coordinates that do not obviously correspond to the file.

**The seam takes `*mut T`, not `&mut T`** — and this is a transpiler
gap, not a style choice. Rust coerces `&mut T` to `*mut T` implicitly at
an FFI call; C++ has no `T&` -> `T*` conversion, so a `&mut` wrapper
emits `srpc_fiber_swap(from, to)` against `Ctx*` params and fails to
compile. Raw pointers at an `extern "C"` boundary are idiomatic anyway,
so this is a comfortable shape — but the missing coercion is real and
deserves its own fix.

## Reproduce

```sh
cd probes/s8seam && cargo test --test swap        # Rust side
rusty-cpp-transpiler --crate Cargo.toml --auto-namespace --output-dir out
# then compile out/s8seam.cppm as a MODULE, assemble src/fiber_x86_64.S,
# link both with driver.cpp — and again WITHOUT the asm object.
```

Module form is mandatory: this tree has been bitten by clang modules
repeatedly, and a non-module TU proves nothing about module purview.
