# Raft Phase 8.4 Leaf 1: LogStorage Facade

## Scope selection

Phase 8.4 has three major implementation bullets. Implementing all of them in
one commit would be high-risk and hard to validate.

This leaf intentionally scopes to the first bullet only:

1. Add `src/deptran/raft/log_storage_facade.hpp`
2. Mirror every `LogStorage` method in a proxy facade
3. Add facade conformance tests

## Why this is commit-sized

- Single new header and one focused test target.
- No runtime behavior changes in `RaftServer` yet.
- Validation is straightforward: facade compile/behavior coverage plus full
  raft regression suite.

## Design rationale

- Follow the same proxy pattern already used in `transport.hpp` and
  `dispatcher.hpp`:
  - one `PRO_DEF_MEM_DISPATCH` tag per method
  - one `LogStorageFacade` that defines method conventions
  - one `LogStorageProxy` alias
- Mirror method signatures exactly (including constness and reference args) so
  later migration (`RaftServer::log_storage_` virtual pointer to proxy) can be
  mechanical and low-risk.

## Operator notes

- New focused test target:
  - `test_raft_log_storage_facade`
- Full raft regression gate:
  - `ctest --test-dir build --output-on-failure -R '^(test_raft_.*|raft_lab_standalone)$'`
