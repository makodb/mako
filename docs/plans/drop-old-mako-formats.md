# Retire old Mako storage formats

Status: complete. Removed 2,019 nonblank Rust lines net, including comments
and tests, with no new component or production dependency.

Passed 26 storage cases, 128 runtime/API cases in fresh processes, strict Clippy,
both CMake profile configurations, and a RocksDB-only compile. The engine-free
upper gate passed in 166.065 seconds on 32 CPUs; source correspondence now points
to the current native checkpoint path. Dependency, inventory and tooling tests
also passed. See the [qualification record](../testing/drop-old-mako-formats/README.md).

Compatibility with retired Mako formats is not required during development.
Keep native RocksDB adoption, current-format checkpoint recovery, and the same
files with Masstree enabled or disabled.

1. Remove the prefixed-key layout reader, legacy row mirror, file-only checkpoint
   reader, old row-command decoder and `mako_convert` program/build targets.
2. Require the current persistent application contract in the log runtime.
   Remove converter-only import entry points and the unused compatibility feature.
3. Move prepare/replay/tombstone tests onto the current protocol index and native
   storage contract. Replace conversion scenarios with reopen and engine-toggle
   scenarios. Keep explicit rejection of unsupported directories.
4. Point the upper source audit at native checkpoint publication and protocol
   metadata validation. Preserve all proof obligations and semantic controls.
5. Run component, runtime/API, strict lint, upper verification and boundary checks;
   update the book and component accounting.

Historical qualification records remain available. No user database is deleted
or automatically rewritten by this cleanup.
