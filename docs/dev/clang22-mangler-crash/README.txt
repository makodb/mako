Artifacts for the clang 22.1.7 Itanium-mangler frontend crash.
See ../clang22-mangler-crash.md for the full analysis.

  crash-backtrace.txt              - clang's stack dump (mangler SIGSEGV)
  attempted-repro-real-headers.cppm - `for_in` over BTreeMap using the real
                                      rusty headers; does NOT crash in
                                      isolation (the trigger needs
                                      shard_manager's fuller instantiation
                                      state). Kept as a starting point for a
                                      creduce-based standalone minimization.

Reproduce the real crash: check out shard_manager.h before the
apply_migration_delta workaround, then from build_clang22:
  ninja -t commands CMakeFiles/cluster.dir/src/cluster/shard_manager.h.o | tail -1
and run that command with clang 22.1.7.
