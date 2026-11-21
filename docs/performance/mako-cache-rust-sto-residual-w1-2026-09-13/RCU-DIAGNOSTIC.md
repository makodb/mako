# Whole-loop native RCU attribution diagnostic

This is a private Stage3 source variant, not a production change or batching
API. It measures an upper bound on amortizing native RCU entry and exit.

Source: `/var/tmp/mako-residual-gap.IvBRsm/rcu-source`.
Base: `/var/tmp/mako-w1-implementation.VtilJG/stage3`.
Patch: `rcu-diagnostic.patch` in this directory.

The patch changes five files. It adds a hidden `LocalDb::diagnostic_rcu_scope`
helper, reexports the existing Masstree guard, forwards the helper from Cache,
and wraps the engine_compare measured loop in the guard for Rust STO builds.
C ABI builds retain their original scopes. The standard result JSON is
unchanged; stderr labels the diagnostic scope policy explicitly.

Each foreground worker completes warmup and the warmup barrier first. It
then opens one scope before allocation counting and PMU start. The loop and
transaction operations are unchanged. It stops timing, PMU and allocation
counting, explicitly closes the scope, then enters the completion barrier and
checks every final value. Engine, engine-logged and cache-ack modes use this
placement. Scope errors reject the sample.

## Ownership and admission

The guard borrows the database and is neither Send nor Sync. Its native Worker
is independently retained in the existing thread-local attachment. No mutable
reference to the core WorkerContext is added, and no guard is retained in a
worker slot or across process lifetime. Native attach and scope errors retain
their original types through the diagnostic-only boxed error result.

The existing native controlled-RCU path still checks runtime health and still
takes per-operation structural admission. Core OCC, limits, canonical records,
clock allocation, commit phases and cleanup are unchanged. Nested scope or
read-scope creation and quiescence remain rejected by the native boundary.
The final explicit close reports failure rather than hiding it in Drop.
On an operation error, that operation's transaction drops before the outer
guard. The guard's existing Drop provides cleanup on an ordinary error or
Rust unwinding. A panic-abort build still ends the process on panic.

The scope delays native reclamation throughout the loop. It therefore does
not establish a safe production scope across idle work or blocking calls.
The benchmark's PMU control calls occur while the scope is held, outside the
measured loop, and no peer barrier or verification occurs inside the scope.
A real implementation would need an explicit short synchronous batch boundary.

No native ABI or native archive changes are required. No per-operation edits
or added unsafe blocks occur in this patch. Root owns the pinned Rust 1.95
builds, native archive identity checks, smoke validation and timing. Patch
creation and read-only applicability checks alone do not count as test passes.
