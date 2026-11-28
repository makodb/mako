# STO boundary and leaf workloads

The Masstree transaction adapter implements the STO datatype/storage contract.
It must not own application workloads, distributed coordination or migration
policy. TPC-C is a Rust application above those components, with no production
package depending on it.

1. Inventory the active workload, wrapper and native build. Preserve the current
   RocksDB API dependency cleanup. Record remaining compatibility-only code and
   the tests/proofs that cover each production responsibility.
2. Extract Rust TPC-C, its benchmark driver/configuration and executable into a
   leaf workload package. Move its tests without changing their assertions.
   Keep generic node configuration in the runtime. Preserve executable names
   and supported launch commands; confirm that no active C++ TPC-C remains.
3. Extract reusable Masstree storage ownership and retained-participant support
   from the old workload-named wrapper. Remove coordinator/migration/workload
   code from that storage component; keep these responsibilities in their upper
   components. Preserve storage safety, quarantine, replay and recovery tests.
   Retire obsolete compatibility entry points only with explicit replacement
   coverage and documentation.
4. Enforce dependency boundaries in component build/audit gates. Distinguish
   shared transaction cores from runtime orchestration in the component table.
   Update the book, dependency diagram, manifests, CI and accounting.
5. Qualify the isolated storage component, leaf workload and both production
   storage modes in the container. Run the upper protocol gate and update proof
   adapter mappings to the actual production implementation. Do not weaken
   proof or test oracles. Record results and remaining limitations.

Status: complete. The generic
`sto-masstree-api` has only STO/clock/index dependencies. `mako-workloads` is a
Rust leaf with an enforced ban on incoming dependencies. The book and agents.md
separate shared transaction cores from runtime orchestration; the grouped
production dependency graph is acyclic. The existing RocksDB facade cleanup is
preserved.

Qualification: 307 isolated Masstree cases plus four doctests,
129 runtime/facade/workload cases plus one doctest, strict Clippy and the upper
Verus/scenario gate in 177.819 seconds on 32 CPUs. Both executable profiles build. All eight RocksDB-only binaries passed
the dependency/symbol audit, 50 RocksDB-only regression cases passed, and a
two-shard TPC-C run passed with the new explicit launcher engine option. The
independent MASSTREE_TX build and final accounting checks passed. See [the qualification record](../testing/sto-boundary/README.md)
for retired APIs, replacement coverage and evidence.
