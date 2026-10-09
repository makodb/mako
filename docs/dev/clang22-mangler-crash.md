# LLVM Itanium-mangler frontend crash on C++20-module container lambdas (clang 21 *and* 22)

<!-- Filename is historical ("clang22-…"); the crash affects clang 21 too — see the CORRECTION below. -->
<!-- Original title: "clang 22 Itanium-mangler frontend crash (rusty::iter dispatcher lambda)" -->

Trigger lambda: the `rusty::iter` dispatcher at `slice.hpp:1821`.

**Status:** worked around in-tree (see "Workaround" below). Not yet filed
upstream — this doc is the writeup to review and, if we agree it's a clean
LLVM bug, submit to the LLVM project.

**Compiler:** Homebrew `clang++` **22.1.7** (libc++, `-std=gnu++23`,
C++20 modules).

> **CORRECTION (verified 2026-07-22): this is NOT a clang-22 regression.
> clang 21.1.8 ALSO crashes** with the identical mangler SIGSEGV at
> `slice.hpp:1821` when it compiles the **module form** of the rusty-cpp
> containers. Building `src/cluster` on clang 21 (gate bypassed) segfaults
> on `shard.h` and `sharding_policy.h` — the same `rusty::iter` dispatcher
> lambda, reached through `btree_internal.cppm` / `vec_port.raw_vec.cppm`.
> The earlier "clang 21 does not crash" claim was an **invalid inference**
> from mako-dev being green: mako-dev pins an older rusty-cpp where these
> containers are **headers** (`#include <btree_port/btreemap.hpp>`), so the
> lambda is textually included and never crosses a module boundary — the
> bug's precondition doesn't exist. No clang-21 build had ever compiled the
> module form. It is a **shared LLVM 21+22 modules/mangler defect**, exposed
> by rusty-cpp's header→C++20-module container migration, not a version
> regression. See "Why both clang 21 and 22 crash" below.

**Trigger file:** `src/cluster/shard_manager.h` (a C++23 named-module
partition `cluster:shard_manager`) at the state *before* commit
`<workaround-commit>` — i.e. with `ShardManager::final_sync` iterating the
migration delta maps directly:

```rust
for kv in (*self).mig_staged  { (*self).shards.get_mut(dst_id).unwrap().put(kv.0, kv.1); }
for dk in (*self).mig_deleted { (*self).shards.get_mut(dst_id).unwrap().remove(dk.0); }
```

`for kv in <BTreeMap>` lowers to `rusty::for_in(<BTreeMap>)` (rusty-cpp,
`third-party/rusty-cpp/include/rusty/slice.hpp`).

---

## Symptom

`clang++` **segfaults (exit 139) in its own frontend** — a compiler bug,
not a diagnosable error in our source. It happens during **LLVM-IR
generation of the module partition** (`.pcm`/`.o`), independent of `-O`
level (`-O0`, `-O1`, `-O2` all crash).

```
clang++: error: clang frontend command failed with exit code 139
```

## Crash backtrace (the important part)

Frontend context (what clang was doing):

```
2. Per-file LLVM IR generation
3. rusty/slice.hpp:2015:16: Generating code for declaration 'rusty::for_in'
4. rusty/slice.hpp:2042:7:  LLVM IR generation of compound statement ('{}')
5. rusty/slice.hpp:1821:13: LLVM IR generation of declaration '(lambda)::operator()'
6. rusty/slice.hpp:1821:13: Mangling declaration '(lambda)::operator()'   <-- dies here
```

Stack (top frames; `ItaniumMangle.cpp`):

```
#4  CXXNameMangler::mangleSourceName(clang::IdentifierInfo const*)     <-- SIGSEGV
#5  CXXNameMangler::mangleUnqualifiedName(...)
#6  CXXNameMangler::manglePrefix(clang::DeclContext const*, bool)      <-- recurses
#7  CXXNameMangler::manglePrefix(clang::DeclContext const*, bool)
#8  CXXNameMangler::mangleTemplatePrefix(clang::GlobalDecl, bool)
#9  CXXNameMangler::mangleNestedName(...)
#10 CXXNameMangler::mangleFunctionEncoding(clang::GlobalDecl)
#11 ItaniumMangleContextImpl::mangleCXXName(clang::GlobalDecl, raw_ostream&)
#12 getMangledNameImpl(CodeGenModule&, GlobalDecl, NamedDecl const*, bool)
#13 CodeGen::CodeGenModule::getMangledName(GlobalDecl)
#14 CodeGen::CodeGenModule::EmitGlobal(GlobalDecl)                     <-- emitting a global
#15 CodeGen::CodeGenModule::EmitTopLevelDecl(Decl*)
#16 CodeGeneratorImpl::HandleTopLevelDecl(DeclGroupRef)   (ModuleBuilder)
#17 BackendConsumer::HandleInterestingDecl(DeclGroupRef)
#18 MultiplexConsumer::HandleInterestingDecl(DeclGroupRef)
#19 ASTReader::PassInterestingDeclsToConsumer()                       <-- module reader
#20 ASTReader::GetExternalDeclStmt(unsigned long)                     <-- lazy BMI load
#21 FunctionDecl::getBody(FunctionDecl const*&) const
#22 ExprEvaluatorBase<RecordExprEvaluator>::handleCallExpr(...)       (ExprConstant.cpp)
...  (constant-expression evaluation frames) ...
#32 clang::Expr::isEvaluatable(ASTContext const&, ...)
#33 CodeGen::CodeGenFunction::EmitReturnStmt(clang::ReturnStmt const&) <-- probe origin
```

The symbol it dies mangling is the **generic lambda** at `slice.hpp:1821`,
the universal-iterator dispatcher inside `rusty::iter`:

```cpp
[](auto&& __r) -> decltype(__r.iter()) { return __r.iter(); }
```

## Root cause (mechanism)

Reading the stack bottom-up, three features stack up to reach a buggy
codegen path:

1. **Codegen of a `return <call>`** (`#33 EmitReturnStmt`) inside
   `rusty::for_in` / `rusty::iter`.
2. clang probes whether that return expression is **constant-foldable**
   (`#32 isEvaluatable` → `ExprConstant`). To evaluate the call it needs
   the callee's *body*.
3. The callee body lives in an **imported module's BMI**, so
   `ASTReader::GetExternalDeclStmt` deserializes it (`#20`). In module
   builds, deserializing an "interesting" decl makes the ASTReader hand it
   to the **codegen** consumer (`#17–#19`), which tries to **emit** it now
   (`EmitGlobal` → `getMangledName`, `#13–#14`) — and the decl is the
   generic lambda's `operator()`.
4. To emit it, clang must **mangle** its name. The closure type is a
   *local class of a function template* (`iter<Range>`); mangling walks the
   enclosing prefix (`namespace rusty` → `iter` → the closure), `manglePrefix`
   recurses (`#6–#7`), and `mangleSourceName` is handed a bad/null
   `IdentifierInfo*` → **segfault**.

So the fatal combination is: **C++20 module boundary (lazy BMI decl
deserialization) + a generic lambda whose closure is local to a function
template + that lambda being emitted as a side effect of constexpr-fold
analysis during codegen.** clang 22's mangler mishandles this shape;
clang 21 does not.

## Why both clang 21 and 22 crash (NOT a version regression)

A well-formed program cannot make the mangler segfault, so this is a
compiler defect — but it is **not** clang-22-specific. It is a defect in
the LLVM Itanium mangler (both 21 and 22) for the
"local-class-of-a-function-template, materialized across a module boundary"
case: `CXXNameMangler::mangleSourceName` dereferences a bad pointer.

Empirically (verified 2026-07-22, gate-bypassed `src/cluster` build):

| Compiler | `shard.h` / `sharding_policy.h` | `cluster_config.cc` |
|----------|--------------------------------|----------------------|
| clang 22.1.8 | ✅ compiles | ❌ lambda-ODR error (a *different* modules bug) |
| clang 21.1.8 | ❌ **mangler SIGSEGV** at `slice.hpp:1821` | not reached (crashes first) |

So the two compilers fall over on **different translation units**, but both
fall over on the same construct family. Neither is "immune."

The reason mako-dev (clang 21) is green is **not** that clang 21 handles
this correctly — it is that mako-dev pins an older rusty-cpp where the
containers are **headers**, so the offending lambda is textually included
into each TU and never crosses a C++20-module (BMI) boundary. The crash's
precondition therefore never arises there. rusty-cpp `c529cd3d` migrated
`btree_port`/`vec_port` to C++20 modules (module form introduced at
`7311d187`), which is what exposes the latent mangler bug — on *both*
LLVM 21 and 22.

Corollary: "fall back to clang 21" does **not** avoid this. clang 21 is in
fact worse for the module form — it crashes inside the rusty container
modules / cluster interface partitions (no in-tree workaround, it's library
code), whereas clang 22 gets far enough that the remaining failures are in
first-party cluster code we can refactor. clang 21 additionally frontend-
crashes on `srpc.reactor`'s module codegen (the reason `CMakeLists.txt`
gates to clang >= 22 in the first place). (Related, independently observed
mangler crashes with rusty-cpp are noted in the project memory as the
"clang22 hashbrown mangler crash".)

## How to reproduce

From the CMake build dir (`build_clang22`), with the module BMIs already
built:

```
ninja -t commands CMakeFiles/cluster.dir/src/cluster/shard_manager.h.o | tail -1
# run that exact command (clang 22.1.7) -> SIGSEGV in the mangler
```

The full crashing command and stack dump are archived alongside this doc
(scratch: `repro/crash_dump.log`). The `@…​.modmap` supplies the imported
module BMIs (`rusty`, `btree_port.btree.map`, etc.).

## Minimization status (TODO)

A standalone minimal reproducer is **not yet extracted**. Findings so far,
which narrow it and should help a `creduce` pass:

- A tiny module that only does `rusty::for_in` over a
  `BTreeMap<string,string>` (and/or `<string,bool>`) — i.e. `final_sync`'s
  loops in isolation, using the *real* rusty headers — **does not crash**.
- Three hand-written standalone modules capturing the shape (module +
  generic lambda local to a function template + a `return <call>` probed
  for constexpr-foldability) **did not crash** either.

⇒ The trigger depends on shard_manager's **accumulated instantiation
state** (it imports `cluster:shard` / `:config_manager` / `:cluster_config`
and instantiates the `Shard`/`ShardInfo` graph plus the migration
protocol), not a single construct. Reduction will likely need `creduce`
on the preprocessed module inputs rather than hand-minimization.

## Workaround (applied in-tree)

`shard_manager` was the only `for_in` user in its partition. We moved the
delta-map iteration onto `Shard` (in the smaller `cluster:shard`
partition), which already iterates a `BTreeMap` via `for_in` and compiles
cleanly:

- Added `Shard::apply_migration_delta(staged, deleted)` in
  `src/cluster/shard.h` (does the two `for_in` loops).
- `ShardManager::final_sync` now calls it instead of iterating inline.

With no `for_in` left in the `shard_manager` partition, there is no
`rusty::iter` lambda to emit+mangle there, and the partition compiles.
The crash is dodged, not fixed — the same construct in a
sufficiently-large partition would crash again on clang 22.

If we later re-align the merged branch to clang 21 (the other pinned
toolchain), this workaround is unnecessary but harmless.
