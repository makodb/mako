# libclang 22 parse-time crash on first-time user-type template instantiation in module purview

## Status

- **Open** upstream LLVM bug — not yet reported (collected here for filing).
- **Workaround landed**: rusty-cpp's parser detects the libclang `Crash`
  error and retries the parse with module args dropped (keeps `import std;`
  only). The borrow check survives but loses cross-module callee resolution
  on the affected TUs.
- Affected files in rrr: `src/rrr/rpc/server.cpp`, `src/rrr/rpc/fiber_channel.cpp`.

## Summary

clang 22.1.5's libclang frontend crashes (returns the opaque error
`"Crash"` to `clang_parseTranslationUnit2`) when parsing certain
C++23 module purview translation units. The crash fires the moment
the parser closes the body of a function that performs a **first-time
template instantiation of a user-defined type** in module purview —
e.g. `rusty::make_box<ChannelFactoryProxy>(...)`.

`clang++ -fsyntax-only` with the exact same command-line flags
**compiles the file successfully**, so the bug is specific to
libclang's parse path, not clang's frontend in general.

The same source built with clang 19's libclang against
clang-19-generated PCMs parses cleanly with no recovery needed.

## Reproducer location

`src/rrr/rpc/server.cpp` lines 695-700:

```cpp
void set_channel_factory(ChannelFactoryProxy factory) {
    if (!factory) return;
    channel_factory_ = rusty::Some(
        rusty::make_box<ChannelFactoryProxy>(std::move(factory)));
}
```

Bisection (clean truncations of `server.cpp` evaluated with our
borrow-check pipeline, libclang 22.1.5, all rrr module-graph PCMs
loaded):

| File length | Result |
|---:|---|
| 500 lines (no `set_channel_factory`) | clean parse |
| 600, 650, 675, 690, 695, 697, 698, 699 lines | clean parse |
| **700 lines** (closes `set_channel_factory` body) | **first-attempt `"Crash"`** |
| 705 lines | first-attempt `"Crash"` |
| 710 lines (closes the following methods) | clean parse again |

The crash is **parse-state-sensitive**: extending the file past the
trigger function with more method definitions can mask the crash.
This rules out "always crashes once X is parsed" and suggests an
intermediate parser state that recovers if the parse continues far
enough.

## Source-level characterization

- `ChannelFactoryProxy` is a typedef visible only in the current
  module's purview (defined in `src/rrr/rpc/channel.cpp`, the
  `rrr.channel` module).
- `rusty::make_box<T>` is `template<typename T, typename... Args>
  Box<T> make_box(Args&&...)`, defined in `third-party/rusty-cpp/include/rusty/box.hpp`.
- Calling `rusty::make_box<ChannelFactoryProxy>(...)` from inside
  `Server::set_channel_factory` triggers the **first** instantiation
  of `rusty::make_box<ChannelFactoryProxy>` in the rrr.server module
  purview.
- The GMF of `server.cpp` already does
  `#include <rusty/rusty.hpp>` — the documented workaround for the
  related clang-22 codegen crash on libc++ container templates
  (see [`srpc_module_migration_plan.md`](srpc_module_migration_plan.md)).
  That workaround pre-instantiates std::vector/std::deque/... but
  doesn't reach user-defined types like
  `rusty::Box<ChannelFactoryProxy>`.

## clang-19 vs clang-22 comparison (definitive)

Built `rrr` with clang 19 in `build_clang19/`, producing matching
clang-19 PCMs for every module except `rrr.server` (which clang-19
itself can't compile due to a separate, documented multi-attachment
bug on `rusty::HashMap::operator()`).

Pointed a libclang-19-linked rusty-cpp at `build_clang19/compile_commands.json`:

| Setup | server.cpp | fiber_channel.cpp |
|---|---|---|
| libclang 22 + clang-22 PCMs (production) | First-attempt **Crash**; recovers via std-only retry; degraded analysis | First-attempt **Crash**; recovers via std-only retry; degraded analysis |
| libclang 19 + clang-19 PCMs (matched, this test) | **Clean parse**, 6 full-info findings | **Clean parse**, 3 full-info findings |
| libclang 19 + clang-22 PCMs (mismatched) | PCM version-mismatch error; recovers cleanly | Same |

Combined with the fact that `clang++ -fsyntax-only` compiles the
source without issue, this is conclusively a **libclang-22
regression** introduced between clang 19 and clang 22.

## Why we don't downgrade

clang 19 has its own showstopper: `rusty::HashMap<K, V>::operator()`
gets multi-attached across module boundaries (instantiations from
`rrr.reactor` clash with the same instantiation in `rrr.server`),
which clang 22 explicitly fixed. We hit this in this very
investigation:

```
rusty-cpp/include/rusty/hashmap.hpp:77:12: error: declaration
  'operator()' attached to named module 'rrr.reactor' can't be
  attached to other modules
```

Reverting to clang 19 trades a recoverable libclang parse crash for
an unrecoverable production-build failure. Net negative.

## Possible upstream classification

- Likely a regression in clang 22's **template-instantiation
  serialization** within libclang's incomplete-TU parsing mode (the
  driver pipeline does not exhibit it because it follows a different
  codegen path).
- Related bug class to the documented clang-22 codegen crash
  ("instantiating libc++ container templates ... for the first time
  crashes EmitScalarExpr inside EmitReturnStmt/EmitIfStmt") — but
  that one is in codegen (`-c`) and only affects libc++ types, while
  this one is in libclang parse and affects user-defined types too.

## Minimal-shape repro outline (for upstream filing)

A reduced reproducer would look approximately like:

```cpp
// File: repro.cpp
module;
#include <rusty/rusty.hpp>     // workaround pre-instantiation for libc++
export module repro;
import std;
import other_module;            // brings UserType into scope

export namespace repro {

class UserHolder {
public:
    rusty::Option<rusty::Box<UserType>> slot_;

    void set(UserType v) {
        // First instantiation of rusty::make_box<UserType> in this
        // module's purview. libclang-22 crashes when parsing the
        // closing `}` of this function.
        slot_ = rusty::Some(rusty::make_box<UserType>(std::move(v)));
    }
};

}
```

Building the dependency PCMs and running:

```
clang++ -fsyntax-only -std=gnu++23 -stdlib=libc++ -x c++-module \
  -fmodule-file=std=std.pcm -fmodule-file=other_module=other.pcm \
  -fparse-all-comments repro.cpp
```

succeeds. Invoking libclang-22's parse API (`clang_parseTranslationUnit2`)
with the same arguments returns `CXError_Crashed`.

We have not yet validated this exact minimal shape against a stock
clang 22 install; the rrr-internal reproducer is concrete and
exercises the path.

## Filing checklist (when ready)

- [ ] Build a self-contained minimum reproducer (no rusty-cpp dep —
      use raw `std::optional<std::unique_ptr<T>>` shape).
- [ ] Verify the reproducer fires on stock clang 22.1.5 and check
      whether it has been fixed on trunk.
- [ ] File at https://github.com/llvm/llvm-project/issues with the
      "clang:frontend" and "modules" labels.
- [ ] Cross-link the upstream issue here and in
      [`srpc_module_migration_plan.md`](srpc_module_migration_plan.md).

## Cross-references

- `third-party/rusty-cpp/src/parser/mod.rs` — Crash-recovery
  fallback in `parse_cpp_file_with_includes_defines_and_args`.
- `third-party/rusty-cpp/src/parser/header_cache.rs` — import-chasing
  for cross-module annotation discovery (related fix).
- [`srpc_module_migration_plan.md`](srpc_module_migration_plan.md)
  — broader migration narrative and the related codegen crash.
