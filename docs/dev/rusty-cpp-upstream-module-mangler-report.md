# Transpiled C++20-module containers trigger an LLVM Itanium-mangler frontend crash (clang 21 & 22)

**Repo:** shuaimu/rusty-cpp
**Component:** transpiled module-form containers (`btree_port`, `vec_port`) + the `rusty::iter` / `deref_call` dispatcher in `include/rusty/slice.hpp`
**Severity:** blocks any consumer that `import`s a transpiled container module and instantiates it from a C++20-module implementation unit.

## Summary

rusty-cpp's transpiled **module-form** containers emit a **generic lambda whose
closure type is local to a function template** (the `rusty::iter` / `deref_call`
dispatcher). When that lambda is instantiated **across a C++20 module (BMI)
boundary** — i.e. a consumer does `import btree_port.btree.map;` and instantiates
e.g. `BTreeMap<K,V>::insert` — clang's Itanium name mangler dereferences a bad
pointer and the **frontend segfaults**. On some translation units clang 22 instead
emits a spurious ODR error (`lambda ... operator() ... is not present in definition`).

This reproduces on **both clang 21.1.8 and clang 22.1.8** (libc++, `-std=gnu++23`,
`import std`). The **header form** of the same containers does **not** trigger it,
because the lambda is textually included and never crosses a module boundary.

A valid program cannot make the mangler segfault, so the *root cause* is an LLVM
defect. But rusty-cpp controls the shape of the emitted dispatcher, and the crash
is triggered by that specific shape — so the practical fix is a codegen change here
(see **Suggested mitigation**). LLVM is unlikely to be fixed on a useful timeline.

## Environment

- Homebrew `clang++` **21.1.8** and **22.1.8**, libc++, `-std=gnu++23`, C++20 named
  modules, `import std`.
- rusty-cpp at commit `c529cd3d` (btree_port/vec_port are C++20 modules; the module
  form was introduced at `7311d187`, "btree_port: complete BTreeMap+BTreeSet
  transpiled-module migration").
- Consumer: a C++23 named module (`export module cluster;`) whose implementation
  unit instantiates `BTreeMap<uint32_t, T>::insert` / `BTreeMap<std::string, T>` and
  iterates a `BTreeMap` via `rusty::for_in`.

## The two trigger sites (in rusty-cpp's own output)

1. `include/rusty/slice.hpp:1821` — the `rusty::iter` universal-iterator dispatcher,
   used by `rusty::for_in`:
   ```cpp
   [](auto&& __r) -> decltype(__r.iter()) { return __r.iter(); }
   ```
2. `transpiled/btree_port/btree_port.btree.btree_internal.cppm:5488` — the
   `Handle<Node, Type>::insert_fit` dispatcher passed to `deref_call`:
   ```cpp
   [&](auto&& __recv)
       -> decltype(std::forward<decltype(__recv)>(__recv).key_area_mut(rusty::range_to(new_len)))
       { return std::forward<decltype(__recv)>(__recv).key_area_mut(rusty::range_to(new_len)); }
   ```

Both share the same shape, which is what the compiler mishandles:
- a **generic lambda** (`auto&&` parameter ⇒ its `operator()` is itself a template);
- a **`decltype`-SFINAE trailing return type** (site 2 additionally depends on a
  captured local, `new_len`);
- whose **closure type is local to a function template** (`iter<Range>` /
  `Handle<Node,Type>::insert_fit`);
- **defined in a module interface** and instantiated **from a consumer across the
  module boundary**.

## What happens

| Compiler | Symptom | Where |
|----------|---------|-------|
| clang 21.1.8 | **frontend SIGSEGV** in `CXXNameMangler::mangleSourceName` while *"Mangling declaration '(lambda)::operator()'"* | any module unit that instantiates a module-form container (observed on our `shard.h`, `sharding_policy.h`), through `slice.hpp:1821` |
| clang 22.1.8 | same **SIGSEGV** on some TUs; on others a spurious **ODR error**: `'...insert_fit(...)::(lambda)::operator()' from module 'btree_internal' is not present in definition of '(lambda at btree_internal.cppm:5488)' in module 'btree_internal'` | through `btree_internal.cppm:5488` |

clang-21 crash tail:
```
third-party/rusty-cpp/transpiled/btree_port/btree_port.btree.btree_internal.cppm:2740:46:
    note: in instantiation of template class 'rusty::Result<std::string, rusty::String>' requested here
include/rusty/slice.hpp:1821:13: Mangling declaration '(anonymous class)::operator()'
clang++: error: unable to execute command: Segmentation fault
clang++: error: clang frontend command failed due to signal (use -v to see invocation)
```

clang-22 mangler stack (top frames, `ItaniumMangle.cpp`):
```
CXXNameMangler::mangleSourceName(clang::IdentifierInfo const*)   <-- SIGSEGV
CXXNameMangler::mangleUnqualifiedName(...)
CXXNameMangler::manglePrefix(clang::DeclContext const*, bool)    <-- recurses
CXXNameMangler::mangleTemplatePrefix(clang::GlobalDecl, bool)
CXXNameMangler::mangleNestedName(...)
...
CodeGen::CodeGenModule::EmitGlobal(GlobalDecl)                   <-- emitting a global
ASTReader::PassInterestingDeclsToConsumer()                     <-- from a module (BMI) load
```

## Root cause (mechanism)

When a consumer instantiates the container operation, clang must instantiate the
generic lambda's `operator()`. Because the definition lives in an imported module's
BMI, the `ASTReader` lazily deserializes it and (in a module build) immediately hands
it to codegen, which tries to **mangle** its name. The closure is a *local class of a
function template*; mangling walks the enclosing prefix (`namespace` → function
template → closure), `manglePrefix` recurses, and `mangleSourceName` is handed a
bad/null `IdentifierInfo*` → segfault. clang 22 sometimes instead fails to reconcile
the lambda's `operator()` definition against the BMI copy → the ODR error.

So the fatal combination is: **generic lambda whose closure is local to a function
template + materialized across a C++20 module/BMI boundary.** LLVM 21 and 22 both have
the defect; they merely fall over on different translation units.

**Why it was not seen before the module migration:** in the header form of these
containers the lambda is textually included into each TU, so it never crosses a module
boundary and the precondition never arises. The regression is header → C++20-module,
not a compiler-version change.

## Suggested mitigation (rusty-cpp codegen side)

Since LLVM won't be fixed soon, the actionable fix is to stop emitting the
crash-triggering shape:

1. **Preferred — hoist the dispatcher out of the function template.** Emit the
   `iter` / `deref_call` dispatcher as a **named functor or free function template at
   namespace scope** instead of a generic lambda *local to* `iter<Range>` /
   `insert_fit`. The mangler handles namespace-scope templates fine; the crash is
   specific to the *local-class-of-a-function-template* case.
2. **Or make the dispatcher non-generic** where the receiver type is statically known
   (drop `auto&&`), so there is no templated closure `operator()` to instantiate
   across the boundary.
3. **Or offer a header fallback** for the hot container types (as pre-`7311d187`), so
   consumers can avoid the module boundary for `BTreeMap`/`Vec`.

Option 1 is the most robust and localized — it removes the exact construct the mangler
chokes on while keeping the module form.

## Reproduction (using rusty-cpp's own tree)

1. Build the transpiled containers as C++20 modules (`rusty` core, `btree_port`,
   `vec_port`) with clang 21.1.8 or 22.1.8, libc++, `-std=gnu++23`, `import std`.
2. From a module **implementation unit**:
   ```cpp
   module some_mod;                 // implementation unit of `export module some_mod;`
   import btree_port.btree.map;
   void f() {
       auto m = btree_port::btree::map::BTreeMap<std::string, std::string>::new_();
       m.insert(std::string("a"), std::string("b"));   // instantiates insert_fit + the lambda
   }
   ```
   (and/or `rusty::for_in` over a `BTreeMap`, which routes through `slice.hpp:1821`.)
3. clang 21 → SIGSEGV mangling the `slice.hpp:1821` lambda; clang 22 → SIGSEGV or the
   `btree_internal.cppm:5488` ODR error.
4. Control: the identical instantiation via the **header** form does **not** crash.

## Minimization status

A fully-minimal *standalone* reproducer (hand-written module + a generic lambda local
to a function template + a consumer implementation unit) did **not** reproduce in ~5
attempts — the trigger depends on the accumulated instantiation depth of the real
container (`Result<>` / `RawVec` / `Handle` over a non-trivial type such as
`std::string`), not the isolated shape. A reduced repro will likely need `creduce` on
preprocessed module inputs. The reliable reproducer is the real transpiled output as
above, which is available directly in this repo.
