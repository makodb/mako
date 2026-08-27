# Reconciling the client.1 / server.1 DSL drift

`scripts/srpc_dsl_check.sh` reports two blocks failing their
`rust_sha256`: `client.1` (rpc/client.cpp:2669) and `server.1`
(rpc/server.cpp:1126).

## It is TRANSPILER drift, not source drift

The obvious reading — someone edited the Rust and forgot to regenerate,
so the DSL is an uncompiled draft — is **wrong**. Regenerating into a
scratch copy and diffing shows the committed C++ was produced by an
OLDER transpiler:

| difference | what it is |
|---|---|
| `+ static constexpr bool is_send / is_sync` | the Send/Sync derivation added in rusty-cpp `b48c4135` |
| `mark_forgotten_if_supported(...)` per field | a transpiler correctness improvement |
| `void pause()` → `void pause_()` | an API rename — see below |

client.cpp: 24 changed lines. server.cpp: 16.

So the DSL Rust is fine. What is stale is the generated C++, and
"reconciling" means regenerating `src/srpc` with the current transpiler.

## Which is blocked, because the rename would break deptran

`pause()` has real callers: `src/deptran/communicator.cc:456`,
`src/srpc/tests/rpc_chaos_test.cc:266` and `:330`, plus internal uses in
client.cpp. Regenerating renames the definition and breaks all of them.

## And the rename is a transpiler over-reach

`escape_cpp_keyword` (codegen/mod.rs ~54697) renames `dup`, `sleep`,
`raise`, `kill`, `pause`. The rationale is sound and documented: an
UNQUALIFIED free function loses overload resolution to libc's exact
match — the comment cites `::dup(5)` duplicating file descriptor 5.

But `ClientConnection::pause` is a MEMBER function. `obj.pause()`
resolves in class scope and can never select `::pause`, so there is no
hazard to escape. The same comment already notices the cost —
*"common ones (index/abs/close/div/exit) appear as variables and methods
throughout the ports and renaming them breaks pins"* — and handles it by
keeping the list short rather than by not escaping in member position.

**Fix: do not apply the libc-collision renames to member functions**
(or to any name emitted in member/qualified position). There is already
precedent for the distinction one function below —
`escape_cpp_keyword_in_runtime_path` skips exactly these renames because
a qualified path cannot select the libc symbol either.

## Order of work

1. Fix the escape rule in the transpiler so member functions keep their
   names. Verify `pause` survives, and that free-function `pause` (if
   any exists) still escapes.
2. Re-run the regen diff. It should reduce to the additive
   `is_send`/`is_sync` and `mark_forgotten` changes.
3. Regenerate `src/srpc`, confirm deptran and the chaos tests still
   build, and land it — the two blocks then verify clean.

Doing (3) before (1) means renaming a public method across deptran to
work around a transpiler bug, which is the pattern this migration is
explicitly correcting.
