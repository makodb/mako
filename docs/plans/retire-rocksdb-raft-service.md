# Retire the standalone RocksDB/Raft service

Use the distributed Mako runtime for one shard and multiple shards. A one-shard
deployment uses the same transaction API, OCC + 2PC implementation, storage
adapters and local/Raft logs. Optional Masstree remains an engine setting.

1. Delete the separate service crate, RPC/C client API, launchers, benchmark and
   build profile. Delete the benchmark-only native optimistic transaction wrapper.
2. Keep replication component qualification and run it with the main RocksDB
   runtime. Exercise the same transaction contract with one participant under
   local and replicated logging, with and without Masstree.
3. Update current documentation, CI selection and component accounting. Preserve
   historical reports as evidence of their original revisions.
4. Run component tests, both runtime profiles, native link audits and the upper
   protocol gate. Record the retained crash/failover coverage and results.

Status: complete. The service, C API, profile, launchers and benchmark-only
wrapper are removed. Replication CI now uses the main runtime and shared
components. Qualification passed: 128 native integration cases, 49 RocksDB-only
cases, 869 component test executions across configurations, strict linting,
production dependency audits and upper verification in 155.903 seconds on 32 CPUs.

The inventory now contains 27 Cargo packages and 7 support accounting units,
with 6,170 fewer nonblank source lines. See the
[qualification record](../testing/standalone-service-retirement/README.md).
