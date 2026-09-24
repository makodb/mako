# SRPC from C++

This companion covers the generated C++23 library: translation, builds, service
code generation, application code and C++ API spellings. The
[main SRPC book](srpc-book.md) teaches the native Rust library and its runtime.

C++ uses the same canonical Rust algorithms through generated modules, plus the
rusty-cpp C++ runtime and compatibility headers. The native Cargo library does not
need those inputs. C++ API exports and native Rust visibility can differ; use the
Rust book and `cargo doc` when writing Rust applications.

## Contents

1. [Translation and building](#1-translation-and-building)
2. [Service definition and code generation](#2-service-definition-and-code-generation)
3. [C++ service and client walkthrough](#3-c-service-and-client-walkthrough)
4. [C++ API reference](#4-c-api-reference)
5. [Ownership and translation checks](#5-ownership-and-translation-checks)
6. [Runtime APIs in C++](#6-runtime-apis-in-c)
7. [C++ client bindings](#7-c-client-bindings)
8. [C++ server bindings](#8-c-server-bindings)
9. [C++ serialization](#9-c-serialization)
10. [C++ reliability configuration](#10-c-reliability-configuration)
11. [Threading and synchronization in C++](#11-threading-and-synchronization-in-c)
12. [C++ performance and historical measurements](#12-c-performance-and-historical-measurements)
13. [C++ pitfalls and best practices](#13-c-pitfalls-and-best-practices)
14. [Troubleshooting generated C++](#14-troubleshooting-generated-c)

---

## 1. Translation and building

The pinned transpiler emits the C++ library from the canonical Rust crate.
This chapter explains the translation inputs and checks; the native Rust runtime
is described in the [main book](srpc-book.md).

### The translation

The pinned `rusty-cpp` transpiler reads the whole crate in one invocation and
emits one C++23 module interface unit per canonical file: `rpc/client.rs`
becomes `srpc.client.cppm`, exporting `namespace srpc` declarations whose
spellings are the historical C++ API. The mapping is source-to-source and
deliberately conservative:

- Types map through the `rusty` runtime: `Vec<T>` to `rusty::Vec<T>`, `Arc` /
  `Rc` / `Box` / `Option` / `Result` to their `rusty::` ports, closures to
  `rusty::Function` or lambdas at the call site.
- `async fn f(..) -> T` becomes a C++ coroutine returning `rusty::Task<T>`;
  `.await` becomes `co_await`, `return` becomes `co_return` ([C++ fibers](#fibers-and-coroutines-in-c)).
- `thread_local!` becomes `inline thread_local rusty::LocalKey<T>`, with the
  closure-only `.with()` accessor lowering through the ordinary method-call
  path ([reactor ownership](#reactor-ownership-in-c)).
- Rust names that collide with C++ keywords are mangled predictably:
  `Server::new` emits as `new_`, `this_fiber::r#yield` as `yield()`.

Some C++ contracts use inert `#[cfg_attr(any(), ...)]` attributes.
Rustc ignores them because `any()` is false; the emitter reads them as directives. Examples include `cpp_namespace(::janus)` for [generated events](#generated-event-apis),
`cpp_noexcept` and `cpp_abi`. The source gate audits the allowed families;
[CLAUDE.md](../CLAUDE.md) documents how to change that inventory. The mirror form
`#[cfg_attr(not(any()), derive(...))]` is the opposite tool: derives rustc
applies that the emitter must not see, which is how a C++ `operator==` can be
deliberately withheld while the Rust side keeps `PartialEq`.

Handwritten native code is limited to the shared nine-C manifest and architecture-selected
fiber context-switch assembly, plus the ABI/import headers described in [runtime ownership](srpc-book.md#runtime-ownership).
Canonical Rust owns runtime and protocol decisions. A neighboring header does not supply
a second implementation.

### What the gates hold

The normal CMake `ALL` build includes the source and dual-compile gates.

The **source gate** (`srpc_goal0_source_gate`) runs the DSL census, the
extraction check (`src/lib.rs` must match `rust-modules.toml`), contract negative
controls, the canonical Rust AST audit, native source and ABI-binding checks,
Cargo tests, and clippy with warnings denied. The Cargo independence check also
runs tests and doctests in a copied tree containing only Rust sources and the
C/assembly kernel, without the facade, vendored C++ runtime or transpiler.
A new clippy warning breaks the C++ build.

The **dual-compile gate** (`srpc_goal0_dual_compile`) is the ABI oracle. It
recompiles every generated module into its own object and links one importer
program twice. One executable uses the fresh objects placed ahead of `libsrpc.a`;
the other uses the archive alone. The gate runs both and compares per-module
`nm` strong-symbol sets against the reviewed inventory in
`scripts/check_srpc_crate_mode.py`.
Changes to that inventory require a measured, explained delta. Passing Cargo
tests does not establish C++ behavior. Verify changes in both lanes and update
the expected ABI only when the measured public contracts change.

Byte digests of the generated C++ are advisory only; symbol sets, import lists
and the zero-hand-slot requirement are mandatory. Unsupported-lowering markers
fail the gate, but their absence does not prove faithful translation. Compile
tests must instantiate the relevant templates, and runtime tests must check the
resulting behavior. The ABI/import checks and paired runtime fixtures provide
bounded evidence; they do not prove equivalence for every input or interleaving.

### Standard adapters, from the C++ side

The transpiler maps Rust std values, ownership types, synchronization, futures and
wakers to the C++ runtime. Explicit compiler mappings preserve the established C++
callback and container interfaces where their representation differs from Rust.
These mappings and C++ headers are inputs only to the generated C++ lane. Cargo uses
the standard Rust implementations directly. SRPC scheduling, events, archives and
protocol policy stay in canonical Rust.

The canonical AST audit rejects missing runtime bodies and checks empty or
constant-returning functions against a reviewed inventory. Native source and ABI
audits check the shared C/assembly kernel and its declarations. The Cargo independence
check rejects production Rust dependencies and builds a copy without the C++ inputs.
These checks complement behavioral tests; they do not establish equivalent behavior
for every input or execution schedule.

### Building the C++ library

Use Linux, Clang 22 or newer with libc++, CMake 3.30+, Ninja, a Rust toolchain with
clippy, Python 3.11+ and ripgrep. CMake builds the pinned transpiler from the
submodule and uses it to generate the production modules.

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build -L srpc --output-on-failure
```

GoogleTest is required when `BUILD_TESTING=ON`; configuration fails if it is missing.
Inspect the configured inventory with `ctest --test-dir build -N -L srpc`.
The `srpc` label excludes vendored tests whose executables are outside the default
build. Cargo tests and clippy are also part of the C++ source gate.

The library uses C++ named modules with `-std=gnu++23`, libc++ and `-march=native`.
Consumers must use compatible flags and the build's module map. See
[build wiring](#build-wiring) for downstream integration and its current limitations.

The C++ code generator in `pylib/` is a separate tool for application service
headers. It does not implement the SRPC runtime or run as part of Cargo.

### What the umbrella header gives you

`srpc.hpp` is the header nearly every consumer includes. It is a textual `#include`
chain followed by a hand-maintained list of `import srpc.*;` lines, in that order. libc++
rejects the reverse and fails with ODR errors inside its own internals, which is also why
`std_compat.hpp` exists to do the same for the `std` module. At the very bottom it
declares the one alias the library ships:

```cpp srpc-no-compile
namespace base = srpc;
```

Nothing generates or checks that import list, so it can drift, and eight modules are
deliberately commented out of it: `srpc.circuit_breaker`, `srpc.connection_metrics`,
`srpc.epoll_wrapper`, `srpc.heartbeat`, `srpc.internal_protocol`, `srpc.load_balancer`,
`srpc.reconnect_policy` and `srpc.request_options`. Each comment describes the omission as a build-time optimization for a module
unused by consumers. The repository does not measure that build-time cost.
Measure the effect when changing the import list.

To name a type from one of those modules, such as `ConnectionMetrics` or
`RequestOptions`, import its module explicitly after including `srpc.hpp`:

```cpp srpc-no-compile
#include "srpc.hpp"
import srpc.connection_metrics;
import srpc.request_options;
```

Finally, a name that used to be here and is gone: there is no `Marshal` class. Binary
serialization is `srpc::Serialize_::serialize` / `Deserialize_::deserialize` over
`BinaryWriteArchive` and `BinaryReadArchive`, which you build from proxies. [C++ serialization](#9-c-serialization)
covers it.

---


## 2. Service definition and code generation

The `.rpc` generator emits C++ and a Python stub. Its typed classes wrap the
dispatch-level `Service` trait that a [Rust service](srpc-book.md#9-rpc-server)
implements directly. There is no Rust service code generator yet.

You do not write the C++ lane's wire code by hand. You describe a service in a small
`.rpc` file, run a Python generator over it, and get back a single C++ header
containing a typed server base class and a typed client proxy. This chapter is about that file, that
generator, and exactly what comes out the other end.

The fullest worked example in the tree is `tests/benchmark_service.rpc`, its committed
output `tests/benchmark_service.h`, and the twelve out-of-line handlers in
`tests/benchmark_service.cc`. The optional `rpcbench` CMake target compiles the header
and handlers. Generation remains a separate, manual step. [The walkthrough](#3-c-service-and-client-walkthrough)
uses the smaller `Demo` service below for a complete server and client walkthrough.

### The service definition language

A `.rpc` file is an optional namespace declaration, then any number of struct and
service declarations in any order:

```
namespace demo

struct point3 {
    double x;
    double y;
    double z;
};

abstract service Demo {
    sayhi(string hi);
    sum(i32 a, i32 b, i32 c | i32 result);
    fast dot_prod(point3 p1, point3 p2 | double v);
    defer slow_echo(string msg | string echoed);
};
```

The namespace may be qualified, as in `namespace demo::inner`. It wraps the
generated structs and service classes. The `%%` sections described below sit
outside it. Structs become plain C++ structs with serialize/deserialize functions
attached; you can nest them and use them as parameter types anywhere.

Inside a service, each line is one method: an optional dispatch attribute, the method
name, and a parenthesized signature whose input and output parameter lists are separated
by `|`. The `|` is optional, so a method may have inputs only (`sayhi`), outputs only
(`nop( | i32 status)`), or neither. Parameter *names* are optional too. `fast nop(string)`
is legal, and the generator invents a name for you (see below).

Four rules about types are worth knowing before you write anything:

**Integers must carry an explicit size.** The type keywords are `i8`, `i16`, `i32`,
`i64`, and the varint-encoded `v32` and `v64`; they become `srpc::i8` … `srpc::v64` in
the generated header. Writing `bool`, `int`, `unsigned` or `long` is a hard parse error
whose message tells you to use a sized type instead. The wire format requires a fixed integer width. Note that `v32`/`v64` come out as the
`srpc::v32` / `srpc::v64` wrapper structs rather than arithmetic types, so you read and
write them with `.get()` and `.set()`.

**Eight names get a `std::` prefix for free.** `pair`, `string`, `map`, `list`, `set`,
`vector`, `unordered_map` and `unordered_set` are rewritten to `std::pair`,
`std::string`, and so on. Templates nest, so `map<i32, vector<string>>` becomes
`std::map<srpc::i32, std::vector<std::string>>`.

**Every other name passes through untouched.** That is how `point3` above works, and how
you reach your own types, including qualified names like `::mylib::Blob`. Floating-point
types are in this bucket: `double` and `float` are not keywords, they are just symbols
the generator copies verbatim. Only `double` has serialization support. There is no
`float` encoder or decoder in `misc/serializable.rs`, so a `float` field parses fine and
then emits a header with no overload for the generated `serialize` call to bind to. Use
`double`. Only an identifier that is *exactly* a reserved word collides; the scanner
takes the longest match, so `integer` and `asyncfoo` parse as ordinary symbols even
though `int` and `async` are keywords.

**Method and parameter names in `__NAME__` form are rejected.** The generator reserves
that shape for the glue it emits (`__reg_to__`, `__dispatch__`, the per-method wrappers),
and raises rather than let you collide with it.

Two lexical quirks round it out. Semicolons are ignored entirely, so use them or don't.
Comments run from `//` to end of line, but they must not be empty: a line containing
exactly `//` matches no token and is a syntax error. Expect no polish from the error
reporting: yapps prints a `line:col: Trying to find one of ...`
diagnostic and then dies inside its own error printer with an unrelated `TypeError`.
Read the first line and ignore the traceback.

### Splicing raw C++ into the header

If the generated header needs an `#include`, a `using namespace`, or a forward
declaration, you supply it with `%%` section markers. The rule is easy to get wrong, so
state it precisely: the generator counts lines that consist of *exactly* `%%`.

With two such lines, the generator copies everything above the first marker
near the top of the generated header, parses the middle region as IDL, and
copies everything below the second marker to the bottom of the header. That is the layout `tests/benchmark_service.rpc` uses.

With **one** marker there is no header section: the text above it is parsed as IDL and
the text below it becomes the footer. With none, the whole file is IDL.

Both spliced regions land outside the generated namespace. The header text goes
above the opening brace and the footer below the closing one, as `tests/benchmark_service.h`
shows. So code you splice in is at global scope (or wherever your own `namespace` block
puts it), not in `demo`. That matters most for hand-written `serialize`/`deserialize`
pairs: a pair at global scope for a type declared inside `namespace demo` is invisible
to the ADL dispatch of [C++ serialization](#9-c-serialization). Either declare the type in the header section too, so
both sit at global scope, or wrap the pair in your own `namespace demo { ... }`.

```
// this lands at the top of the generated header
#include <math.h>
%%

namespace demo
service Demo { ... };

%%
// this lands at the bottom of the generated header
```

### Choosing how each method is dispatched

The keyword in front of a method name decides how the server runs your handler and,
consequently, what signature the generated virtual has.

| Attribute | Generated handler signature | How it runs |
| --- | --- | --- |
| *(none)* | `rusty::Result<Resp, srpc::i32> m(const Req&) const` | in a fiber the server spawns per request; may suspend through SRPC fiber APIs |
| `fast` / `prefix` | same | inline on the poll thread, no fiber |
| `defer` | `void m(const Req&, Resp& resp, srpc::DeferredReply defer) const` | in a fiber; you reply whenever you like |
| `fiber` | `rusty::Result<Resp, srpc::i32> m(const Req&) const` | in a fiber inside the request fiber (see the caveat below) |
| `async` | `rusty::Task<rusty::Result<Resp, srpc::i32>> m(const Req&) const` | entered inline on the poll thread, then resumed as a stackless coroutine |
| `raw` | `void m(rusty::Box<srpc::Request>, srpc::WeakServerConnection) const` | in a fiber; you decode and reply yourself |

The mechanism behind the table has two halves. The generated `__reg_to__` registers each
method id with the server through either `reg_fast_rpc` (for `fast`, `prefix` and
`async`) or `reg_rpc` (for everything else). Then the server's connection dispatch, in
`rpc/server.rs`, checks whether the incoming id is in the fast set: if it is, the handler
is invoked inline on the poll thread; if it is not, the server spawns a fiber and runs
the dispatch there so the handler is free to yield.

`fast` and `prefix` are the same attribute. The grammar maps the `fast`
keyword onto `prefix`. A `fast` handler is the cheapest option, but there is no fiber
under it to yield from, so it must not block: if it does, every connection on that poll
thread stalls. Every poll of an `async` handler also runs on that worker and must
return promptly. Default fiber dispatch permits cooperative SRPC waits and sleeps;
ordinary blocking I/O, thread sleep and RPC future waits still block the OS worker.

A `defer` handler receives a `srpc::DeferredReply` by value and a reference to a
response struct. Fill in the response and call `defer.reply()` or
`defer.reply_error(code)` when the answer is ready. Both methods fire at most
once. Dropping the handle without replying leaves the caller waiting for a reply
or timeout. Keep the handle on its supported execution thread.

The generated wrapper creates the typed request as a local variable. A deferred
callback must copy any request fields it needs and retain or move the reply
handle. Capturing `const Req&` after the handler returns leaves a dangling
reference. The reply handle's writer captures shared ownership of the response
struct, so that response remains alive while the writer is retained. Finish
writing it before replying, and do not access it after the reply consumes the
writer unless another owner keeps it alive.

An `async` handler is a C++20 coroutine returning `rusty::Task<...>`; finish it with
`co_return`. The generated wrapper calls it, then hands the task to
`srpc::reactor_spawn_stackless_task_with_result` on the current reactor with a completion
callback that upgrades the weak connection handle and sends the reply.

The async wrapper also creates its typed request as a local. The reactor polls
the task immediately, but the wrapper returns after the first suspension. Copy
needed input fields into coroutine-owned values before that suspension; the
`const Req&` argument does not remain valid for later resumes. Retaining the raw
RPC request for reply delivery does not retain this separate typed local. The
wrappers in [benchmark_service.h](../tests/benchmark_service.h) show both lifetime
boundaries.

The `fiber` attribute adds a second fiber. It registers on the ordinary path,
so the server has already created a request fiber when the generated wrapper
spawns the inner one. Worse, that wrapper names `Fiber::create_run` unqualified, so the generated
header only compiles if you have spliced a `using namespace srpc;` into your `%%` header
section. The plain default already gives you a fiber; prefer it.

`abstract` is a *service-level* keyword: `abstract service Demo { ... }` makes every
generated handler pure virtual, which is what you want whenever you plan to subclass the
generated class, because then the compiler will not let you forget a method. If you want
just one method pure, leave the service concrete and put a trailing `= 0` on that method
instead:

```
service Demo {
    sum(i32 a, i32 b | i32 result) = 0;   // pure
    sayhi(string hi);                     // declared, you define it out of line
};
```

Be aware of the trap on the other side of that choice: for a non-`abstract` service, the
generated virtuals are declared but never defined, so the class has no key function and
its vtable is never emitted. Subclassing it fails at link time even when you override
everything. Define the virtuals out of line in a `.cc`, as
`tests/benchmark_service.cc` does for the non-abstract `BenchmarkService`.

### Running the generator

There is no `bin/rpcgen` in this repo. Import the generator with `pylib/` on the Python
path, as the configured generator tests do:

```sh
PYTHONPATH=/path/to/srpc/pylib python3 -c \
  "from simplerpcgen.rpcgen import rpcgen; rpcgen('demo.rpc', ['cpp'])"
```

That writes `demo.h` next to `demo.rpc`. Pass `['cpp', 'python']` to also emit `demo.py`,
a stub for the external `simplerpc` Python package. `rpcgen` takes a third keyword
argument, `archive`, which defaults to `True`; it controls whether the `serialize` /
`deserialize` functions and their `BinaryWriteArchive` / `BinaryReadArchive` operators are
emitted next to each struct. Leave it on. The generated dispatch wrappers call those
functions unconditionally, and every header committed here was generated with it on.

Generation is not part of the build. Run it by hand and commit the output, the way
`tests/benchmark_service.h` is committed here.

The generated header opens with `#include "srpc/srpc.hpp"`. Supply an include directory
whose `srpc/` entry points to this repository. The `rpcbench` CMake target creates
`build/bench-include/srpc` as a symlink to the source tree and adds `build/bench-include`
to its include path. Keep the generated include intact and use the same arrangement
for your application; [build wiring](#build-wiring) covers the other consumer settings.

### RPC method ids, and how to not break the wire

Each method gets an id from `random.randint(0x10000000, 0x70000000)`. Random ids are only
tolerable because the generator stabilizes them: before writing the header, it *reads the
header it is about to overwrite*, scrapes the `enum` of ids back out of it, and reuses any
id whose `Service.METHOD` key it recognizes. New methods get fresh ids drawn to avoid
collisions with the ones already in use.

Three consequences follow, and each of them is a way to silently break wire
compatibility:

**Never regenerate into a clean directory.** If the old header is gone, there is nothing
to scrape, and every id changes. Nothing fails loudly; the server simply answers
error 2 (`ENOENT`, the server's no-such-handler code) to every call from a peer built
against the old header.

**Renaming a method reassigns its id.** The scrape is keyed on the uppercased method name,
so a rename looks exactly like a new method. Plan for it the way you would plan any
wire-breaking change.

**Never regenerate `pylib/simplerpcgen/rpcgen.py` from `rpcgen.g`.** `rpcgen.py` is the
live generator and has been hand-edited since it was produced by yapps. `rpcgen.g` is a
stale grammar whose epilogue predates all of this: it has no `load_existing_rpc_codes`, no
`existing_codes` argument, and no `archive` flag. Regenerating would quietly drop the id
stabilization described above. There is no vendored yapps compiler;
`pylib/yapps/` contains only its runtime.

### What the generated header contains

For each service you get two classes: `<Svc>Service` and `<Svc>Proxy`. Everything else is
a *member* of `<Svc>Service`.

Per method, the generator synthesizes one request struct and one response struct from the
input and output lists. The name is `Rpc` + the method name split on `_` with each part
capitalized + `Request`/`Response`. So `dot_prod` gives `RpcDotProdRequest` and
`RpcDotProdResponse`, and `slow_echo` gives `RpcSlowEchoRequest` / `RpcSlowEchoResponse`.
Fields take their names from the IDL parameter names; unnamed parameters fall back to
`in_0`, `in_1`, … and `out_0`, `out_1`, … by position. A method with no outputs
still gets an empty response struct.

Alongside the structs, `<Svc>Service` carries an `enum` of the method ids (uppercased
method names: `SUM`, `DOT_PROD`), the `__reg_to__` registration function, the
`__dispatch__` switch, one private `__<method>__wrapper__` per non-`raw` method that does
the decoding and replying, and the typed virtual for you to override.

For `sum` above, inside `class DemoService`, that is:

```cpp srpc-no-compile
struct RpcSumRequest {
    srpc::i32 a;
    srpc::i32 b;
    srpc::i32 c;
};
struct RpcSumResponse {
    srpc::i32 result;
};

enum {
    SAYHI = 0x1234abcd,   // the real values are random draws in
    SUM   = 0x5678ef01,   // [0x10000000, 0x70000000], stable across regenerations
    // ...
};

// typed service signatures
virtual rusty::Result<RpcSumResponse, srpc::i32> sum(const RpcSumRequest& req) const = 0;
```

The structs are nested, so from outside the class they are spelled
`DemoService::RpcSumRequest`. A bare `demo::RpcSumRequest` does not exist. `DemoProxy`
re-exports each of them with a `using`, which is why client code says
`DemoProxy::RpcSumRequest`.

The generated service class has **no base class**. It does not inherit
`srpc::Service`; that interface is satisfied by a type-erasure shim the server wraps you
in. Register with `reg_service_typed`, not `reg_service`:

```cpp srpc-no-compile
svr.reg_service_typed(rusty::make_box<MyDemoService>());
```

### Implementing the server side

Inherit from the generated class and override the typed virtuals. Return `::Ok(resp)` with
the response filled in, or `::Err(code)` with an error code of your choosing; the
generated wrapper does all the serialization and sends the reply. Every generated
service method is `const`; its override must be too. The [complete Demo implementation](#the-service)
in [the walkthrough](#3-c-service-and-client-walkthrough) defines all four pure virtuals, including `sayhi` and the deferred reply.

An `async` method is written as a coroutine on the same class:

```cpp srpc-no-compile
rusty::Task<rusty::Result<BenchmarkService::RpcAsyncNopResponse, srpc::i32>>
BenchmarkService::async_nop(const RpcAsyncNopRequest& req) const {
    (void)req;
    co_return rusty::Result<RpcAsyncNopResponse, srpc::i32>::Ok(RpcAsyncNopResponse{});
}
```

### The generated client proxy

`<Svc>Proxy` wraps a `srpc::Client*` and gives every non-`raw` method three things: a
blocking call, an `async_` call, and a per-method future wrapper class named
`<method>TypedFuture`. This name uses the raw IDL method name, so `dot_prod`
yields `dot_prodTypedFuture`.

```cpp srpc-no-compile
DemoProxy demo(const_cast<srpc::Client*>(cl.get()));

DemoProxy::RpcSumRequest req;
req.a = 1; req.b = 2; req.c = 3;

// Blocking: async_sum(...).unwrap().resolve(), collapsed into one call.
auto result = demo.sum(req);
if (result.is_ok()) {
    printf("1 + 2 + 3 = %d\n", result.unwrap().result);
}

// Non-blocking: get the typed future back immediately.
auto fu = demo.async_sum(req);   // rusty::Result<sumTypedFuture, srpc::i32>
if (fu.is_ok()) {
    auto typed = fu.unwrap();
    // ... issue more calls, do other work ...
    auto resolved = typed.resolve();
    if (resolved.is_ok()) {
        printf("result = %d\n", resolved.unwrap().result);
    }
}
```

`async_<method>` takes an optional second argument, a `srpc::FutureAttr`, which is where a
completion callback goes if you would rather be notified than poll. The typed future
exposes `ready()`, `wait()`, `get_error_code()`, `raw_future()` and `resolve()`; `resolve()`
checks the error code first and returns `Err(code)` without decoding if it is nonzero.

There is no `await_<method>` method, and the typed future is not awaitable.
The generator emits no client-side `co_await` support. `rusty::Task` appears only
in the *server* handler signature for `async` methods.

The blocking form uses the underlying future's default one-second wait budget.
A pending `resolve()`, `wait()` or `get_error_code()` can therefore report error
`110` if the server takes longer. For a longer wait, use the async proxy's
`raw_future()`, set its `RequestOptions::timeout_ms`, and call
`wait_with_options()` before resolving. An ordinary future accepts these options;
request attempt deadlines and retries are separate client policies.

### Raw methods keep the old shape

A `raw` handler receives the undecoded `srpc::Request` and a weak connection
handle, then performs its own reading and replying. The generator still emits
request and response structs for the method, but the raw interfaces do not use them.

The proxy side of a `raw` method is the one place in the generated code where the old
pointer-out-parameter style survives. Inputs are passed by const reference, outputs are
passed as pointers, and the sync form returns a bare `srpc::i32` error code rather than a
`rusty::Result`:

```cpp srpc-no-compile
// For the IDL line:  raw fetch(i64 key | string val);
srpc::FutureResult async_fetch(const srpc::i64& key,
                               const srpc::FutureAttr& attr = srpc::FutureAttr());
srpc::i32 fetch(const srpc::i64& key, std::string* val);
```

Every non-`raw` handler and proxy call uses one request struct and one response
struct. Those interfaces have no pointer out-parameters.

---


## 3. C++ service and client walkthrough

This walkthrough uses the four-method `Demo` service from
[the service definition](#the-service-definition-language): `sayhi`, `sum`, `dot_prod` and
`slow_echo`. Save that IDL as `demo.rpc`, then [run the generator](#running-the-generator)
to produce `demo.h`. Keep the generated header when regenerating, because it stores
the stable RPC method IDs. The examples below are separate application files that
require that header; the generic book snippet checker does not generate it.

### The service

Put the implementation in `demo_service.hpp`. Generated request and response types
are nested in `DemoService`; the subclass can name them directly. The `abstract`
service requires all four overrides, each with the generated `const` qualifier.

```cpp srpc-no-compile
// Application header: requires the generated demo.h.
#pragma once
#include <cstdio>
#include "demo.h"

class MyDemoService : public demo::DemoService {
public:
    rusty::Result<RpcSayhiResponse, srpc::i32> sayhi(const RpcSayhiRequest& req) const override {
        std::printf("%s\n", req.hi.c_str());
        return rusty::Result<RpcSayhiResponse, srpc::i32>::Ok(RpcSayhiResponse{});
    }

    rusty::Result<RpcSumResponse, srpc::i32> sum(const RpcSumRequest& req) const override {
        RpcSumResponse resp{};
        resp.result = req.a + req.b + req.c;
        return rusty::Result<RpcSumResponse, srpc::i32>::Ok(resp);
    }

    rusty::Result<RpcDotProdResponse, srpc::i32> dot_prod(const RpcDotProdRequest& req) const override {
        RpcDotProdResponse resp{};
        resp.v = req.p1.x * req.p2.x + req.p1.y * req.p2.y + req.p1.z * req.p2.z;
        return rusty::Result<RpcDotProdResponse, srpc::i32>::Ok(resp);
    }

    void slow_echo(const RpcSlowEchoRequest& req, RpcSlowEchoResponse& resp,
                   srpc::DeferredReply defer) const override {
        resp.echoed = req.msg;
        defer.reply();
    }
};
```

Return `::Ok(resp)` and the generated wrapper serializes and replies with error
code 0; return `::Err(code)` and it replies with your code and an empty body.
For a deferred reply, fill in the response and call `reply()` or `reply_error(code)`.
Either fires at most once. Dropping the handle without replying leaves the caller
waiting until its deadline. [Dispatch modes](#choosing-how-each-method-is-dispatched)
explains when each handler runs and which ones can suspend cooperatively.
Ordinary blocking operations stop the OS worker under every dispatch mode.

### Starting the server

Save this as `demo_server.cc`. A server needs a poll thread and service registration
before it starts listening. `reg_service_typed` wraps the generated class, which does
not inherit the low-level `srpc::Service` interface.

```cpp srpc-no-compile
// Application translation unit: requires demo_service.hpp and generated demo.h.
#include "demo_service.hpp"

int main() {
    auto poll = srpc::PollThread::create();
    {
        auto svr = srpc::Server::new_(rusty::Some(poll));
        svr.reg_service_typed(rusty::make_box<MyDemoService>());

        const char* addr = "127.0.0.1:8848";
        if (svr.start(reinterpret_cast<const int8_t*>(addr)) != 0) {
            return 1;
        }

        // A coordinated control thread may call svr.do_shutdown().
        // Without one, this waits until the process is stopped externally.
        svr.wait_for_shutdown();
        svr.graceful_shutdown(30000); // Run lifecycle changes on the owning thread.
    } // Destroy the server and close accepted connections before the poll thread.
    poll->shutdown();
    return 0;
}
```

`start()` returns 0 on success and -1 on failure. Its address argument is
`const int8_t*`, hence the cast. TCP is installed automatically. Bind to port `0`
and call `get_bound_port()` if the OS should choose the port.

Register hooks before waiting. A coordinated control thread can call `do_shutdown()`
while keeping the server alive; after the wait returns, the owning thread performs
`graceful_shutdown()`, as above. These owner-thread calls illustrate the hook and
drain budget:

```cpp srpc-no-compile
// Owner-thread lifecycle fragment: install hooks before waiting, then drain on exit.
svr.add_shutdown_hook([]() { /* release application resources */ });
svr.graceful_shutdown(30000);
```

The timeout is in milliseconds. Shutdown stops acceptance, drains in-flight work,
runs the hooks and releases `wait_for_shutdown()`. Server destruction closes accepted
connections. Close clients first, destroy the server, then shut down the poll thread.
[Server lifecycle](#lifecycle-and-replies) describes the lifecycle and thread restrictions.

### Connecting the client

Save this as `demo_client.cc`. The proxy borrows the client, so keep its owner alive
for every call.

```cpp srpc-no-compile
// Application translation unit: requires the generated demo.h.
#include <cstdio>
#include "demo.h"

int main() {
    auto poll = srpc::PollThread::create();
    auto client = srpc::Client::create(poll);
    const char* addr = "127.0.0.1:8848";
    if (client->connect(reinterpret_cast<const int8_t*>(addr), true) != 0) {
        client->close();
        poll->shutdown();
        return 1;
    }

    demo::DemoProxy proxy(const_cast<srpc::Client*>(client.get()));
    demo::DemoProxy::RpcSumRequest req{};
    req.a = 1; req.b = 2; req.c = 3;

    auto result = proxy.sum(req);
    if (result.is_ok()) {
        std::printf("1 + 2 + 3 = %d\n", result.unwrap().result);
    } else {
        std::printf("RPC error: %d\n", result.unwrap_err());
    }

    client->close();
    poll->shutdown();
    return 0;
}
```

`rusty::Arc<T>::get()` returns `const T*`; the generated proxy constructor takes
`Client*`, which accounts for the cast. Run the server and client in separate
terminals. The successful reply prints `1 + 2 + 3 = 6`.

The blocking call has the same one-second limit as the underlying future. Common
errors are 107 `ENOTCONN`, 110 `ETIMEDOUT`, 16 `EBUSY` for an open circuit breaker,
and 2 `ENOENT` for an unknown RPC method. [Error codes](#error-codes) covers the full
set.

Every non-raw method also has an `async_<method>` form. The [generated proxy example](#the-generated-client-proxy)
shows issuing calls before resolving their typed futures, along with `ready()`,
`wait()`, `get_error_code()`, `raw_future()` and callback support through `FutureAttr`.
The default blocking wait is one second per wait. For a longer budget, set options
on the underlying future and call `wait_with_options()`. Request attempt and retry
policy is configured separately with `Client::request_with_options`; use
`demo::DemoService::SUM` and serialize all three request fields. The configuration
examples below distinguish the caller's wait from the attempt policy.

Further client setup is covered where each policy is explained:

- [Reconnect, heartbeat and circuit breaker configuration](#10-c-reliability-configuration),
  including buffering after `connect`. Heartbeat configuration alone does not schedule probes.
- [Connection callbacks](#10-c-reliability-configuration) and [live metrics](#client).
- [ClientPool](#clientpool) for selecting a connection to each server address.
- [In-memory transport](#an-in-memory-channel-factory) for synchronous tests
  with one shared switchboard, plus drop and send-error injection.

### Build wiring

There is no `install()` target or CMake package config. Downstream consumption uses
`add_subdirectory`, but SRPC still has top-level build assumptions. This is the
starting configuration, not a tested standalone consumer project:

```cmake
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_MODULE_STD ON)
add_compile_options(-stdlib=libc++)
add_link_options(-stdlib=libc++ -lc++abi)

add_subdirectory(srpc)

add_executable(demo_server demo_server.cc)
target_link_libraries(demo_server srpc)
add_executable(demo_client demo_client.cc)
target_link_libraries(demo_client srpc)
```

Use Clang 22 or newer with libc++. The directory-level standard-library settings
must also apply to your consumer. The `srpc` target exports its include paths and
`SRPC_CXXFLAGS`, including `-march=native`, debug/frame-pointer options and `-w`.
Supply the generated header's `srpc/srpc.hpp` include path as described in
[running the generator](#running-the-generator).

The gate targets are in `ALL` and some paths use `CMAKE_BINARY_DIR`, so an unadjusted
parent project can run gates against the wrong build directory. Pure module consumers
may also need `CXX_SCAN_FOR_MODULES OFF`, `CXX_MODULE_STD OFF` and an explicit module
map. The in-tree `rpcbench` target and `scripts/emit_module_map.py` show the working
recipe; use those target settings when adapting this sketch.

Put textual `#include`s before `import std;`. libc++ rejects headers introduced after
the module import. Keep the producer and consumer target flags consistent:
`-march=native` is required for module compatibility, and a build tree cannot be
copied between machines with incompatible CPU features.

C++ and Rust endpoints share the frame and RPC envelope formats. Applications must
also agree on method ids, field order, payload types and native-endian encoding.
The historical cross-language benchmark results used one C++ client against both
servers; they are measurements of the recorded revisions.

### Migrating from simple-rpc

The IDL is largely familiar, but application code and build wiring change:

| Earlier interface | Current SRPC interface |
| --- | --- |
| `rrr::` | `srpc::`; the shipped convenience alias is `namespace base = srpc;` |
| `sum(a, b, c, &result)` | A request struct and `rusty::Result<Response, srpc::i32>`; service methods are `const` |
| Hand-decoded handlers | The `raw` attribute retains manual decoding and its proxy's pointer-output form; `defer` uses a response reference |
| `bin/rpcgen` | Import the generator from `pylib/simplerpcgen` with Python |
| Legacy service registration | `reg_service_typed(rusty::make_box<T>())` for generated classes |
| Implicit runtime setup | Give clients and servers a `PollThread`; C++ address parameters use `const int8_t*` |
| waf | CMake and Ninja with Clang 22+ and libc++ |

Choose `abstract service` when subclassing. For a concrete generated service, define
its virtual methods out of line, as `tests/benchmark_service.cc` does. The Python
generator can also emit stubs for the external `simplerpc` package; this walkthrough
covers the C++ consumer, not that package's runtime.

---

## 4. C++ API reference

This is a selected C++ declaration reference for the generated `srpc.*` modules.
Native Rust callers should use the [Rust API guide](srpc-book.md#16-rust-api-and-verification)
and `cargo doc`; exported C++ names and native Rust visibility are not interchangeable.

These declarations describe the generated C++ APIs from canonical Rust in
`base/`, `misc/`, `reactor/` and `rpc/`. Check the current sources and generated
modules when a declaration disagrees with the implementation. For behavior, see
[events](#generated-event-apis), [the protocol](#protocol-and-transport-access-from-c),
[clients](#7-c-client-bindings), [servers](#8-c-server-bindings),
[reliability configuration](#10-c-reliability-configuration),
[service generation](#2-service-definition-and-code-generation) and
[error codes](#error-codes).
Every block below is tagged `srpc-no-compile`: these are signatures to read, not fragments
the snippet harness builds.

Names are C++ spellings. Rust `Self::new` becomes `new_` in C++ where `new` is reserved,
which is why you write `Server::new_(...)`. Ownership types come from `rusty`:
`rusty::Arc<T>`, `rusty::Rc<T>`, `rusty::Box<T>`, `rusty::Option<T>` (`rusty::Some(x)`,
`rusty::None`), `rusty::Result<T, E>`, `rusty::Function<Sig>`. A `Box<dyn Trait>` is
`rusty::Box<Trait>`, and a Rust `Vec<T>` is `rusty::Vec<T>`, not `std::vector<T>`.

Runtime signatures use C integer types such as `int32_t`, `uint64_t` and
`int8_t`, plus `size_t` for sizes. The `srpc::` integer aliases (`srpc::i8`, `srpc::i16`,
`srpc::i32`, `srpc::i64`, and only those four) exist for the *generated* IDL code, which is
why handler signatures out of `pylib` are spelled `srpc::i32` while `Client::connect` is
spelled `int8_t`. They are the same types.

`srpc.hpp` pulls in most of what follows. Eight modules carry an explicit
"trimmed from consumer umbrella" comment in `srpc.hpp` and must be imported by name if you
need their types: `srpc.circuit_breaker`, `srpc.connection_metrics`, `srpc.epoll_wrapper`,
`srpc.heartbeat`, `srpc.internal_protocol`, `srpc.load_balancer`, `srpc.reconnect_policy`,
`srpc.request_options`. That commented-out list is hand-maintained and nothing checks it.
Four more modules were never in the umbrella at all and also need naming:
`srpc.inmemory_channel` (where `make_inmemory_factory_proxy` below lives),
`srpc.any_message`,
`srpc.serializable_envelope` and `srpc.callback_wrapper`.

### Client

`Client` is always held through an `Arc`, and it is a thin front for a `ClientConnection`
that only exists after a successful `connect`.

```cpp srpc-no-compile
class Client {
    static rusty::Arc<Client> create(rusty::Arc<PollThread> poll_thread);

    // Connection lifecycle. `addr` is "host:port"; the bool selects client mode.
    int32_t connect(const int8_t* addr, bool client) const;
    void close() const;
    int32_t reconnect(OnReconnectCompleteCallbackFn on_complete) const;
    bool try_reconnect_if_needed() const;
    void pause() const;
    void resume() const;

    // Issuing requests. write_fn is any callable taking BinaryWriteArchive&.
    template<class F>
    rusty::Result<rusty::Arc<Future>, int32_t>
        request(int32_t rpc_id, const FutureAttr& attr, F write_fn) const;

    template<class F>
    rusty::Result<rusty::Arc<Future>, int32_t>
        request_with_options(int32_t rpc_id, const RequestOptions& options, F write_fn) const;

    template<class F>
    rusty::Result<rusty::Unit, int32_t>
        request_async(int32_t rpc_id, F write_fn, AsyncReplyCallback on_reply) const;

    // State.
    bool connected() const;
    bool has_connection() const;
    ConnectionState connection_state() const;
    bool is_reconnecting() const;
    bool validate_connection() const;
    rusty::String host() const;
    uint64_t server_instance_id() const;
    bool is_idle(uint64_t idle_ms, uint64_t current_time_ms) const;

    // The live connection, if any.
    rusty::Option<rusty::Arc<ClientConnection>> connection() const;

    // Pending-request bookkeeping.
    size_t pending_request_count() const;
    void clear_pending_requests(int32_t error_code) const;
    void handle_free(int64_t xid) const;

    // Configuration. Only the first four are staged and applied at connect();
    // set_buffering_config forwards to the connection, so call it AFTER connect().
    void set_keepalive(const KeepaliveConfig& config) const;
    void set_heartbeat(const HeartbeatConfig& config) const;
    void set_circuit_breaker(const CircuitBreakerConfig& config) const;
    void set_reconnect_policy(const ReconnectPolicy& policy) const;
    void set_buffering_config(const BufferingConfig& config) const;
    void set_channel_factory(NullableChannelFactoryProxy factory) const;

    KeepaliveConfig keepalive_config() const;
    HeartbeatConfig heartbeat_config() const;
    CircuitBreakerConfig circuit_breaker_config() const;
    CircuitState circuit_breaker_state() const;

    // Connection-event callbacks.
    void add_on_connected(OnConnectedCallbackFn cb) const;
    void add_on_disconnected(OnConnectedCallbackFn cb) const;
    void add_on_reconnecting(OnConnectedCallbackFn cb) const;
    void add_on_reconnected(OnReconnectedCallbackFn cb) const;
    void add_on_error(OnErrorCallbackFn cb) const;
    void clear_connection_callbacks() const;
    void set_on_server_restart(OnServerRestartCallbackFn cb) const;

    // Shared live counters, retained by the Client through close and reconnect.
    const ConnectionMetrics& metrics() const;
};
```

`request` takes exactly three arguments. A method with no input parameters
still passes an empty lambda,
which is what the generator emits. `connect` and `Server::start` take `const int8_t*`, so
call sites write `reinterpret_cast<const int8_t*>(addr)`. `metrics()` returns a reference
to shared live counters. [C++ client bindings](#7-c-client-bindings) works through the client API.

### ClientConnection

Reached with `client->connection()`, which returns an `Option`. This is where the live
state is.

```cpp srpc-no-compile
class ClientConnection {
    const ConnectionMetrics& metrics() const; // the real counters

    bool connected() const;
    ConnectionState connection_state() const;
    bool is_closed() const;
    bool is_reconnecting() const;
    rusty::String host() const;
    uint64_t server_instance_id() const;

    size_t pending_future_count() const;  // outstanding Futures
    size_t pending_request_count() const; // offline queue depth

    uint64_t last_activity_time() const;
    void update_last_activity(uint64_t current_time_ms) const;
    bool is_idle(uint64_t idle_ms, uint64_t current_time_ms) const;

    ReconnectPolicy reconnect_policy() const;
    BufferingConfig buffering_config() const;
    CircuitState circuit_breaker_state() const;
};
```

`replay_pending_requests()` sends unexpired queued bodies through the active channel
and returns the number sent. Successful reconnect invokes it automatically. Keepalive
configuration is applied through the channel's capability rather than a client-owned fd.

### Future

```cpp srpc-no-compile
class Future {
    static rusty::Arc<Future> create(int64_t xid, FutureAttr attr); // attr by value

    bool ready() const;
    void wait() const;              // uses the default one-second timeout
    void timed_wait(double sec) const;
    bool wait_with_options() const; // uses this future's RequestOptions
    bool timed_out() const;

    int32_t get_error_code() const;               // also waits
    rusty::MutexGuard<ReplyBuffer> get_reply() const; // also waits
    int64_t get_xid() const;

    RequestOptions get_options() const;
    void set_options(const RequestOptions& opts) const;
    TimeoutType get_timeout_type() const;
    uint16_t get_retry_count() const;
    bool should_retry() const;

    bool add_completion_callback(rusty::Function<void()> callback) const;

    static void safe_release(rusty::Arc<Future> fu); // consumes and releases one Arc owner
};
```

Each blocking `wait()` uses a default one-second budget. `get_error_code()` and
`get_reply()` also wait while the future is pending. C++ additionally exports
`timed_wait(seconds)`; `wait_with_options()` uses a nonzero
`RequestOptions::timeout_ms` and falls back to the default otherwise. These methods
block the OS thread. They do not yield an SRPC fiber.

Expiry sets error `110` without removing the pending request. A late reply can
replace the stored error and body without making that timed-out future ready.
Treat a reported timeout as that wait's outcome rather than expecting later
inspection to recover normal success. The [Rust timeout discussion](srpc-book.md#timeouts-and-retries)
separates wait budgets from attempt deadlines and retries.

`get_reply()` hands back a `MutexGuard<ReplyBuffer>`. Keep the guard alive for the
whole decode and release it before callbacks or waits that need the same reply lock.

`FutureAttr` carries one field, a completion callback, and default-constructs to empty. The
generated proxy passes a default-constructed one.

### ClientPool

```cpp srpc-no-compile
class ClientPool {
    static ClientPool new_(rusty::Option<rusty::Arc<PollThread>> poll_thread,
                           PoolConfig config);

    rusty::Option<rusty::Arc<Client>> get_client(std::string_view addr) const;

    void set_pool_config(PoolConfig config) const;
    PoolConfig pool_config() const;

    size_t total_client_count() const;
    size_t address_count() const;
    size_t get_healthy_client_count(std::string_view addr) const;

    size_t remove_unhealthy_clients(std::string_view addr) const;
    size_t remove_all_unhealthy() const;
    size_t close_idle_clients(std::string_view addr, uint64_t current_time_ms) const;
    size_t close_all_idle(uint64_t current_time_ms) const;

    bool is_client_healthy(const rusty::Arc<Client>& client) const;
};
```

The factory asserts `min_connections > 0` and `max_connections >= min_connections`, and
creates its own `PollThread` when handed `rusty::None`. Destroying the pool closes every
cached client and shuts that poll thread down. Health checking and `LEAST_CONNECTIONS`
use live counters from `Client::metrics()`. `LEAST_LATENCY` still needs explicit latency
samples. See [reliability configuration](#10-c-reliability-configuration) and
[performance](#12-c-performance-and-historical-measurements).

### Server

`Server` is used by value, not through an `Arc`.

```cpp srpc-no-compile
class Server {
    static Server new_(rusty::Option<rusty::Arc<PollThread>> poll_thread);

    // Registration. reg_service_typed is the one you want for a generated service.
    template<class T> void reg_service_typed(rusty::Box<T> svc);
    void reg_service(rusty::Box<Service> svc);
    void reg_service_proxy(ServiceProxy proxy);
    int32_t reg_rpc(int32_t rpc_id, size_t svc_index);
    int32_t reg_fast_rpc(int32_t rpc_id, size_t svc_index);
    void unreg(int32_t rpc_id);

    // Transport. Call set_channel_factory before start() to override TCP.
    void set_channel_factory(NullableChannelFactoryProxy factory);
    bool is_channel_factory_bound() const;

    int32_t start(const int8_t* bind_addr); // 0 on success, -1 on failure
    int32_t get_bound_port() const;         // -1 if unparseable
    rusty::String addr() const;             // only after start()

    // Shutdown. kDefaultDrainTimeoutMs is 30000.
    void stop_accepting();
    bool drain(uint64_t timeout_ms) const;
    void graceful_shutdown(uint64_t drain_timeout_ms);
    void do_shutdown() const;
    void wait_for_shutdown() const;
    void add_shutdown_hook(ShutdownHook hook) const;
    ShutdownPhase phase() const;

    // Bookkeeping.
    int32_t pending_request_count() const;
    size_t service_count() const;
    uint64_t instance_id() const;
    void set_drop_heartbeat_replies(bool drop_replies) const;
    bool drop_heartbeat_replies() const;

    template<class F> void for_each_service(F callback) const; // only after start()
};
```

Generated service classes have **no base class**. Register one with
`reg_service_typed(rusty::make_box<MyService>())`; the adapter that wraps it is
`ServiceBoxShim<T>`. `reg_service` takes a `Box<dyn Service>` and is not what you want for a
generated class. There is no `add_service` and no `stop()`.

`start()` freezes the pending registrations into an immutable `RpcServiceContext`,
auto-installs a `TcpFactory` if none is bound, creates and wires the listener, and binds.
Registrations after `start()` do not take effect.

Shutdown progresses `RUNNING -> STOP_ACCEPTING -> DRAINING -> CLOSING -> STOPPED`
(`ShutdownPhase`, with `shutdown_phase_to_string`); `graceful_shutdown` runs the whole
sequence. [Server lifecycle](#lifecycle-and-replies) covers teardown order.
Destroying the `Server` closes already-accepted connections.

### Service and dispatch

```cpp srpc-no-compile
// The interface generated services satisfy.
class Service {
    virtual int32_t __reg_to__(Server& server, size_t svc_index) = 0;
    virtual void __dispatch__(int32_t rpc_id, rusty::Box<Request> req,
                                   WeakServerConnection sconn) const = 0;
};

// One in-flight request. `src` is the cursor over the argument bytes.
struct Request {
    rusty::Vec<uint8_t> body;
    BufferSource src;
    int64_t xid;
};
```

The generated `<Svc>Service` class
declares the handlers you implement. Their shape depends on the dispatch attribute in the `.rpc` file; the
grammar accepts six (`prefix`, `fast`, `raw`, `fiber`, `defer`, `async`) plus the unattributed
default. [Service definitions](#2-service-definition-and-code-generation) explains what each one costs.

| Attribute | Generated handler signature |
| --- | --- |
| *(none)* | `rusty::Result<Rpc<M>Response, srpc::i32> m(const Rpc<M>Request&) const` |
| `fast` / `prefix` | same, but registered with `reg_fast_rpc` and dispatched inline on the poll thread |
| `defer` | `void m(const Rpc<M>Request&, Rpc<M>Response& resp, srpc::DeferredReply defer) const` |
| `fiber` | same as *(none)*, but the wrapper calls it inside a `Fiber::create_run`; see the extra fiber cost in [dispatch modes](#choosing-how-each-method-is-dispatched) |
| `async` | `rusty::Task<rusty::Result<Rpc<M>Response, srpc::i32>> m(const Rpc<M>Request&) const` |
| `raw` | `void m(rusty::Box<srpc::Request>, srpc::WeakServerConnection) const` |

The `Rpc<M>Request` / `Rpc<M>Response` structs are **members** of `<Svc>Service` and are
re-exported into `<Svc>Proxy` with `using`. Pointer out-parameters survive only on a `raw`
method's proxy.

### DeferredReply

```cpp srpc-no-compile
class DeferredReply {
    void reply();                   // send the filled-in response
    void reply_error(int32_t code); // send a header-only error reply
};
```

Both `reply()` and `reply_error()` fire at most once. A second call logs a warning
and is ignored. Dropping the handle without replying is safe. There is also a `run_async`
taking a callable, but it offloads nothing: the body invokes the callable on the calling
thread and returns 0. [Reply lifecycle](#lifecycle-and-replies) has the rest.

### Reactor

The canonical Rust runtime uses `thread_local!` storage. The transpiler maps it to
C++ thread-local storage; native Rust tests exercise the same reactor ownership
rules. Both accessors below return thread-confined `Rc` handles.

```cpp srpc-no-compile
class Reactor {
    static rusty::Rc<Reactor> get_reactor();
    static rusty::Rc<Reactor> get_disk_reactor();

    rusty::Rc<Fiber> create_run_fiber(rusty::Function<void()> func) const;
    void continue_fiber(const rusty::Rc<Fiber>& fiber) const;
    void register_fiber(const rusty::Rc<Fiber>& fiber) const;
    void recycle(rusty::Rc<Fiber>& fiber) const;

    void run_loop(bool infinite, bool do_check_timeout) const;
    void prune_finished_events() const;
    void display_waiting_ev() const;

    size_t register_stackless_poller(rusty::Function<bool(rusty::Context&)> poller) const;
    void enqueue_stackless_task(size_t idx) const;
    bool process_stackless_tasks() const;
};
```

`run_loop` takes two booleans, not one. It asserts that it is running on the thread the
reactor was created on. `create_run_fiber` runs the fiber immediately and drives the loop
internally, so a fiber that never suspends has already finished when the call returns.

### Fiber and this_fiber

```cpp srpc-no-compile
class Fiber {
    template<class F> static rusty::Rc<Fiber> create_run(F func);
    static rusty::Option<rusty::Rc<Fiber>> current_fiber();
    static void sleep(uint64_t microseconds);

    void run() const;
    void yield_() const;
    void continue_() const;
    bool finished() const;
};

namespace this_fiber {
    uint64_t get_id();                         // 0 outside fiber context
    rusty::Option<rusty::Rc<Fiber>> current();
    bool in_fiber_context();
    void yield();                              // no-op outside a fiber
    void sleep_us(uint64_t microseconds);
    void sleep_ms(uint64_t milliseconds);
    void sleep_s(uint64_t seconds);
    void sleep_until_us(uint64_t abs_time_us); // past deadlines return at once
}
```

Fibers are stackful: `kDefaultStackBytes` is 1 MiB, `mmap`'d with one extra page
`mprotect`ed `PROT_NONE` as a guard, and switched by
`reactor/fiber_context_{x86_64,aarch64}.S`. The field order of `srpc_fiber_ctx` in
`reactor/srpc_fiber.h` *is* the ABI contract with that assembly.

### Events

There is no `Event` base class in the C++ surface and no `create_sp_event<T>` template.
Events are concrete types built by **named free functions**, and the methods are lowercase.

```cpp srpc-no-compile
rusty::Arc<IntEvent> create_sp_int_event(int32_t target);
rusty::Arc<TimeoutEvent> create_sp_timeout_event(uint64_t wait_us);
rusty::Arc<NeverEvent> create_sp_never_event();
rusty::Arc<WaitAny> create_sp_waitany(rusty::Arc<EventPollable> a,
                                      rusty::Arc<EventPollable> b);
rusty::Arc<WaitAll> create_sp_waitall();
rusty::Arc<WaitAll> create_sp_waitall_from(const rusty::Vec<rusty::Arc<EventPollable>>& evs);
template<class T> rusty::Arc<BoxEvent<T>> create_sp_box_event();

// The N-of-M primitive lives in global namespace ::janus, not in srpc.
namespace janus {
    rusty::Arc<QuorumEvent> create_sp_quorum_event(int32_t n_total, int32_t quorum);
}
```

The common surface, from the `EventPollable` interface plus each type's own methods:

```cpp srpc-no-compile
bool test() const;
bool is_ready() const;
uint64_t wakeup_time() const;
bool prunable() const;
void set_prunable(bool v) const;

// Status is reachable two ways: the EventPollable methods, and the public field
// they read through. [generated events](#generated-event-apis) uses the field.
EventStatus status() const;                 // == status_.get()
void set_status(EventStatus s) const;       // == status_.set(s)
rusty::Cell<EventStatus> status_;     // public field on every concrete event

void wait() const; // IntEvent, BoxEvent, WaitAny, WaitAll, TimeoutEvent, QuorumEvent
void wait_timeout(uint64_t timeout_us) const;

int32_t IntEvent::get() const;
int32_t IntEvent::set(int32_t n) const;
template<class T> T BoxEvent<T>::get() const;
template<class T> void BoxEvent<T>::set(const T& c) const;
template<class T> void BoxEvent<T>::clear() const;
void WaitAll::add_event(rusty::Arc<EventPollable> ev) const;

void janus::QuorumEvent::vote_yes() const;
void janus::QuorumEvent::vote_no() const;
bool janus::QuorumEvent::yes() const;
bool janus::QuorumEvent::no() const;
void janus::QuorumEvent::add_xid(uint16_t site, int64_t xid) const;
void janus::QuorumEvent::remove_xid(uint16_t site) const;
void janus::QuorumEvent::finalize(uint64_t timeout, QuorumFinalizeFn f) const;
```

There is no `WaitN` or `NEvent`; `QuorumEvent` is the N-of-M primitive. The `::janus`
placement is a hard ABI contract. `srpc::QuorumEvent` and `srpc::janus::QuorumEvent` mangle
differently and are not substitutes.

`FiberPromise<T>` / `FiberFuture<T>` in `srpc.future` wrap a `BoxEvent<T>`:
`make_promise<T>()` returns the pair, `make_ready_future<T>(value)` returns a satisfied one,
and `FiberFuture<T>::wait_for(timeout_us)` treats a zero timeout as "wait forever".

### PollThread

```cpp srpc-no-compile
class PollThread {
    static rusty::Arc<PollThread> create(); // spawns exactly one OS thread

    void add_proxy(PollableProxy poll) const;
    void remove(Pollable& poll) const;
    void remove_fd(int32_t fd) const;
    void request_close(int32_t fd) const;
    void update_mode(int32_t fd, int32_t new_mode) const;
    void add(rusty::Arc<Job> job) const;
    void shutdown() const; // idempotent; skips self-join
};
```

`PollableProxy` is `rusty::Box<PollableBase>` from `srpc.pollable_proxy`; the `Pollable`
that `remove` takes comes from `srpc.epoll_wrapper`, one of the modules trimmed out of the
umbrella.

Registration, removal, close, mode-update and job methods post commands through
an mpsc channel to the worker. They return before the worker applies the command.
If the send fails, `update_mode` logs
`PollThread::update_mode: send failed! Channel disconnected?` at ERROR;
the other methods discard that failure.
`get_remove_count()` reads an atomic count of accepted `remove_fd` commands. It includes
unregistered descriptors and excludes commands rejected after shutdown.

### Channel layer

`rpc/channel.rs` is the transport facade. TCP (`srpc.tcp_channel`) and in-memory
(`srpc.inmemory_channel`) implement it; `FiberChannel` is not an implementation but an
adapter that turns callback delivery into a fiber-blocking `recv_frame()`.

```cpp srpc-no-compile
enum class ChannelError {
    None = 0, WouldBlock, ConnectionRefused, ConnectionReset, Timeout,
    AddressInUse, AddressInvalid, PermissionDenied, TooManyOpenFiles, Internal,
};
std::string_view channel_error_to_string(ChannelError error);

struct ChannelFrame { const uint8_t* payload; size_t size; };

class ChannelConnectionBase {
    virtual ChannelError send_frame(const ChannelFrame& frame) const = 0;
    virtual void flush() const = 0;
    virtual void close() const = 0;
    virtual bool is_closed() const = 0;
    virtual rusty::String peer_address() const = 0;
    virtual void set_on_frame(OnFrameCallback cb) = 0;
    virtual void set_on_closed(OnClosedCallback cb) = 0;
    virtual void set_on_error(OnErrorCallback cb) = 0;
};

class ChannelListenerBase {
    virtual ChannelError listen(std::string_view address) = 0;
    virtual void close() = 0;
    virtual bool is_closed() const = 0;
    virtual rusty::String local_address() const = 0;
    virtual void set_on_accept(OnAcceptCallback cb) = 0;
    virtual void set_on_error(OnErrorCallback cb) = 0;
};

struct ConnectResult {
    rusty::Option<ChannelConnectionProxy> connection;
    ChannelError error;
};

class ChannelFactoryBase {
    virtual ConnectResult connect(std::string_view address) = 0;
    virtual rusty::Option<ChannelListenerProxy> make_listener() = 0;
    virtual rusty::String backend_name() const = 0;
};
```

The handle typedefs are `ChannelConnectionProxy`, `ChannelListenerProxy` and
`ChannelFactoryProxy`, all `rusty::Box<dyn ...>`. To install a non-default transport, build
the factory and hand it over *before* `connect` / `start`:
`make_tcp_factory_proxy(arc_of_tcp_factory)` and `make_inmemory_factory_proxy(...)`.
The latter needs `import srpc.inmemory_channel;`, which the umbrella does not supply.
`TcpFactory::set_connect_timeout_ms(i32)` defaults to 5000.

### Framing and wire format

```cpp srpc-no-compile
// srpc.frame_codec
constexpr size_t kFrameHeaderSize = 4;
constexpr int32_t kMaxFramePayloadSize = 64 * 1024 * 1024;

enum class FrameDecodeStatus { NeedMoreBytes = 0, Complete = 1, Malformed = 2 };

// srpc.internal_protocol (not in the srpc.hpp umbrella)
constexpr int32_t kInternalHeartbeatRpcId = INT32_MIN;
constexpr uint32_t kResponseHeaderExtFlag = 0x80000000;
constexpr uint32_t kResponseSizeMask = 0x7fffffff;

bool response_has_extended_header(int32_t encoded_size);
int32_t response_payload_size(int32_t encoded_size);
int32_t encode_response_size(int32_t payload_size, bool extended_header);
```

The header is 4 bytes, **native-endian**: bit 31 is the extended-header flag, bits 0 through 30 the
payload size. A request body is `v64 xid` (a varint of 1 through 9 bytes) then `i32 rpc_id` (fixed four
bytes) then the arguments. A reply body is `v64 xid`, `v32 error_code`,
`v64 server_instance_id`, then the payload. The [Rust protocol chapter](srpc-book.md#7-rpc-protocol) walks the format byte by byte,
including why the extended flag is vestigial.

### Serialization

There is no `Marshal` class. Serialization goes through two free-function dispatchers over
an archive:

```cpp srpc-no-compile
// The two dispatchers the generated code calls.
template<class T> void srpc::Serialize_::serialize(const T& value, BinaryWriteArchive& ar);
template<class T> void srpc::Deserialize_::deserialize(T& value, BinaryReadArchive& ar);

// Archives hold a type-erased proxy over a sink or source.
struct BinaryWriteArchive { SinkProxy sink_; };
struct BinaryReadArchive {
    SourceProxy source_;
    DecodeError error_;
    bool failed() const;
};

// Concrete backings.
struct BufferSink { rusty::Vec<uint8_t> bytes; };
struct BufferSource {
    BufferSource(const uint8_t* data, size_t len);
    size_t pos() const;
    size_t remaining() const;
    bool eof() const;
};

// Proxy factories.
SinkProxy make_sink_proxy_buffer(BufferSink* sink);
SourceProxy make_source_proxy_buffer(BufferSource* source);
SinkProxy make_sink_proxy_fd(FdSink* sink);
SourceProxy make_source_proxy_fd(FdSource* source);
```

There is no bare `make_source_proxy`. Building a read archive over a reply looks like the
generated code's own decode:

```cpp srpc-no-compile
auto guard = fu->get_reply();
srpc::BinaryReadArchive ar(srpc::make_source_proxy_buffer(&guard->src));
srpc::Deserialize_::deserialize(resp.field, ar);
```

`Serialize_::serialize` resolves through an ADL bridge, so a user type joins the protocol by
declaring `serialize` / `deserialize` overloads in its own namespace.
The generator emits those overloads for each struct in a `.rpc` file. [C++ serialization](#9-c-serialization) covers the rest.

### Configuration structs and their defaults

All eight are plain aggregates with static factory functions. Where a `defaults()` exists it
is an alias for `new_()`; `KeepaliveConfig` and `ReconnectPolicy` have no `defaults()` at
all, so spell those `KeepaliveConfig::new_()` and `ReconnectPolicy::new_()`.
A fresh `Client` stages its own defaults, described in
[reliability configuration](#10-c-reliability-configuration) and the
[Rust configuration table](srpc-book.md#what-is-staged-and-what-is-not).

```cpp srpc-no-compile
struct RequestOptions {        // srpc.request_options. not in the umbrella
    uint64_t timeout_ms;       // 1000
    uint64_t total_timeout_ms; // 0 = unlimited
    uint16_t max_retries;      // 0
    uint16_t base_delay_ms;    // 50
    uint16_t max_delay_ms;     // 5000
    float jitter_factor;       // 0.1
    bool idempotent;           // false

    static RequestOptions defaults();
    static RequestOptions with_retry(uint16_t max_retries, uint64_t timeout_ms);
    static RequestOptions idempotent_retry(uint16_t max_retries);
    static RequestOptions no_timeout(); // timeout_ms = 0
    static RequestOptions fast();       // 100ms, 2 retries, idempotent
    static RequestOptions patient();    // 10s / 60s total, 5 retries, idempotent

    bool can_retry(uint16_t current_retry_count) const;
    uint64_t calculate_delay_ms(uint16_t attempt) const;
    bool is_total_timeout_exceeded(uint64_t elapsed_ms) const;
    uint64_t remaining_time_ms(uint64_t elapsed_ms) const;
};
```

`can_retry` is `idempotent && current_retry_count < max_retries`. Both conditions must hold,
and `request_with_options` forces `max_retries = 0` on a non-idempotent request. `defaults()`
leaves `idempotent` false; the four presets other than `no_timeout()` set it true.

```cpp srpc-no-compile
struct PoolConfig {                       // defaults()   aggressive()  conservative()
    int32_t min_connections;              // 1            2             1
    int32_t max_connections;              // 4            8             2
    uint64_t idle_timeout_ms;             // 300000       60000         600000
    bool health_check_enabled;            // true (also no_health_check())
    uint64_t unhealthy_threshold_percent; // 50 / 70 / 30
    uint64_t min_requests_for_health;     // 10 / 5 / 20
    LoadBalancingStrategy load_balancing; // RANDOM
};

struct BufferingConfig {         // defaults()
    DisconnectBehavior behavior; // QUEUE   (or FAIL_FAST)
    size_t max_pending;          // 1000
    uint32_t default_ttl_ms;     // 30000
    OverflowStrategy overflow;   // DROP_OLDEST
    bool enabled;                // true    (disabled() flips both)
};

struct KeepaliveConfig {  // new_()      aggressive()  relaxed()
    bool enabled;         // true        true          true
    int32_t idle_sec;     // 60          10            60
    int32_t interval_sec; // 10          2             10
    int32_t count;        // 5           3             5
};                        // also disabled(); no defaults()

struct HeartbeatConfig {  // defaults()
    bool enabled;         // true
    uint32_t interval_ms; // 10000
    uint32_t timeout_ms;  // 5000
    uint32_t max_missed;  // 3
};

struct CircuitBreakerConfig {   // defaults()
    uint32_t failure_threshold; // 5
    uint32_t success_threshold; // 3
    uint32_t timeout_ms;        // 30000
    bool enabled;               // true
};

struct ReconnectPolicy {       // new_(); conservative() is the same values.
    bool auto_reconnect;       // true    aggressive(): true / 0 / 100 / 5000 / 1.5 / true
    uint32_t max_retries;      // 5       no_retry():   false / 0 / 0 / 0 / 1.0 / false
    uint32_t initial_delay_ms; // 1000
    uint32_t max_delay_ms;     // 30000
    double backoff_multiplier; // 2.0
    bool jitter_enabled;       // true
};                             // no defaults()

struct RequestQueueConfig {             // defaults()
    size_t max_size;                    // 1000
    uint32_t default_ttl_ms;            // 30000
    OverflowStrategy overflow_strategy; // DROP_OLDEST
    bool enabled;                       // true
};
```

### Enumerations

The blocks below give the value names and their numeric assignments, which is what you need
to read a log line or a stored code. They are not a transcription of the emitted
declaration. At use sites the values are spelled `Type::VALUE`.

```cpp srpc-no-compile
enum class ConnectionState { NEW=0, CONNECTING, CONNECTED, DISCONNECTING, DISCONNECTED, FAILED };
enum class CircuitState { CLOSED=0, OPEN, HALF_OPEN };
enum class ShutdownPhase { RUNNING, STOP_ACCEPTING, DRAINING, CLOSING, STOPPED };
enum class EventStatus { INIT=0, WAIT, READY, DONE, TIMEOUT, DEBUG };
enum class TimeoutType { NONE=0, CONNECT_TIMEOUT, REQUEST_TIMEOUT, RESPONSE_TIMEOUT, TOTAL_TIMEOUT };
enum class OverflowStrategy { DROP_OLDEST=0, DROP_NEWEST, FAIL_FAST };
enum class DisconnectBehavior { QUEUE, FAIL_FAST };
enum class LoadBalancingStrategy { RANDOM=0, ROUND_ROBIN, LEAST_CONNECTIONS, LEAST_LATENCY };
enum class ServerConnStatus { CONNECTED, CLOSED };
```

Most have a `*_to_string` free function: `connection_state_to_string`,
`circuit_state_to_string`, `shutdown_phase_to_string`, `timeout_type_to_string`,
`overflow_strategy_to_string`, `load_balancing_strategy_to_string`,
`channel_error_to_string`, `frame_decode_status_to_string`. Three of the nine above have
none: `EventStatus`, `DisconnectBehavior` and `ServerConnStatus`.
There is no `event_status_to_string` to call.

### Error codes

Two numbering schemes live side by side. `Future::get_error_code()` and
generated proxies return errno-style integers, including application reply codes.
The client exports `CLIENT_ERR_*` constants from `rpc/client.rs`. The server
exports `SERVER_ERR_NO_ENTRY` from `rpc/server.rs`; its invalid-argument and
duplicate-registration constants remain private in canonical Rust. The [Rust protocol](srpc-book.md#7-rpc-protocol) and [troubleshooting](srpc-book.md#15-troubleshooting) chapters explain what each code means in context and what to do
about it.

| Value | Constant | |
| --- | --- | --- |
| 0 | - | success |
| 2 | `SERVER_ERR_NO_ENTRY` | no handler registered for that rpc id |
| 5 | `CLIENT_ERR_IO` | send failed at the channel layer |
| 11 | `CLIENT_ERR_AGAIN` / `CLIENT_ERR_WOULD_BLOCK` / `CLIENT_REQUEST_QUEUE_REJECTED_ERROR` | the last is the offline queue refusing an incoming request or evicting an older one; it is 11 on Linux and 35 only on macOS (`#[cfg(target_os = "macos")]`) |
| 16 | `CLIENT_ERR_BUSY` | circuit breaker open, or async slot occupied |
| 17 | *(server-side `EEXIST`)* | returned by `reg_rpc` for a duplicate id |
| 22 | `CLIENT_ERR_INVALID_ARGUMENT` | also what the server replies for a truncated request frame |
| 32 | `CLIENT_ERR_BROKEN_PIPE` | |
| 101 | `CLIENT_ERR_NETWORK_UNREACHABLE` | |
| 103 | `CLIENT_ERR_CONNECTION_ABORTED` | |
| 104 | `CLIENT_ERR_CONNECTION_RESET` | |
| 107 | `CLIENT_ERR_NOT_CONNECTED` | no connection, or the channel is closed |
| 110 | `CLIENT_ERR_TIMED_OUT` | a future wait or request attempt timed out |
| 111 | `CLIENT_ERR_CONNECTION_REFUSED` | |
| 113 | `CLIENT_ERR_HOST_UNREACHABLE` | |
| 125 | `CLIENT_ERR_CANCELED` | |

**`RpcError`** in `srpc.errors` is a separate, categorized enumeration used by the
connection-error callback (`add_on_error`) and by `clientconn_map_system_error`. It does not
appear in a future's error code.

```cpp srpc-no-compile
enum class RpcError {
    OK = 0,
    // 100–199 connection
    NOT_CONNECTED = 100, CONNECTION_REFUSED, CONNECTION_RESET,
    NETWORK_UNREACHABLE, HOST_UNREACHABLE, CONNECTION_CLOSED, CIRCUIT_OPEN,
    // 200–299 protocol
    INVALID_MESSAGE = 200, UNKNOWN_RPC_ID, MARSHALLING_ERROR,
    VERSION_MISMATCH, CHECKSUM_ERROR,
    // 300–399 application
    RPC_FAILED = 300, SERVICE_UNAVAILABLE, PERMISSION_DENIED,
    INVALID_ARGUMENT, NOT_FOUND, ALREADY_EXISTS,
    // 400–499 timeout
    CONNECT_TIMEOUT = 400, REQUEST_TIMEOUT, RESPONSE_TIMEOUT,
    IDLE_TIMEOUT, HEARTBEAT_TIMEOUT,
    // 500+ internal
    UNKNOWN_ERROR = 500, OUT_OF_MEMORY, INVALID_STATE, INTERNAL_ERROR,
};

enum class RpcErrorCategory { NONE=0, CONNECTION, PROTOCOL, APPLICATION, TIMEOUT, INTERNAL };

std::string_view rpc_error_to_string(RpcError err);
std::string_view rpc_error_category_to_string(RpcErrorCategory cat);
RpcErrorCategory get_error_category(RpcError err);
bool is_connection_error(RpcError err); // 100–199
bool is_timeout_error(RpcError err);    // 400–499
bool is_retryable_error(RpcError err);
```

`is_retryable_error` is an explicit list, not a range: `CONNECTION_RESET`,
`NETWORK_UNREACHABLE`, `HOST_UNREACHABLE`, `CONNECT_TIMEOUT`, `REQUEST_TIMEOUT`,
`RESPONSE_TIMEOUT` and `SERVICE_UNAVAILABLE`. The request path does not consult
it; callers can use it for their own policy.

---


## 5. Ownership and translation checks

### Rust checks and generated behavior

The canonical source is checked by rustc. CMake runs Rust tests, clippy, source
ownership checks and the native binding audits before accepting generated C++.
Those checks establish source contracts; generated C++ also needs compilation,
ABI checks and runtime tests to establish that it implements them.

The compiler lowers ordinary Rust owners, borrowed values and callbacks into C++
types and moves. Explicit mappings preserve selected C++ interfaces whose
representation differs from Rust, including nullable callbacks and STL containers.
The [migration record](dev/facade-and-runtime-remaining.md) describes the reviewed
boundaries and acceptance results.

### The ownership types you actually hold

| C++ type | Role |
| --- | --- |
| `rusty::Box<T>` | Unique ownership, including a service passed to `reg_service_typed` and an owned request |
| `rusty::Arc<T>` | Shared ownership; the contained type still determines which operations may cross threads |
| `rusty::Rc<T>` | Shared ownership on one thread, used by reactors and fibers |
| `rusty::Weak` / `WeakServerConnection` | Non-owning references that may expire |
| `rusty::Option<T>` | Optional values such as a client's current connection |
| `rusty::Result<T, E>` | Results that require a success or failure decision |

An `Arc<Client>` does not authorize simultaneous application calls from multiple
threads. Likewise, a `const` C++ method can modify interior state. Follow the
canonical type's synchronization and lifecycle contract, rather than inferring it
from the pointer wrapper or the method qualifier.

Check optional connections before reading their metrics:

```cpp srpc-no-compile
// Context: cl is a live Client owner.
auto conn = cl->connection();
if (conn.is_some()) {
    printf("%lu\n", conn.unwrap()->metrics().requests_sent());
}
```

Weak references avoid cycles between callbacks, connections and their owners.
Upgrading a weak connection can fail after close. Deferred callbacks must retain the
owners they need and tolerate an expired peer.

### Mutation and thread ownership

Canonical Rust distinguishes thread-local `Cell`/`RefCell` state from synchronized
state. Services use a shared dispatch receiver and satisfy `Send + Sync`.
Connections, request queues, futures and reliability managers use mutexes, atomic
fields or synchronized wrappers where concurrent access is supported.

The reactor's fibers and events stay on their owning thread. A paused stackful
handler can overlap another handler on that same reactor, so application locks or
exclusive borrows must not survive a cooperative suspension if another handler
needs them. Ordinary OS-blocking operations remain blocking even inside a fiber.

Native FFI, raw archive pointers and context switching retain explicit lifetime
preconditions. Rust's unsafe-code allowances do not prove these preconditions, and
C++ callers must honor them too.

### Compiler metadata and ABI

Attributes under `#[cfg_attr(any(), ...)]` are inactive for rustc and interpreted by
the transpiler. They describe C++ contracts such as ABI spellings, namespace choices,
exception specifications and ownership mappings. Removing one may leave Cargo green
while changing the generated library.

The opposite form, `#[cfg_attr(not(any()), ...)]`, can supply Rust-only derives.
Read the current metadata and [CLAUDE.md](../CLAUDE.md) when changing a canonical
source. The source and ABI inventories require measured, reviewed changes.

There is no remaining inline Rust DSL implementation carrier. Generated module
providers come from the canonical `.rs` files; handwritten compatibility headers
forward or adapt the C++ boundary.

### The legacy borrow-check target

`ENABLE_BORROW_CHECKING` remains a CMake option, but its old list of handwritten C++
carriers is empty. The `borrow_check_srpc` target therefore reports no configured
files. Enabling it does not add analysis of the canonical Rust or the generated
modules. Rust compilation and the translation/runtime checks described above are
the active checks.

### Testing translated ownership

The runtime tests cover callback lifetimes, cancellation, suspended receivers,
connection close, shared state and retained wakers. The sanitizer configurations
instrument the C++ build, including its runtime and generated modules. They do not
instrument a separately launched Cargo binary.

Fiber stack switching lacks complete sanitizer fiber annotations, and the address
run retains documented allocation suppressions. Passing these tests is bounded
evidence, not a proof that suppressed paths are leak-free. See the
[acceptance record](dev/facade-and-runtime-remaining.md) for the exact limits.

The C++ examples use `srpc-compile*` tags for snippets checked against the configured
module map and `srpc-no-compile` for declaration sketches or contextual fragments.
The documentation checker preserves that distinction when examples move between
chapters.

---

---

## 6. Runtime APIs in C++

### Fibers and coroutines in C++

The [Rust book's fiber chapter](srpc-book.md#3-fibers) describes scheduling,
stack ownership, explicit continuation, and teardown. Those behaviors also apply
to the generated C++ library. This chapter covers the consumer spellings and
coroutine integration.

#### Fiber and reactor names

`#include "srpc.hpp"` exposes the generated `srpc::Fiber`, `srpc::Reactor`, and
`srpc::this_fiber` declarations. Their module owners are `srpc.reactor` and
`srpc.fiber`. A Rust `Rc<Fiber>` becomes the runtime's `rusty::Rc<Fiber>` handle,
and the Rust raw identifier `this_fiber::r#yield()` becomes
`srpc::this_fiber::yield()`.

C++ ownership wrappers do not make a local reactor or event safe to use from
another thread. Use the shared `PollThread` handle or a copied waker to notify an
owner. Plain yield still requires an explicit continuation; it does not enqueue
the fiber for automatic resumption.

#### The native context layout

The native C fiber engine is shared by both builds. Its context layout must
match the assembly offsets. This excerpt is the x86_64 declaration from
`reactor/srpc_fiber.h`, shown for engine maintenance rather than application code.

```c
typedef struct srpc_fiber_ctx {
    void* rsp;       /* offset 0 */
    void* rip;       /* offset 8 */
    uintptr_t rbx;   /* offset 16 */
    uintptr_t rbp;   /* offset 24 */
    uintptr_t r12;   /* offset 32 */
    uintptr_t r13;   /* offset 40 */
    uintptr_t r14;   /* offset 48 */
    uintptr_t r15;   /* offset 56 */
} srpc_fiber_ctx;
```

The header gives `fiber_swap_context` C linkage for C++ consumers. The aarch64
layout has thirteen words for `sp`, `pc`, `x19` through `x28`, and `fp`.
Canonical Rust supplies the entry callback in both builds; generated C++ does
not own a separate stack allocator or context-switch engine.

#### Generated async handlers

A canonical Rust future becomes `rusty::Task<T>` in generated C++. Rust `.await`
becomes `co_await`, and task completion uses `co_return`. The IDL `async` attribute
produces a handler with this shape. This is a signature illustration; the method
and request/response names stand for generated service declarations.

```cpp srpc-no-compile
virtual rusty::Task<rusty::Result<RpcMethodResponse, srpc::i32>>
method(const RpcMethodRequest& req) const;
```

The generated dispatcher registers the method on the fast path, invokes it on
the delivering thread, and passes the returned task to
`reactor_spawn_stackless_task_with_result`. Its completion callback upgrades the
weak server connection and replies. Const qualification belongs to the generated
virtual contract, including the out-of-class definition.

This definition fragment requires the generated benchmark service declaration
and its response type; it illustrates the coroutine return expression.

```cpp srpc-no-compile
rusty::Task<rusty::Result<BenchmarkService::RpcAsyncNopResponse, srpc::i32>>
BenchmarkService::async_nop(const RpcAsyncNopRequest& req) const {
    (void)req;
    co_return rusty::Result<RpcAsyncNopResponse, srpc::i32>::Ok(
        RpcAsyncNopResponse{});
}
```

Spawning polls the task once inline. A task that completes immediately also runs
its completion callback inline. Before the first suspension, an async handler
has the same nonblocking requirement as a fast handler.

SRPC does not provide ready-made coroutine awaiters for its event types. A
hand-written C++ awaiter obtains the current poll context through
`rusty::current_context()`, copies the `Waker`, and wakes that copy when its
condition becomes ready. Never retain an alias to the reactor's temporary
`Context`. A copied waker can outlive task completion or owner teardown safely,
but a completed or cancelled task will not resume again.

### Reactor ownership in C++

The generated reactor thread-local storage follows canonical `thread_local!`
state. Each thread calling `Reactor::get_reactor()` gets its own scheduler.
`PollThread` is the shared command handle; `PollThreadWorker` owns the actual
poll loop and local reactor. `run_loop(false, true)` drives ready tasks, events,
and deadlines on the owner thread. It does not perform socket polling by itself.

The common scheduling details, event queues, timer limitations, and fiber reuse
rules are in the [Rust reactor chapter](srpc-book.md#4-the-reactor-pattern).
Calling an API through a C++ wrapper does not remove those owner-thread rules.

### Generated event APIs

The Rust `EventPollable` trait becomes the generated abstract base for concrete
events. Use the named free factories, such as `create_sp_int_event`,
`create_sp_timeout_event`, and `create_sp_waitall`; there is no generic
`Reactor::create_sp_event<T>()` constructor. Factories initialize weak-self and
reactor registration state that aggregate construction would omit.

`BoxEvent<T>` requires default construction and copying in C++, matching Rust's
`Default + Clone` bounds. `get()` copies the value and `clear()` resets it.
`FiberPromise<T>` and `FiberFuture<T>` use that same state and are separate from
`rusty::Task<T>` coroutines. The promise allows one future retrieval and one
value assignment.

#### The quorum namespace is global janus

The quorum types are exported in the global `::janus` namespace:
`janus::QuorumPolicy`, `janus::QuorumEvent`, and `janus::QuorumEventWrapper`.
The associated factory is `janus::create_sp_quorum_event`. Their module owner
remains `srpc.reactor`; their namespace is not nested inside `srpc`.

This placement is an ABI contract. `srpc::QuorumEvent` or
`srpc::janus::QuorumEvent` would have different symbols and RTTI. Canonical source
uses inert `cpp_namespace(::janus)` markers on three types and five free
functions to preserve it. Ordinary event types remain in `srpc`.

Quorum voting, cleanup snapshots, timeout policy, and single-waiter limitations
are shared runtime behavior. See the [Rust event chapter](srpc-book.md#5-event-system)
for those details. `SharedIntEvent` methods are non-const and require exclusive
access across a suspended wait; C++ references do not provide a synchronization
mechanism for it.

### Polling interfaces in C++

`srpc.hpp` includes reactor and pollable-proxy declarations but omits the
`srpc.epoll_wrapper` module. Naming `Epoll`, `PollMode`, `PollReady`, or `Pollable`
requires an explicit import. This file-scope include fragment is illustrative;
the consumer also needs the project's configured module map and compiler flags.

```cpp srpc-no-compile
#include "srpc.hpp"
import srpc.epoll_wrapper;
```

The worker stores `PollableProxy`, emitted as `rusty::Box<PollableBase>`, and
calls its nine virtual operations. Implementing the older `Pollable` interface
alone does not make an object a worker registration.

#### Typed shared adapters

The generated `PollableArcShim<T>` forwards the pollable operations to an owned
`rusty::Arc<T>`. `make_pollable_proxy_from_typed_arc` constructs it. C++ can
instantiate that template structurally for a type with the required methods;
the canonical Rust helper instead has a private trait bound and is not an
extension trait for downstream Rust implementations.

The shared owner must retain its registered native descriptor. A logical close
that drops or replaces a descriptor slot needs a separate registration lease,
which the TCP-specific proxy factories provide. An `Arc` to the transport
object alone does not guarantee the interior descriptor remains alive.

#### Jobs

Rust's `Job` trait becomes an abstract class with `Ready`, `Work`, and `Done`
virtuals. They are mutable operations. C++ implementers must preserve the same
exclusive worker execution contract as an `unsafe impl Job` in Rust.
`OneTimeJob` takes an owned callback and runs it once per submission. The worker
never uses `Done()` to reschedule it.

The C++ job set is keyed by shared object identity, so a remove command must
refer to the same `Arc` that was submitted. Ready jobs execute in stackful fibers.
The full epoll error policy, command ordering, and descriptor-lifetime rules are
in the [Rust I/O chapter](srpc-book.md#6-io-layer-polling-and-connections).

### Protocol and transport access from C++

The [Rust protocol chapter](srpc-book.md#7-rpc-protocol) specifies the common
wire format, frame-size limit, instance IDs, sparse integers, and current
callback-slot limitation. C++ peers use the same native-endian encoding.

The generated proxy writes arguments through `Client::request` and decodes a
successful response through its typed resolver. It checks the response error
before reading return values. Dispatcher wrappers route replies through the
same canonical `sconn_reply` encoder, including deferred and coroutine replies.

#### Generated method IDs

`rpcgen` initially assigns method IDs in `[0x10000000, 0x70000000]` and stores them
in the generated service declaration. On regeneration it reads the existing
header and reuses those IDs. Keep that generated header under version control;
deleting it before regeneration can silently assign incompatible IDs. The
reserved internal heartbeat ID, `i32::MIN`, lies outside the generator's range.

#### An in-memory channel factory

The in-memory transport is not included by the umbrella, so import
`srpc.inmemory_channel` explicitly. Both endpoints must run in the same process
and share one switchboard. Register the service, then install the factories
before starting or connecting.

This contextual setup fragment requires an existing `Server svr` and owned
client handle `cl`. Put the import at file scope and the remaining setup inside
the function that owns them. The separate-process server/client walkthrough
cannot share this switchboard.

```cpp srpc-no-compile
import srpc.inmemory_channel;
using namespace srpc;

auto switchboard = rusty::Arc<InMemorySwitchboard>::new_(InMemorySwitchboard::new_());
auto make_factory = [&]() {
    auto factory = rusty::Arc<InMemoryFactory>::new_(
        InMemoryFactory::new_(switchboard.clone()));
    return make_inmemory_factory_proxy(std::move(factory));
};

svr.set_channel_factory(make_factory());
svr.start(reinterpret_cast<const int8_t*>("inmemory://demo"));
cl->set_channel_factory(make_factory());
cl->connect(reinterpret_cast<const int8_t*>("inmemory://demo"), true);
```

The exact address string selects the listener; no URI scheme is parsed.
Delivery is synchronous and transports complete RPC bodies, so this setup does
not test TCP fragmentation or epoll scheduling. Fault injection can drop,
duplicate, or reject selected sends, as described in the Rust protocol chapter.

#### Header and module exposure

`srpc.internal_protocol` needs an explicit import when used directly.
`srpc.hpp` imports `srpc.frame_codec`, which makes `kFrameHeaderSize`,
`kMaxFramePayloadSize`, `FrameHeader`, and `FrameDecodeStatus` visible through
the umbrella. Preserve the project's include-before-`import std` order and
configured module map when combining textual headers and modules.

---

## 7. C++ client bindings

The main book's client chapter describes request, timeout, retry, queue, and teardown behavior. The generated C++ bindings use the same canonical request paths, with C++ handle and callback spellings.

`srpc.hpp` imports `srpc.client`. Add `import srpc.request_options;` when naming request options and `import srpc.load_balancer;` when naming load-balancing strategies. Keep textual includes before module imports, as described in the build chapter.

### Creating a client and connecting

The raw address pointer is `const int8_t*`. C++ string literals need a cast:

```cpp srpc-no-compile
#include "srpc.hpp"

int main() {
    auto poll = srpc::PollThread::create();
    {
        auto client = srpc::Client::create(poll.clone());
        int32_t error = client->connect(
            reinterpret_cast<const int8_t*>("127.0.0.1:8848"), true);
        if (error == 0) {
            // Issue requests.
            client->close();
        }
    }
    poll->shutdown();
}
```

A successful connect returns zero; common failures are 111 for refused connection, 22 for an invalid address, and 107 for other factory connection failures. The default factory is TCP. Preserve the close-before-worker-shutdown ordering from the main book.

The client is a `rusty::Arc<Client>`, and `connection()` returns `rusty::Option<rusty::Arc<ClientConnection>>`. Empty options must be checked before unwrapping. A C++ handle does not enforce the native Rust client's thread restrictions, so preserve owner-thread access in application code.

### Requests and typed futures

The ordinary method is `request(rpc_id, attr, write_fn)`, including for methods without arguments. Use an empty lambda for an empty argument body. The result is `rusty::Result<rusty::Arc<Future>, srpc::i32>`.

This function expects a method with one `i64` input and one `i64` result:

```cpp srpc-no-compile
#include "srpc.hpp"

void call_i64(const srpc::Client& client, int32_t method, int64_t argument) {
    auto submitted = client.request(method, srpc::FutureAttr{},
        [argument](srpc::BinaryWriteArchive& archive) {
            srpc::Serialize_::serialize(argument, archive);
        });
    if (submitted.is_err()) {
        return;
    }
    auto future = submitted.unwrap();
    if (future->get_error_code() != 0) {
        return;
    }
    int64_t answer = 0;
    srpc::deserialize_from(future->get_reply(), answer);
}
```

The generated typed proxy and typed future wrap these same operations. A typed future's `wait()`, `get_error_code()`, and `resolve()` still reach the underlying future's blocking wait. They do not create a cooperative wait or remove the one-second default.

`get_reply()` returns a guard by value. `deserialize_from` consumes it; each call advances the reply cursor. Do not bind a borrowed reference to a temporary guard, or retain a guard while blocking elsewhere.

The generated C++ class exposes methods that remain private in native Rust, including transaction-ID access and `timed_wait`. A C++ caller that has the transaction ID can use `client.handle_free(xid)` to remove a pending map entry after abandoning a request. A timeout itself does not remove it.

### Request options

Import the module explicitly. Attempt options and the caller's coordinator wait budget are separate:

```cpp srpc-no-compile
#include "srpc.hpp"
import srpc.request_options;

void retry_call(const srpc::Client& client, int32_t method, int64_t key) {
    auto options = srpc::RequestOptions::with_retry(2, 250);
    options.total_timeout_ms = 2000;
    auto submitted = client.request_with_options(method, options,
        [key](srpc::BinaryWriteArchive& archive) {
            srpc::Serialize_::serialize(key, archive);
        });
    if (submitted.is_ok()) {
        auto future = submitted.unwrap();
        auto caller_wait = options;
        caller_wait.timeout_ms = 2500;
        future->set_options(caller_wait);
        future->wait_with_options();
    }
}
```

The idempotency requirement and retry error behavior are the same as in Rust. Request-level retries do not automatically use the separate idempotency utility.

### Being notified instead of waiting

C++ exposes the callback-taking `FutureAttr` form. The callback receives an owning future handle:

```cpp srpc-no-compile
#include "srpc.hpp"

void call_with_completion(const srpc::Client& client, int32_t method) {
    srpc::FutureAttr attr{srpc::FutureCallback::from_callable(
        [](rusty::Arc<srpc::Future> future) {
            if (future->get_error_code() == 0) {
                int32_t value = 0;
                srpc::deserialize_from(future->get_reply(), value);
            }
        })};
    auto submitted = client.request(method, attr,
        [](srpc::BinaryWriteArchive&) {});
}
```

It runs from reply notification on the delivering thread. Keep it short. The additional `add_completion_callback` form runs before the attribute callback, but rejects registration once the future is ready or timed out, so installing it after submission races completion.

These entry points are not currently public in native Rust. Rust applications use `request_async` for callback-based completion.

`request_async` accepts the generated callable alias directly, with signature `void(int32_t, const uint8_t*, size_t)`. It retains the same 16,384 callback slots, borrowed payload lifetime, and absence of retry, buffering, and timeout support described in the main book.

### Client pool and generated proxies

`ClientPool::new_(optional_poll, config)` returns a value. Both arguments are required; use `.` for its methods. The pool shuts down even a caller-supplied worker when destroyed.

The following is a contextual fragment for a translation unit that already includes its generated `demo.h` and owns `poll`:

```cpp srpc-no-compile
// Put this import at file scope, after textual includes.
import srpc.load_balancer;

// In a function:
auto config = srpc::PoolConfig::defaults();
config.load_balancing = srpc::LoadBalancingStrategy::ROUND_ROBIN;
auto pool = srpc::ClientPool::new_(rusty::Some(poll.clone()), config);
auto selected = pool.get_client("127.0.0.1:8848");
if (selected.is_some()) {
    auto client = selected.unwrap();
    demo::DemoProxy proxy(const_cast<srpc::Client*>(client.get()));
    // Keep client owned throughout every use of this borrowing proxy.
}
```

The proxy borrows the raw client pointer. It does not extend the client's lifetime. Selection and health semantics, including the missing automatic latency samples, are covered in the main book.

---

## 8. C++ server bindings

### Generated services and manual services

The C++ IDL generator emits a service class with virtual application handlers, a non-const registration method, and a const dispatch method. It does not derive that generated class from `srpc::Service`. Register its owned implementation with `reg_service_typed`; the C++ adapter supplies the erased service boundary.

Handler overrides must retain the generated `const` qualification. The method modes and full generated example are in the IDL chapter. Native Rust instead implements the actual `Service: Send + Sync` trait.

A hand-written C++ service can inherit `srpc::Service`. Its interface is:

```cpp srpc-no-compile
#include "srpc.hpp"

struct ManualService : srpc::Service {
    int32_t __reg_to__(srpc::Server& server, size_t index) override;
    void __dispatch__(
        int32_t rpc_id,
        rusty::Box<srpc::Request> request,
        srpc::WeakServerConnection connection) const override;
};
// Supply both definitions, including argument decoding and reply writing.
```

The service context dispatches through shared const service access. It no longer holds a mutable `RefCell` borrow across the handler. That removes the former false requirement to split a service merely because one handler can suspend. Application locks and local fiber ownership still need correct use.

### Registration collisions

The generator chooses method IDs in `0x10000000` through `0x70000000` and reuses IDs from the previous generated header. Preserve that header when stable IDs matter.

Its generated registration method rolls back all its IDs after a registration error. `unreg` does not check which service owns an ID, so a collision can remove the earlier service's winning mapping too. The server registration wrapper discards the returned error and still stores the service. A method unexpectedly returning `ENOENT` can therefore be a registration collision. This rollback behavior belongs to the generated C++ wrapper; native implementations control their own registration logic.

### Lifecycle and replies

Construct a server with `Server::new_(rusty::Some(poll.clone()))`. Register services before the single `start` call. The address cast is the same as for a client. A failed start consumes its freshly assembled service context, so reconstruct or re-register before retrying.

Pass an explicit millisecond argument to `drain` and `graceful_shutdown`. `kDefaultDrainTimeoutMs` is 30,000; the C++ methods do not supply a default argument.

`ServerReplyFn` lowers to `rusty::Function<void(BinaryWriteArchive&)>`. A populated callable writes a body; `srpc::ServerReplyFn{}` makes a header-only reply. For example, this function reports an error when the weak connection is still upgradeable:

```cpp srpc-no-compile
#include "srpc.hpp"

void reply_error(
    const srpc::Request& request,
    const srpc::WeakServerConnection& weak,
    int32_t error) {
    auto connection = weak.upgrade();
    if (connection.is_some()) {
        connection.unwrap()->reply(request, error, srpc::ServerReplyFn{});
    }
}
```

Generated deferred handlers own their request through `DeferredReply`. A send happens at most once; cleanup still runs when the owner is destroyed without a reply. Sending does not drop that owner or its pending-request guard. Keep completed deferred owners short-lived so drain can finish.

C++ callable erasure does not enforce Rust's `Send` constraints. Do not use that difference to move a fiber-local deferred reply across execution threads. `run_async` on the reply and connection executes inline; it does not provide a worker pool.

---

## 9. C++ serialization

The native chapter covers the wire format, descriptor behavior, length limits, and registry semantics. C++ adds open-ended ADL dispatch and standard-library container adapters.

### Archives and primitive dispatch

`srpc.hpp` includes the serialization header and imports its module. A borrowed memory archive uses the named proxy constructors:

```cpp srpc-no-compile
#include "srpc.hpp"

void round_trip() {
    srpc::BufferSink sink{};
    {
        srpc::BinaryWriteArchive archive(srpc::make_sink_proxy_buffer(&sink));
        srpc::Serialize_::serialize(int32_t{42}, archive);
    }
    srpc::BufferSource source =
        srpc::BufferSource::new_(sink.bytes.data(), sink.bytes.size());
    int32_t value = 0;
    {
        srpc::BinaryReadArchive archive(srpc::make_source_proxy_buffer(&source));
        srpc::Deserialize_::deserialize(value, archive);
    }
}
```

The proxies borrow. Keep their targets unmoved and live, and leave the source's backing bytes unchanged until the archive dies. `FdSink::new_(fd)` and `FdSource::new_(fd)` also borrow their descriptor. They perform unbuffered leaf I/O.

Use `srpc::Serialize_::serialize(value, archive)` and `srpc::Deserialize_::deserialize(value, archive)` for primitive and container values. Generated struct stream operators do not provide a generic `archive << primitive` interface.

### C++ container support

| C++ type | Encoding |
|---|---|
| Fixed-width signed and unsigned integers | Raw native-endian bytes |
| `double` | Eight host-representation bytes |
| `std::string`, `rusty::String` | `v64` byte length, then bytes |
| `std::string_view` | Same encoding, write side only |
| `std::pair<A, B>` | First field followed by second |
| `std::vector`, `std::list`, `std::set`, `std::unordered_set` | Count followed by values |
| `std::map`, `std::unordered_map` | Count followed by key/value pairs |
| `rusty::Vec`, `rusty::BTreeSet`, `rusty::BTreeMap` | Matching canonical collection encoding |

C++ decoders default-construct collection elements and clear the destination. The STL map adapters use first-insertion behavior for duplicate keys; native Rust map decoders retain the last duplicate. Do not depend on a duplicate-key payload producing the same result in both container families.

There is no decoder for `std::string_view`, and no built-in primitive serialization for `float`, `bool`, or pointers. Unordered iteration is not a stable encoded ordering.

Generated `rusty::HashSet` and `rusty::HashMap` decoders are insert-only and have coverage. Their encoders remain a C++ toolchain limitation: enumerating hashbrown through the runtime iterator dispatcher triggers clang-22's Itanium mangler failure. Avoid instantiating those encoders; choose ordered runtime containers or STL containers for that C++ use. Native Rust `HashMap` and `HashSet` encoding is supported and tested.

### Your own C++ types

Put a free `serialize` and `deserialize` pair in the type's namespace:

```cpp srpc-no-compile
#include "srpc.hpp"

namespace geometry {
struct Point3 {
    double x{};
    double y{};
    double z{};
};

inline void serialize(const Point3& value, srpc::BinaryWriteArchive& archive) {
    srpc::Serialize_::serialize(value.x, archive);
    srpc::Serialize_::serialize(value.y, archive);
    srpc::Serialize_::serialize(value.z, archive);
}

inline void deserialize(Point3& value, srpc::BinaryReadArchive& archive) {
    srpc::Deserialize_::deserialize(value.x, archive);
    srpc::Deserialize_::deserialize(value.y, archive);
    srpc::Deserialize_::deserialize(value.z, archive);
}
}
```

The dispatcher uses a dependent unqualified call and an ordinary-lookup poison declaration. Argument-dependent lookup must find the overload in the value's namespace or the archive's namespace. Hidden friends also work, and are how the generator handles request/response structs nested in a service class. Missing overloads fail at instantiation.

The generator emits these free functions and small stream-operator forwarders for IDL structs. For a custom type in an IDL header section, declare both the type and its functions together. That section is emitted at global scope, outside the IDL's namespace. A global overload for a type declared only inside `namespace demo` will not become visible through ADL.

### Dynamic payloads

A C++ payload supplies `save`, `load`, and `kind` members and is default-constructible. The erased holder records its type for checked recovery:

```cpp srpc-no-compile
#include <string>
#include "srpc.hpp"

struct GraphPayload {
    static constexpr int32_t kKind = 60;
    int32_t node_count{0};
    std::string label;

    void save(srpc::BinaryWriteArchive& archive) const {
        srpc::Serialize_::serialize(node_count, archive);
        srpc::Serialize_::serialize(label, archive);
    }
    void load(srpc::BinaryReadArchive& archive) {
        srpc::Deserialize_::deserialize(node_count, archive);
        srpc::Deserialize_::deserialize(label, archive);
    }
    int32_t kind() const { return kKind; }
};

static int registered_graph =
    srpc::SerializableRegistry::reg<GraphPayload>(GraphPayload::kKind);
```

Registration takes an explicit kind and returns zero, which permits static-initialization registration. Missing kinds panic on creation. The helper base `Serializable<KIND>` supplies `kind()` and `static_kind()`; its kind must be nonzero. Application payloads need not inherit that base.

Import `srpc.any_message` or `srpc.serializable_envelope` explicitly; neither is in the umbrella. Those C++ types have ADL serialization helpers, unlike their native Rust types, which require explicit `save` and `load` calls.

This contextual fragment assumes `GraphPayload` above:

```cpp srpc-no-compile
// File scope, after textual includes:
import srpc.any_message;
static int registered_graph_name =
    srpc::reg_any_message_as<GraphPayload>("demo.GraphPayload");

// In a function:
GraphPayload payload;
payload.node_count = 42;
auto message = srpc::AnyMessage::pack<GraphPayload>(
    rusty::Arc<GraphPayload>::make(payload));
auto recovered = message.unpack<GraphPayload>();
if (recovered.is_some()) {
    auto shared = std::move(recovered).unwrap();
}
```

Name aliases and unchecked `pack_as` have the same type-checking caveats as the native implementation. Check the unpacking result. The main book also describes why registry factories must return fresh ownership for loading.

For a typed numeric envelope, specialize the C++ membership marker in `namespace srpc`:

```cpp srpc-no-compile
// Context: GraphPayload is defined above. Import at file scope.
import srpc.serializable_envelope;

struct MyCommands {};
namespace srpc {
template <>
struct PayloadMember<MyCommands, GraphPayload> {
    static constexpr bool value = true;
    static constexpr int32_t KIND = GraphPayload::kKind;
};
}
using MyEnvelope = srpc::SerializableEnvelope<MyCommands>;
```

`pack<T>(value)` copies, `pack_aliased<T>(arc)` shares, `unpack<T>()` returns a nullable raw pointer, and `unpack_shared<T>()` returns an optional owning handle. The compile-time marker controls typed operations; loading still consults the global numeric registry without validating set membership. The encoded kind comes from the payload, not the marker constant. Keep all three kind declarations consistent.

---

## 10. C++ reliability configuration

The reliability behavior and native implementation status are documented in the main book. These additional modules must be imported when their types are named:

```cpp srpc-no-compile
#include "srpc.hpp"
import srpc.reconnect_policy;
import srpc.circuit_breaker;
import srpc.heartbeat;
import srpc.connection_metrics;
import srpc.load_balancer;
import srpc.request_options;
```

`srpc.connection_state`, `srpc.errors`, and `srpc.request_queue` are already in the umbrella. `BufferingConfig`, `DisconnectBehavior`, `KeepaliveConfig`, and `PoolConfig` belong to `srpc.client`.

Rust constructors named `new` are spelled `new_` in C++. Setters take the config by const reference without Rust's explicit borrow syntax. `ReconnectPolicy::new_()` and `conservative()` enable reconnect; the client's staged heartbeat and breaker remain disabled. Keepalive's default is enabled and now applies the TCP socket options.

Lifecycle callbacks accept C++ callables directly. The error callback takes `RpcError` and `std::string_view`, and the restart callback takes the old and new `uint64_t` IDs. Preserve the same short callback duration and captured-owner lifetimes as in native Rust. `clear_connection_callbacks()` waits for existing dispatches and must not be called from a callback.

Set buffering and restart callbacks after a successful connect. Configure keepalive, heartbeat, breaker, and reconnect before or after connect; those four are staged for replacements.

Importing the heartbeat module does not provide an automatic probe timer. Importing metrics does not populate latency samples. Neither behavior differs merely because the caller is C++.

---

### Checked request and reliability examples

These function-body examples use the imports below. In examples with a `client` variable, it is a non-null `const srpc::Client*` whose owning `rusty::Arc<srpc::Client>` remains alive throughout the call. The request example also assumes an application RPC ID named `RPC_METHOD_ID` and an `int32_t arg1` argument.

```cpp srpc-no-compile
#include "srpc.hpp"
import srpc.circuit_breaker;
import srpc.connection_metrics;
import srpc.heartbeat;
import srpc.reconnect_policy;
import srpc.request_options;
```

#### Retry options and the caller's wait

This fragment calls an already connected client. The operation must be safe to repeat before setting `idempotent`. The caller's wait budget covers the attempt chain; the coordinator retains its own copy of the attempt options.

```cpp srpc-compile-client
auto options = srpc::RequestOptions::defaults();
options.timeout_ms = 500;
options.max_retries = 3;
options.idempotent = true;

auto submitted = client->request_with_options(RPC_METHOD_ID, options,
    [arg1](srpc::BinaryWriteArchive& archive) {
        srpc::Serialize_::serialize(arg1, archive);
    });
if (submitted.is_ok()) {
    auto future = submitted.unwrap();
    auto caller_wait = options;
    caller_wait.timeout_ms = 5000;
    future->set_options(caller_wait);
    if (future->wait_with_options()) {
        auto retries = future->get_retry_count();
        auto error = future->get_error_code();
        (void)retries;
        (void)error;
    }
}
```

#### Staged and live connection settings

The first settings are retained for the next connect. Buffering needs a live connection, so apply it only after the dial succeeds. Heartbeat configuration still has no automatic probe timer.

```cpp srpc-compile-client
client->set_reconnect_policy(srpc::ReconnectPolicy::conservative());
client->set_keepalive(srpc::KeepaliveConfig::aggressive());
client->set_heartbeat(srpc::HeartbeatConfig::defaults());
client->set_circuit_breaker(srpc::CircuitBreakerConfig::defaults());

int32_t error = client->connect(
    reinterpret_cast<const int8_t*>("127.0.0.1:8080"), true);
if (error == 0) {
    client->set_buffering_config(srpc::BufferingConfig::defaults());
}
```

#### Reconnect policy

A zero retry limit means unlimited reconnect attempts. Jitter applies after the delay cap, so a sampled delay can exceed that cap.

```cpp srpc-compile
auto policy = srpc::ReconnectPolicy::new_();
policy.auto_reconnect = true;
policy.max_retries = 10;
policy.initial_delay_ms = 100;
policy.max_delay_ms = 30000;
policy.backoff_multiplier = 2.0;
policy.jitter_enabled = true;
```

#### Circuit-breaker thresholds

This configuration opens after five consecutive failures. In half-open state it needs two successful probes to close.

```cpp srpc-compile
auto breaker = srpc::CircuitBreakerConfig::defaults();
breaker.failure_threshold = 5;
breaker.success_threshold = 2;
breaker.timeout_ms = 5000;
```

#### Heartbeat settings

These fields configure the heartbeat state machine. They do not supply the missing client-side scheduling call.

```cpp srpc-compile
auto heartbeat = srpc::HeartbeatConfig::defaults();
heartbeat.interval_ms = 5000;
heartbeat.timeout_ms = 2000;
heartbeat.max_missed = 2;
```

#### Reading connection metrics

The optional connection handle keeps the connection alive while its metrics reference is used. These atomic reads are not a transactional snapshot.

```cpp srpc-compile-client
auto connection = client->connection();
if (connection.is_some()) {
    auto owned = connection.unwrap();
    const srpc::ConnectionMetrics& metrics = owned->metrics();
    auto sent = metrics.requests_sent();
    auto completed = metrics.requests_completed();
    auto failed = metrics.requests_failed();
    auto in_flight = metrics.in_flight_requests();
    auto bytes_out = metrics.bytes_sent();
    auto bytes_in = metrics.bytes_received();
    auto reconnects = metrics.reconnect_count();
    auto success_percent = metrics.success_rate_percent();
    auto queue_drops = metrics.queue_dropped_requests();
    auto circuit_rejections = metrics.circuit_open_rejections();
    auto circuit_opens = metrics.circuit_open_transitions();
    (void)sent;
    (void)completed;
    (void)failed;
    (void)in_flight;
    (void)bytes_out;
    (void)bytes_in;
    (void)reconnects;
    (void)success_percent;
    (void)queue_drops;
    (void)circuit_rejections;
    (void)circuit_opens;
}
```

#### Registering lifecycle callbacks

Callbacks accumulate. Their captures must remain valid until registration is cleared and every in-flight invocation finishes. A user close can still invoke `on_disconnected`; it does not imply an unexpected failure.

```cpp srpc-compile-client
client->add_on_connected([]() {
    // Connect succeeded.
});
client->add_on_disconnected([]() {
    // Inspect the application's shutdown state before starting failover.
});
client->add_on_error([](srpc::RpcError error, std::string_view message) {
    // Record the classification and copy message if it must be retained.
    (void)error;
    (void)message;
});
client->add_on_reconnecting([]() {
    // The reconnect loop is starting.
});
client->add_on_reconnected([](bool success) {
    // False covers exhausted or cancelled attempts.
    (void)success;
});
```

Do not call `clear_connection_callbacks()` from these callbacks; it waits for existing invocations to finish.

---

## 11. Threading and synchronization in C++

The runtime rules are the same as in the Rust book. Each `PollThread` has one
OS worker. Fibers and stackless tasks run cooperatively on that worker.
Ordinary blocking I/O, thread sleep and RPC future waits still stop the
worker. Changing a service method from `fast` to ordinary dispatch enables
SRPC fiber suspension; it does not make arbitrary blocking operations yield.

### Generated ownership types

| C++ type | Use |
| --- | --- |
| `rusty::Arc<T>` | Shared lifetime. The payload still needs a thread-safe access contract. |
| `rusty::Rc<T>` | Shared lifetime within one thread, including reactor and fiber handles. |
| `rusty::Weak` | Non-owning references. Check `upgrade()` before using the payload. |
| `rusty::Box<T>` | Unique ownership, including registered services and request bodies. |

`Client::create` returning an `Arc` does not authorize concurrent access to
one client handle. Its canonical Rust `Client` is not `Sync`. Keep one client
handle per application thread. Clients may share a poll worker or use separate
workers; the choice determines scheduling and CPU use.

Services have const-callable dispatch and must synchronize mutable shared
state. The shared context owns boxed services and immutable routing tables.
It does not hold an exclusive service borrow across a suspended handler.

`Reactor::get_reactor()` and `get_disk_reactor()` use real thread-local
storage. The second accessor returns a separate reactor on the same thread.
Do not move their `Rc` handles, fibers or events across threads. C++ runtime
tests reset thread-local state between independent test cases with the
following test-only operations:

```cpp srpc-no-compile
srpc::sp_running_fiber_th_.with([](auto& slot) { *slot.borrow_mut() = rusty::None; });
srpc::sp_reactor_th_.with([](auto& slot) { *slot.borrow_mut() = rusty::None; });
```

Application code should not clear a live reactor's thread-local slots.
Cross-thread stackless wakeups submit a synchronized wake ticket; completion
still runs on the owning reactor.

### Shutdown and native synchronization

`do_shutdown()` and `wait_for_shutdown()` share a mutex and condition
variable. C++ callers can coordinate this narrow handshake if they keep the
server alive throughout both operations:

```cpp srpc-no-compile
// Control thread, while the owning thread keeps svr alive:
svr.do_shutdown();

// Owning thread:
svr.wait_for_shutdown();
```

This does not make other `Server` methods thread-safe. Run `stop_accepting`,
`drain` and `graceful_shutdown` on the owning thread. Do not call any of these
mutex-taking operations from a signal handler. Signal the owner through an
appropriate external mechanism.

For application synchronization, use `std::atomic`, `std::mutex`, standard
lock guards and condition variables. SRPC's `SpinLock` has only `lock()` and
`unlock()`, with no RAII guard. Its contended path sleeps the OS thread for
50 microseconds per attempt. Never hold it across a fiber suspension.

The checked pthread wrappers operate on storage owned by the caller:

```cpp srpc-no-compile
pthread_mutex_t m;
pthread_cond_t c;
Pthread_mutex_init(&m, nullptr);
Pthread_cond_init(&c, nullptr);

Pthread_mutex_lock(&m);
while (!ready) {
    Pthread_cond_wait(&c, &m);
}
Pthread_mutex_unlock(&m);
```

Pair initialization and destruction. The wrappers cover mutex, condition
variable and pthread spin-lock operations and verify the native return code.
They do not return an error for the caller to handle. `cpu_pause()` is also
exported for native spin loops. These wrappers are not a guard-returning
`SpinMutex<T>` API.

---

## 12. C++ performance and historical measurements

### Run the checked-in throughput driver

The `rpcbench` target builds `tests/rpcbench.cc` and
`tests/benchmark_service.cc`. It is excluded from the default build and is
not registered with CTest. After configuring the C++ build:

```sh
cmake --build build --parallel 4 --target rpcbench
scripts/run_rpcbench.sh build/rpcbench before-my-change
```

CMake supplies the module map and `build/bench-include/srpc` include symlink.
Keep the generated benchmark header when regenerating its IDL; the generator
recovers method IDs from that header.

The script runs three trials per default mode, starting a new server for
each trial. Default modes are `fast`, `fiber`, `defer` and `async`.
`fast_vec` requires vector payload mode and is excluded by default because
it measures a different workload. `RPCBENCH_MODES`, `RPCBENCH_TRIALS`,
`RPCBENCH_N`, `RPCBENCH_B` and `RPCBENCH_PORT` control the script; inspect
[run_rpcbench.sh](../scripts/run_rpcbench.sh) for the full configuration.
Read the trial spread before interpreting a difference.

The executable uses `-s <addr>` to serve and `-c <addr>` to drive load.
`-m` selects the method mode, `-n` the duration in seconds, `-t` client
threads, `-o` outstanding requests and `-b` payload bytes. Its historical
`-e` and `-w` flags appear in the recorded commands below; they do not create
a configurable worker pool inside the current `PollThread` type.

The Rust book describes the separate checked-in `bench/` leaf-codec
benchmark. It does not reproduce the external Rust TCP driver used for the
tables below. The `run_microbench.sh --compare` copy behavior can leave an
older `bench/` active when that directory already exists at a compared ref;
check which benchmark source each build uses.

### Historical C++ throughput, 2026-08-29

These results were recorded at commit `24e9246` on an AMD EPYC 7702P with
64 cores and 128 threads, Linux 6.8.0, Clang 22.1.8, `-O2 -march=native`
and load average about 1.3. Both endpoints ran on the same host. The workload
was a `nop` RPC with a 10-byte payload, ten-second trials, and three trials
per mode. Values are the mean of client-reported `avg qps`.

| Mode | Mean qps | Spread |
| --- | ---: | --- |
| `fast` | 1,188,955 | ±1.9% |
| `async` | 995,920 | ±4.8% |
| `defer` | 753,513 | ±0.7% |
| `fiber` | 737,705 | ±3.6% |

Recorded commands:

```sh
rpcbench -s 127.0.0.1:18848 -m <mode> -e 2 -w 16
rpcbench -c 127.0.0.1:18848 -m <mode> -n 10 -b 10 -e 2 -o 1000 -w 16 -t 8
```

For this empty-handler workload, inline dispatch avoided the fiber cost;
deferred and ordinary handlers both used a stackful dispatch. These numbers
predate later runtime and ownership repairs and have not been rerun for the
current implementation.

### Historical Rust and cross-language results, 2026-08-31

The original book recorded the following results on the same EPYC host with
rustc 1.97.1, `-C opt-level=3 -C target-cpu=native` and thin LTO. The Rust
driver was named `rust-inmemory-bench` and lived outside the repository.
The tables preserve the recorded observations; the missing driver prevents
reproducing their exact setup from this checkout alone.

An `i64` echo used the real request and reply encoding and three trials:

| Metric | Mean | Spread |
| --- | ---: | --- |
| TCP loopback, one driver thread, 1,000 outstanding | 286,935 op/s | ±3.0% |
| TCP loopback, 100 outstanding | 233,242 op/s | ±1.8% |
| TCP loopback, one outstanding | 1.12 ms/op | ±1.2% |
| Synchronous in-memory round trip | 818,500 op/s | ±0.6% |
| Wire serialization in both directions | 62 ns/op | ±2.2% |

A C++ comparison with one client thread, `-m fast -o 1000 -t 1`, recorded
344,231 qps ±0.7%. The recorded Rust value was about 83% of it. This was a
different client topology from the eight-thread C++ table above.

The cross-language matrix at commit `aa73202`, load average 1.4 to 2.2,
used `fast_nop`, id `0x4b921bd9`, with a 10-byte string argument, an empty
reply and 1,000 outstanding requests per client thread. Three trials were
recorded per cell. The C++ client executable was reused against both servers.

| Aggregate op/s | C++ server, `-e 2 -w 16` | Rust server, one poll thread |
| --- | ---: | ---: |
| C++ client, one thread | 371,760 | 478,764 |
| C++ client, eight threads | 1,233,742 | 2,223,421 |
| Rust client, one thread | 252,126 | 329,601 |
| Rust client, eight threads | 1,111,099 | 1,952,013 |

The recorded eight-thread server swap gave the Rust server about 180% of
the C++ server's throughput. The Rust client used `Arc<Future>` while
rpcbench used `request_async`, so the client rows did not exercise identical
client bookkeeping. The pure-Rust cell was about 158% of the pure-C++ cell.

The accompanying historical profile attributed costs to the generated C++
runtime's erased hash comparison, mutex calls, reference-count helpers and
allocator use. It recorded both servers spending 20% to 30% of samples in
allocation and vector growth. A 64-byte initial serialization-sink capacity
was reported to improve the C++ eight-thread server by 6.5% and Rust cells
by 4% to 8%. These are observations from that revision and profiling session,
not measured explanations for current performance. The sequential TCP result
was attributed to the worker's roughly one-millisecond polling cadence.

A further mode comparison used eight client threads and 1,000 outstanding
requests per thread:

| Mode | C++ server op/s | Rust server op/s | Recorded ratio |
| --- | ---: | ---: | ---: |
| `fast` | 1,410,825 | 2,381,806 | 169% |
| `fiber` | 816,758 | 1,123,166 | 138% |
| `defer` | 806,244 | 1,062,478 | 132% |
| `async`, unequal dispatch | 1,076,875 | 2,331,930 | 217% |

The async row is not a valid like-for-like comparison. Its Rust server
executed the handler inline without running the task state machine. The
recorded correction with a real Rust task was 1,197,856 op/s against the
same C++ value of 1,076,875, about 111%.

The old narrative also recorded about 72,000 fiber calls/s without fiber
reuse and a later two-server smoke run at 831,000 plus 847,000 qps. The
external driver's source and complete run records are not in this checkout,
so these values remain historical context. The older process-global TLS
limitation described in that narrative no longer applies: the current
reactor uses real `thread_local!`, and maintained Rust tests run independent
reactors on different threads.

### Build settings for C++ measurements

Current CMake `SRPC_CXXFLAGS` inherit `BENCH_CXXFLAGS`:

```text
-w -Wreturn-type -MD -MP -DRUSTYCPP_DISABLE_ARC_LOG -DREUSE_FIBER
-O2 -g -fno-omit-frame-pointer -march=native
```

The library keeps debug information and frame pointers for profiling. The
flags are exported `PUBLIC`, so `-w` also affects consumers. Vendored runtime
ports use their own optimization settings. Record both when reporting
measurements; changing a CMake build type alone does not remove explicitly
supplied target flags.

`-march=native` must agree between module providers and importers. A BMI
compiled for one target-feature set can be rejected by an importer using
another. Rebuild on a different CPU instead of moving an existing build tree.

The main Rust book describes the current request costs, fiber reuse,
poll-thread model, wait budgets, frame bounds and reliability settings.
Those mechanisms apply to generated C++ too. Benchmark hypotheses include
using fast registration for small nonblocking handlers, choosing callback
requests where their limitations are acceptable, avoiding a detached retry
thread for every hot-path call, and distributing work across poll threads.
Measure the affected dispatch mode and workload after each change.

---

## 13. C++ pitfalls and best practices

### Generated proxies use blocking RPC futures

A typed proxy's synchronous result eventually calls the RPC future's
blocking getters. Each default blocking wait has a one-second budget,
starting when the wait begins. It is not a deadline fixed at request
creation. The wait blocks the OS thread, including when invoked inside an
SRPC fiber.

```cpp srpc-no-compile
auto result = demo.sum(req);
if (result.is_err() && result.unwrap_err() == 110) {
    // The wait expired; the server may still execute or reply.
}
```

An ordinary future can use `set_options` and `wait_with_options` for a longer
wait without enabling retries. If using `request_with_options`, remember
that the returned coordinator's initial wait budget is zero, which falls
back to the one-second wait. Set the caller's budget too:

```cpp srpc-no-compile
auto opts = RequestOptions::defaults();
opts.timeout_ms = 500;

auto fu = cl->request_with_options(DemoService::SUM, opts,
    [&](BinaryWriteArchive& m) { srpc::Serialize_::serialize(a, m); });

auto f = fu.unwrap();
auto wait_opts = opts;
wait_opts.timeout_ms = 5000;
f->set_options(wait_opts);
f->wait_with_options();
```

A local timeout does not cancel the server operation. The callback method
`request_async` has no timeout and a fixed slot table whose reply lookup
does not check the full xid. These limitations are described in the Rust
book's client and pitfalls chapters.

### Dispatch mode does not change blocking system calls

The IDL keywords `fast` and `prefix` register an inline handler. `async`
handlers also use fast registration and run a task on the poll worker.
Every coroutine resume must avoid ordinary blocking work. Default methods
use stackful fibers; SRPC event waits and fiber sleeps can suspend them,
but mutex contention, blocking I/O and synchronous RPC waits still block
the OS worker. Offload blocking work and arrange completion explicitly.

### Apply configuration to the intended connection

Stage keepalive, heartbeat, circuit-breaker and reconnect settings before
`connect`, and install a custom channel factory before connecting. Buffering
is the exception because it has no staged setting:

```cpp srpc-no-compile
auto cl = Client::create(poll);
cl->set_reconnect_policy(ReconnectPolicy::conservative());
cl->set_heartbeat(HeartbeatConfig::defaults());
cl->set_circuit_breaker(CircuitBreakerConfig::defaults());
cl->connect(reinterpret_cast<const int8_t*>(addr), true);
cl->set_buffering_config(BufferingConfig::defaults());
```

The last call must occur after a successful connect. Applying it before
connect silently leaves the default queue enabled.

Retries require an idempotency assertion:

```cpp srpc-no-compile
auto opts = RequestOptions::defaults();
opts.max_retries = 3;
opts.idempotent = true;
```

The coordinator disables retries when `idempotent` is false. It replays the
same argument bytes, so the server may process a request more than once.
Application error codes matching transport errno values can also affect
the circuit breaker.

### Subclass an abstract generated service

For an `abstract` service, or an IDL method with `= 0`, the generator emits
pure virtual methods. Without that declaration it emits virtual declarations
whose definitions must exist elsewhere. Merely overriding them in a subclass
does not supply the missing generated class definitions and can cause
undefined vtable/key-function references at link time.

Either mark the service abstract or define the generated methods out of
line, as `tests/benchmark_service.cc` does. Match the current const-qualified
signatures. Register implementations with
`reg_service_typed(rusty::make_box<T>())` before starting the server.

### Event and lifetime rules still apply

Use one fresh ordinary event per wait cycle, on its owning reactor thread.
Never hold a borrow guard or a lock across a fiber suspension. Use
`this_fiber::sleep_*` to suspend a fiber; `std::this_thread::sleep_for` and
`Time::sleep` block the OS thread. A zero fiber sleep returns immediately.
The event factories are free functions; quorum types and their factory live
in global `::janus` in generated C++, while other event factories live in
`srpc`.

Keep borrowed reply storage and request bodies alive for their decoders.
Copy a callback's raw payload before retaining it. Inspect `Option` results
and weak upgrades rather than treating generated shared handles as nullable
raw pointers.

---

## 14. Troubleshooting generated C++

The Rust book's error-code, framing, connection and shutdown diagnostics
describe the common runtime. Start there for protocol failures. This chapter
covers C++ build and consumer details.

### Logging from C++

The logger defaults to DEBUG and writes synchronously to stdout. Configure
`Log::set_level` in code; there is no log-level environment setting.
`<unknown>:0` means the caller omitted a filename and line number.

The variadic `Log_debug`, `Log_info`, `Log_warn`, `Log_error` and `Log_fatal`
macros/wrappers are consumer-side helpers in `tests/srpc_log.h`. They are
not exported SRPC module functions. Use that helper pattern or call
`log_line` with a preformatted string. Generated C++ assertion locations
can name a generated module; the client's verifier uses the fixed canonical
path `rpc/client.rs` and line zero.

### Service and shutdown failures

For an unknown RPC id, compare the generated client and server headers.
Regenerate over the existing header to preserve IDs. Deleting the old header
before generation lets the random-ID generator assign new wire IDs.
Register services before `start`, which publishes their routing context.

A stalled poll worker affects all of its connections. Inspect inline
handlers, coroutine resumes, callbacks and fiber code for blocking calls.
An ordinary fiber handler can still block the worker on a synchronous RPC
wait. Longer timeouts do not repair that scheduling dependency.

Shutdown hooks run under the hook-list mutex, so registering another hook
inside a hook deadlocks. Run lifecycle operations on the owning thread,
outside a request handler and outside a signal handler. Close clients,
destroy the server and then shut down the poll thread. A shutdown initiated
on the poll worker skips the join, so the worker may outlive that call.

### CMake and CTest commands

Use the configured inventory to choose a suite:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build -N -L srpc
ctest --test-dir build -L srpc --output-on-failure
ctest --test-dir build -R '^test_fiber$' --output-on-failure
./build/test_fiber --gtest_filter='FiberTest.SleepUsZero'
```

Some runtime targets are plain programs and do not accept GoogleTest
filters. `CMakeLists.txt` explicitly lists built test sources; a historical
file in `tests/` does not establish that a target exists or uses the current
API. The [coverage inventory](test-coverage.md) checks every C++ test source's
disposition and registration. Missing GoogleTest fails configuration unless
`BUILD_TESTING=OFF` explicitly disables test acceptance. A bare
`ctest` can also report unbuilt vendored executables as `Not Run`.

Sanitizers use a separate build tree:

```sh
cmake -S . -B build-asan -G Ninja -DSRPC_SANITIZER=address
scripts/run_sanitizer_battery.sh address
```

Inspect the script for the separate directories and exact targets it uses.
The library's ordinary target flags include debug information and frame
pointers, so tools such as `gdb ./build/test_fiber` can inspect frames
without removing the release configuration. A BMI target-feature mismatch
requires consistent module/importer flags and a fresh compatible build tree.

For a canonical source change, Cargo tests alone do not establish C++ ABI
or runtime correctness. The source gate, generated-provider/archive checks
and runtime parity serve different purposes and all need their relevant
checks.

---
