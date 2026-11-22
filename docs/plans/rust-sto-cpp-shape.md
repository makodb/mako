# C++-familiar Rust STO: implemented shape and stopped qualification record

## Current status — 2026-10-24

The vocabulary/source-shape migration is retained. The separately approved
[native Rust protocol alignment](rust-sto-rustycpp-alignment.md) now implements
datatype-controlled locking/publication, distinct distributed clocks, retained
participants, and actual Mako SRPC/coordinator/replication/replay integration.
Rust remains native; no generated C++ STO backend or broad C++ rusty-safety
campaign is required. Narrow bridge migrations do not complete that deferred
safety campaign. See [the architecture contract](../architecture/rust-sto.md)
for current APIs and the alignment record for bounded exercised evidence.

The protocol-alignment functional checks have passed: core/native integration,
local wrappers and cache consumers, real participant RPC, and all 11 separately
selected C++/Rust topology scenarios, including Paxos/Raft and colocation.
The alignment record names the receipts and the failing-before/passing-after
poll-worker starvation regression. Missing INSTALL replies fail-stop rather
than report success; stale shutdown acknowledgements cannot satisfy a new run.
This does not qualify mixed-backend clusters, live failover, distributed
crash/restart, throughput parity or a default-backend change.

**Performance qualification remains user-stopped.** Everything below is the
historical shape-migration plan and campaign record unless explicitly marked as
a current protocol correction. Imperatives, proposed gates, former API names,
then-current source descriptions and intermediate authorizations record that
work's chronology, not permission to resume it or guarantees for today's code.
Failed and stopped attempts remain failed/stopped; their source identities and
measurements are not current protocol qualification. The original all-gates
definition remains unmet. No new campaign or shared-service change is authorized.

## Historical shape migration and Q11 terminal status

Status: canonical vocabulary and source organization are implemented. Fresh Q11
matched builds, 570 functional cases, full correctness and instrumentation
gates, the public example, and bounded S1/R1 layout/code reviews passed.
Q11 is closed and **not performance-qualified** after baseline allocation
quality-budget exhaustion caused primarily by repeated host LXD restarts.
The authorized throughput floor remains **0.80**: at most **20% regression**,
with the same one-sided 95% confidence requirement and original R0 comparison
for both stages. No Q11 throughput comparison ran.
The user explicitly chose **“Keep performance unqualified”** and stopped further
qualification. Delivery retains the implemented migration and passed
nonperformance evidence; the original all-gates acceptance definition below
is **not met**. No further campaign or shared-service change is authorized.

Q9 is closed and **not performance-qualified**. Its R0 release allocation
cell with 16 items and eight workers exhausted the shared 60-attempt quality
budget at repetition two. No throughput comparison or S1/R1 performance
campaign launched; the 0.80 floor was never evaluated on Q9 timings.
All raw evidence is retained, all 64 clock policies are restored, and the
final host check found no Q9 containers. The user separately authorized a fresh
Q10 family on shared `z00` (`zoo000`), including its disclosed guest/storage and
contention risks. Historical Q7's 0.815950 lower bound remains unreclassified;
no old samples are pooled into the new policy.
A separately authorized one-shot R0 diagnostic also reproduced the sibling
quality rejection. Its all-task capture identified overlapping `go-ycsb`
threads owned by `zyang2` (UID 2001), with active last-CPU samples on all eight
guarded siblings. That diagnostic token is closed. Q10's private Docker,
userspace and source-only prerequisites are installed, with namespace smokes
passing. The user separately authorized a Q10-only enforced AppArmor ABI
correction. Actual Unix IPC, typed service-counter/error/reconnect and journal
checks now pass without guard changes. The fresh instrumented dependency and
91-test preflight passed. Independent fixed-clock PMU and exit-0/exit-23 scopes
verified exact restoration. The initial 30-worker quiet interval was rejected
and remains failed; every eventual sample still requires its own unchanged
bounded quality gates. Q10's fresh C0/R0, S1 and R1 builds, 570 functional
cases, full correctness/instrumentation gates and independent bounded S1/R1
code/layout reviews now pass. Q10 is closed and **not performance-qualified**:
its first 30-worker, one-item R0 allocation cell exhausted all 60 quality
attempts at repetition one. Two earlier allocation samples were accepted;
no throughput comparison or S1/R1 performance campaign ran. All 64 clock
policies were restored and the final host check found no private containers.
Allocation, memory, abort, soak/recovery and performance acceptance remain
unclaimed. No historical timing or nonperformance result replaces a Q10 gate.
The separately authorized one-shot Q10 diagnostic also consumed its quiet
admission without launching a workload. It identified overlapping KVM guest
execution on all four rejected worker/SMT CPUs, not exact attribution of every
tick or earlier baseline failure. Its token is closed and clocks are restored.
The user has now authorized fresh **Q11 on shared `zoo-002`**, including
temporary fixed 2 GHz controls on all 128 AMD CPU policies with exact normal
and supervisory restoration. Existing workloads, services, affinities and
security settings remain untouched. The 0.80 floor does not waive any quality
guard. Actual Docker/native-tool and typed service-counter/error/reconnect
checks passed. The source-only Cargo cache was copied with matching before,
after and destination inventories. Fresh distro-matched RocksDB 8.9.1-2 TSan
dependency preparation and the **100-test frozen preflight** passed, including
34 CLI checks and eight synthetic analysis CLI cases. These are not execution
of the instrumentation or performance acceptance gates.
A stale Intel-only C++ clock-receipt check was corrected for the actual AMD
shape. The first hardware setup then exposed an immediate-read assumption:
Linux [frequency-QoS updates are asynchronous](https://github.com/torvalds/linux/blob/v6.8/drivers/cpufreq/cpufreq.c#L1191-L1217).
Independent supervision restored that failed setup exactly. The controller
now observes one write with a bounded acknowledgment wait, rejects unexpected
values, and retains the recovery journal on failure; three regressions fail
before the fix and pass in the full preflight.
Actual fixed-clock scopes now prove normal and supervisory restoration of all
128 policies, including expected exit 23 and cleanup of one owned idle child.
Independent grouped PMU controls on CPUs 0 and 29 have full counter coverage
and pass the unchanged off-CPU limit. The 30-worker admission remains
**rejected**: CPUs 1, 8, 16 and 21 were fully busy, so that scope launched no
workload or PMU worker. A later whole-host interval did not reproduce those
four busy workers and did not establish a guaranteed clean replacement mask.
Placement, quality guards and all attempt budgets remain unchanged.
`validation/guard-v2-q11-host-control-rollup-01.json` records the separate
control proofs and failed admission. Final independent observation confirms
exact clock/control/lock restoration and no Q11-owned running container.
The independently finalized `R0-26` and `R0-27` Q11 boundaries each contain
25 binaries and nine attestations, but neither has an accepted matched
candidate. `S1-15` exposed build-root-dependent depfile, cached-flag and
assembly-debug identities. The `$$PWD` workaround fixed those differences,
but `S1-16` still failed exact native-byte matching: the production
fingerprint records a raw `MASSTREE_CONFIG_H` macro containing the physical
build root. Expanding the prefix-map flag literally did not fix that macro.
Both failed boundaries remain immutable and unadopted; sources and all
clock/control/lock state were restored exactly.
The replacement uses fresh per-stage physical native and native-Cargo
directories behind fixed logical `native/canonical` and
`targets/canonical-native` paths. Private host mounts and explicit Docker
binds authenticate the same directory inodes; normal and exceptional exit
unmount the aliases without deleting physical outputs. The sole build entry
is `freeze_guard_v2.py`; standalone factory/builder bypasses are removed.
Native comparison is exact again: no path/hash normalization, prefix-map
workaround, production-fingerprint edit or additional native-byte exemption.
An actual two-root Docker CMake/Clang/Ninja/Cargo smoke proves identical raw
macros, complete CMake caches and expanded recipes, C++ and both SRPC assembly
objects, and executed C++/Rust binaries. It also proves wrong-stage rejection,
read-only source views, writable native views, normal/exception cleanup,
unchanged sources and independently verified exact clock restoration:
`guard-v2-q11/provisioning/native-view-02/result.json`.
The subsequent `R0-28`, `S1-17` and `R1-17` boundaries finalized with 224
byte-identical common native files, exact recipes and matching shared inputs.
Their complete 570-case frozen-binary functional matrix passed. The first
correctness launch then failed before gate 1: `native_inputs` invoked its
Docker checker outside `execution_inputs`, so the daemon saw empty canonical
placeholders instead of the host's private native mounts. All 225 R1 native
files, worktree sources, clocks and host controls were independently verified
unchanged after cleanup; the failed attempt remains closed.
`native_inputs` now owns the authenticated execution scope for its before,
after and failure checks. A fresh real-Docker fixture reproduces the missing
bind before the fix, then verifies both native directories, rejects changed
native bytes, restores the Docker scope and succeeds again after restoration.
Normal and exceptional host unmounts also pass:
`guard-v2-q11/provisioning/native-checker-after-01/result.json`.
The repaired helper's fresh `R0-29`, `S1-18` and `R1-18` boundaries then
finalized with 224 byte-identical common native files and matching inputs;
their new 570-case functional matrix passed. Correctness run 02 executed all
14 gates through 19 successful commands and produced six reports, but its
final dependency check failed: unattended host updates changed `libsystemd`
and three frozen Linux headers. That run is closed, not accepted. All 225
native files, sources, clocks and protected host controls remained unchanged.
Neither restoring one old library beside new headers/package metadata nor
changing the host update service is an acceptable qualification environment.
A fresh private copy now contains coherent `/usr`, alternatives, OS/loader
metadata and matching dpkg state. All 670,730 before/after/copied inventory
entries agree. Both Docker launchers expose nine read-only mounts, including
the snapshot's source-path alias; actual writes through that alias fail.
Real probes verify the changed headers/library, matching package versions,
typed service/journal and 128-policy observation, and newly compiled/executed
C++ libc-ABI and Rust-thread programs:
`guard-v2-q11/provisioning/native-userspace-runtime-01/result.json`.
The independent host-control capture is byte-identical to the original
restored state and finds no containers. Host packages, update services,
security controls and foreign workloads were not changed.
The frozen preflight now requires this custody and the actual private
read-only bindings. Fresh RocksDB TSan dependency `thread-02` was rebuilt and
authenticated in that userspace; its earlier output is not adopted.
Fresh preflight 07 and matched `R0-30`, `S1-19` and `R1-19` boundaries now pass
under that private userspace. Their new 570-case functional matrix, all 14
correctness gates through 19 commands and six reports, and all five full
instrumentation gates passed, including all eight unbounded normal Loom models.
The documented public example compiled and executed. Fresh release/ThinLTO
S1/R1 reviews found no added mechanism work in the bounded caller/consumer
scope; all 16 private fixture size/alignment roles matched in each comparison
and profile. These remain dirty-local evidence, not clean-commit qualification.
No historical gate receipt replaced a fresh Q11 prerequisite.

The fresh baseline is now terminally closed. Its release allocation campaign
retained 71 attempts: six accepted, 61 measured quality rejects and four quiet
rejects. The 16-item, one-worker engine cell exhausted all 60 attempts at
repetition one: 56 measured LXD-restart rejects and four quiet rejects.
All 67 launched allocation workloads exited successfully and passed their
state/count/counter validation; none of these instrumented runs qualifies
throughput. Every measured quality rejection had an increasing typed LXD
restart counter. A fixed historical journal window overlapping the terminal
attempts shows repeated failures to bind `130.245.173.104:8443`, followed by a
nil-pointer panic, exit 1 and automatic restart roughly every 64 seconds.
No service, security setting or foreign workload was changed.

No throughput comparison or S1/R1 performance campaign launched, and the 0.80
floor was never evaluated. All 128 clock policies, protected host controls and
the original lock identity were restored exactly; independent observation
found no containers. The pre-publication source audit verified 10,271 file
hashes, the retained symlink identity and both explicit module deletions.
Raw attempts, source identities and restoration evidence are linked from
`validation/guard-v2-q11-baseline-failure-01.json`; the fixed historical service
evidence is in `validation/guard-v2-q11-service-attribution-01.json`.
Allocation, retained-memory, abort, soak/recovery and performance acceptance
remain unclaimed. The user chose to stop qualification and retain the migration
without performance acceptance; the decision is recorded in
`validation/guard-v2-q11-user-stop-01.json`. No further campaign or LXD change
will run under this instruction. Any future resumption requires separate
authorization and a fresh execution context after resolving the host-service
instability or selecting another host. Q11 attempts must not be reopened,
extended, reclassified or pooled.
The current contract remains [Rust STO](../architecture/rust-sto.md).
This plan responds to the requirement that a reader of the original C++ STO
should recognize the Rust object model and datatype protocol, without losing
the current Rust implementation's performance or safety properties.

### Frozen baseline and collector controls (2026-09-25–27)

- `freezes/R0-03-shared-lock-and-source-safe-build` finalized 25 binaries in
  nine attestations after 13 successful build commands. The native rebuild
  removed 830 declared build outputs while preserving five source/configuration
  outputs, including the original RPC header's bytes and file identity. All
  128 CPU policies were restored after the controlled-clock build.
- All 14 ordinary baseline correctness gates passed through 19 commands:
  `correctness/R0/1790287369301392198/result.json`. The post-execution census in
  `diagnostics/1790297604228139172-post-correctness-source/result.json` matched
  all 10,109 recorded source inputs and five repository identities to that freeze.
  This is a post-run source check, not a claim of continuous source monitoring
  by the correctness dispatcher.
- Release and ThinLTO diagnostic fixtures agree on all 16 recorded layouts.
  ThinLTO has no standalone `recycle_frame` symbol; retained diagnostics instead
  select the surviving commit, abort, and committed-poison callers. These
  artifacts establish a baseline, not semantic parity with an absent candidate.
- The first frozen-collector smoke rejected all three opening windows before
  launching a benchmark. Its process guard treated the Docker launcher's
  filename arguments as another runner, and its CPU interval included initial
  observation overhead. The shared detector now identifies the invoked program;
  the CPU interval follows the existing TPC-C guard's telemetry-bookend order.
  The new regression failed before correction; all 42 TPC-C driver tests pass.
- Actual Docker controls in
  `diagnostics/1790298486338446983-guard-runtime-controls/result.json` exclude
  the launcher and filename-only data arguments while detecting a real runner.
  Controlled observer work produced 84.09%/83.40% helper-CPU idle with the old
  boundary and 100% with the corrected boundary. A deliberately busy worker CPU
  still had 0% idle and was rejected. Clock restoration was verified.
- The user explicitly requested continuing with the existing one-CPU Python task
  untouched rather than stopping it or changing hosts. The shared guard now
  records a narrow idle-preflight admission for PID 3043244, UID 2008,
  start-time tick 370015719 and its exact command hash. Both snapshots must
  retain that single-thread identity on the same sole nonquiet CPU, and its
  process ticks must cover the observed busy ticks. This is shared-host
  evidence, not a claim that the background task cannot affect performance.
  Measured-worker off-CPU, during-process SMT, PMU, clocks, initial load, service,
  competitor, correctness and bounded-block checks are unchanged.
- Actual controls in
  `diagnostics/1790302997035028071-guard-runtime-controls/result.json` admitted
  a preflight with 200 busy ticks on CPU 8 and 200 ticks from that task. A
  mismatched process start time denied admission; deliberately added worker-CPU
  activity was still rejected. The process was not modified, and original
  clock policies were restored.
- The subsequent `R0-04-invocation-and-quiet-boundaries` attempt did not finalize:
  its successful native build selected a different cached transpiler executable
  from the one present after correctness testing. The explicit-empty-RUSTFLAGS
  environment selects SHA-256 `f7cb2c6f…`, while the native default environment
  restores the original R0-03 `12b791ca…` executable. Controlled Docker builds
  reproduce that selection; both attest the existing worktree gitlink
  `20707e98a371fee772729ae0a6b516e2d360ddd7` with `git_dirty=false`.
  No upstream source or pin was changed. The boundary wrapper now settles this
  native prerequisite before preparing its immutable receipts.
- `freezes/R0-05-native-tool-and-approved-preflight` then finalized all 25
  binaries and nine attestations after 13 successful build commands. The native
  tool retained `12b791ca…`; clock restoration was exact. Its complete source
  census differs from R0-03 only in the guards, their regression test, five
  protocol declarations and this record. The association in
  `diagnostics/R0-05-correctness-source-association.json` records unchanged
  production sources plus the focused guard evidence; it does not pretend the
  full correctness suite was rerun against R0-05.
- R0-05 actually executed eight core/mixed/native probe collectors, two cache
  collectors (selected write plus short drain/recovery), and four binary/fixed-u64
  point collectors successfully across release and ThinLTO. These 14 collector
  smokes are not full-matrix timing or the 960-second soak protocol.
- The release engine collector exposed an incomplete subprocess proxy before
  launching its workload. The proxy now preserves the driver's other subprocess
  helpers and restores its original module reference. The actual control in
  `diagnostics/1790318697550197577-engine-wrapper-control/result.json` reproduces
  the old `check_output` failure and completes real R0/C0 samples in both profiles,
  with exact counts and final-state checks. No noisy sample becomes accepted.
- The TPC-C smoke exhausted 60 attempts with zero clean blocks. Eleven real
  workloads passed their count checks, but the workload subprocess proxy also
  intercepted the shared monitor's service commands. The monitor now uses a
  separately loaded helper namespace. The control in
  `diagnostics/1790318852648214742-tpcc-monitor-control/result.json` reproduces
  the original proxy error with zero monitor polls; the corrected 30-second
  workload completes with 60 polls. Genuine LXD activity still rejects that
  corrected sample. Both controls restore all original clock policies.
- `freezes/R0-06-isolated-subprocess-collectors` finalized all 25 binaries and
  nine attestations after 13 successful build commands, with the native tool
  unchanged and exact clock restoration. Its complete census changes only
  collector/guard inputs, their test, protocol declarations and this record
  from R0-03. `diagnostics/R0-06-correctness-source-association.json` associates
  the unchanged production sources with the earlier correctness execution;
  it is not a newly executed full correctness suite.
- The host LXD daemon was repeatedly failing to bind an unavailable address,
  restarting approximately every 64 seconds. The user approved temporarily
  pausing its service and activation socket during measurement, after checking
  active workload state, then restoring their prior state. The scoped controller
  checks host PID namespaces and container/VM processes, stops only those two
  units, and requires no live service/control PID or queued job. It restores
  activation and enablement state on exit; historical PIDs/restart counters are
  not numerically restored. Neither Docker nor the existing Python task is
  altered. The original failed inactive-state check and its verified restoration
  remain retained; actual quiescence also permits a stopped failed service.
- Under that reversible scope, the actual R0-06 TPC-C collector completed one
  clean four-run, 30-second balanced block after 13 attempts:
  `diagnostics/1790345587674140718-frozen-collector-smoke/result.json`.
  The service/socket and CPU policies were restored. This is collector proof,
  not full-matrix qualification.
- The subsequent full engine baseline was stopped for a logging defect, not for
  unfavorable throughput ratios or an exhausted attempt budget. Its collector
  repeatedly serialized the entire campaign history: `results.json` reached
  203,384,894 bytes, and a retained process snapshot recorded 71.5% CPU for the
  collector. Genuine quality rejections also occurred; their cause is not all
  attributed to logging. No cell had completed its clean-block requirement at
  the observed stop boundary. Raw sample/block evidence remains retained, but
  the interrupted aggregate may be incomplete and none of that attempt counts
  as a qualified baseline. LXD and clock restoration were verified after stop.
- The engine driver and archive probe/cache/point/TPC-C collectors now append
  completed block records and emit the aggregate once before releasing the
  campaign lock, rather than repeatedly rewriting full history. The actual
  engine control in
  `diagnostics/1790367184831578649-engine-logging-control/result.json`
  retained 12 real samples and three blocks for both old and fixed drivers.
  Measured process write characters fell from 17,440,918 to 6,711,869 (38.5%).
  This is resource/retention evidence, not a throughput comparison; noisy
  blocks remain rejected. An intentionally wrong expected backend was rejected
  and retained with its partial block. Final aggregates agree with per-sample
  records and the new block journal.
- A simulated-child interruption regression reproduces the old driver's missing
  partial sample for both compare and variant entry points and passes after the
  fix. All engine, point and TPC-C driver suites pass.
- `freezes/R0-07-linear-campaign-logging` finalized 25 binaries in nine
  attestations after 13 successful build commands. The 10,109-input census and
  five repository identities preserve production source; only 14 harness,
  protocol, test and checkpoint paths differ from R0-03.
  `diagnostics/R0-07-correctness-source-association.json` retains the association,
  not a new full correctness execution.
- Actual R0-07 archive controls passed core, cache write plus short recovery,
  binary/fixed-u64 point and 30-second TPC-C cases:
  `diagnostics/1790371373251249051-archive-logging-control/result.json`.
  All retained journal entries agree with final aggregates and individual block
  and sample records, including rejected attempts. These are selected collector
  smokes, not complete matrices or a 960-second soak. Host state was restored.
- The first frozen allocation attempt failed before launching its first workload:
  the Docker collector classified its own host-side `measure_diagnostics.py`
  wrapper as a competing runner because Docker breaks their process ancestry.
  Initial load1 of 2.222 also legitimately exceeded the unchanged ceiling.
  `baseline/1790375114987703068-R0-frozen-samples` retains both observations;
  neither is accepted allocation coverage.
- The diagnostic host now passes its PID, UID, start-time tick and command hash
  into Docker. The shared guard excludes only that exact launcher PID,
  revalidates its identity on every process check and records the ownership
  evidence. Other processes, including the launcher's children, remain subject
  to the existing detector. No foreign process or quality threshold is changed.
- Allocation policy now predeclares at most 60 attempts per workload cell,
  shared across its three required repetitions, retrying only rejected quality.
  Real successful output is validated before measured noise can trigger a retry;
  execution, configuration, state and exact-counter failures remain fatal.
  Every attempt is retained and journaled; accepted `allocations.json` is
  finalized on success or failure without repeated full-history rewrites.
- `diagnostics/diagnostic-launch-control-20260926-01/result.json` proves actual
  host ownership exclusion, continued detection of a host-spawned competing
  child, and fail-closed rejection of a changed start-time identity. Real frozen
  allocation workloads with explicitly synthetic quality controls prove
  preflight/noisy/accepted transitions, exhaustion without a child, the shared
  attempt bound across repetitions, and fatal configuration validation despite
  noisy quality. These controls do not qualify timing. The owned fixture was
  reaped; LXD and CPU policies were restored. All 64 driver tests pass.
- `freezes/R0-08-owned-diagnostic-launcher` finalized 25 binaries in nine
  attestations after 13 successful build commands. Its complete 10,109-input
  census and five repository identities preserve production source; 15
  nonproduction paths differ from R0-03.
  `diagnostics/R0-08-correctness-source-association.json` associates ordinary
  correctness evidence and the exact diagnostic controls, not a new full
  correctness execution.
- The bounded R0-08 release allocation campaign retained 14 accepted samples
  in 159 attempts, then exhausted the 60-attempt cache-ack/items-1/workers-32
  cell at its first repetition. That cell had 41 quiet-window rejections and
  19 measured-quality rejections, with no accepted sample. The protected Python
  was observed on worker CPU 56 or sibling 120 in several rejected windows;
  separate Cargo/rustc builds also competed.
  `diagnostics/R0-08-allocation-noise-exhaustion.json` retains the observations,
  without attributing every rejection to one process.
- After a read-only launch-window check observed 61.58 seconds without active
  builds and with load1 below 2, the release engine C0/R0 campaign ran to its
  declared bound. It retained 1,850 attempted balanced blocks and 37 clean
  blocks. Only worker counts 1, 2 and 3 obtained the required six clean blocks;
  the other 29 cells exhausted their 60 attempts. This is inconclusive, not
  performance acceptance or an observed workload-correctness failure.
  `diagnostics/R0-08-engine-noise-exhaustion.json` records coverage and causes.
- Engine rejection evidence is dominated by quiet-window and SMT-sibling
  activity; only four blocks report worker off-CPU violations, and none report
  PMU or LXD violations. Several retained subsecond samples have a single
  accounted busy tick corresponding to 2.6–3.7% sibling utilization, coarser
  than the unchanged 1% ceiling. This establishes a timer-resolution limit,
  not proof that the observed activity was harmless or every rejection false.
- Both failed campaigns retain their raw evidence. LXD and CPU policies were
  restored after each. No foreign process or quality threshold was changed;
  ThinLTO collection was not reached. The fixed attempt bounds must not be
  silently reset or selectively continued. Baseline qualification remains
  incomplete, and canonical API migration remains unstarted.
- The user approved **longer engine samples**, not weaker quality thresholds
  or changes to the protected Python process. Before new baseline timing,
  `validation/engine-duration-calibration-policy.json` predeclares an ascending
  fixed candidate grid and a four-second minimum observed timed-worker envelope
  across frozen C0/R0, both profiles and workers 1/8/32. One diagnostic balanced
  block per cell supplies absolute-duration evidence; selection uses the same
  count for every arm and profile, never comparative ratios. All quality
  rejections remain recorded, and calibration samples cannot qualify performance.
  The chosen count and any mode-specific capacity constraints must be frozen
  before a fresh matched baseline; previous failed campaigns remain inconclusive.
- The pinned-Docker calibration completed 96 actual C0/R0 samples. The first
  common candidate meeting the predeclared duration floor was **16,777,216
  iterations per worker**; its shortest observed timed-worker envelope was
  **9.034472895 seconds** across both profiles and workers 1/8/32. The retained
  report is `diagnostics/engine-duration-calibration-01/result.json`
  (SHA-256 `e443d17b1463e605e46c1250224e39195a3e7252a9c10ab357280d0a8ad55621`).
  All 96 workload/configuration/count checks passed. None of the 24 selected-count
  samples qualified: all failed the initial-load gate, and nine also failed
  opening CPU quietness. None had a during-process SMT, PMU or off-CPU rejection.
  The existing initial-load latch is unchanged; subsequent qualification must
  start from a quiet launch window. LXD and clock restoration were verified.
- The revised v2 engine and allocation recipes use that fixed longer count for
  `engine` and `engine-logged`, with warmup still 65,536. Both paths reuse bounded
  key storage; logged serialization reuses one stack buffer per worker.
  `cache-ack` retains **262,144 iterations** and **524,288 queue slots per worker**:
  applying the longer count there would inflate real per-worker queue storage.
  Logged and cache-ACK engine groups are now separate so one driver invocation
  cannot apply the wrong mode's count. Coverage remains 52 final engine cells
  and 45 accepted allocation samples per profile; all quality limits and attempt
  bounds remain unchanged. A fresh immutable freeze and real workers-1/32 runtime
  controls across all five groups and both profiles precede new full baselines.
  These changes do not migrate production APIs or qualify previous failed runs.
- Before refreezing, 52 deterministic C++-gap archive controls passed across both
  profiles in `diagnostics/cpp-gap-v2-controls-01/controls.json`; these synthetic
  observations remain explicitly nonqualifying. Separate controls checked all
  eight actual calibration candidate/profile archives for internal consistency:
  only the two selected-count archives matched the new policy; all six shorter
  archives were rejected despite their internally consistent counts. Mixed-mode
  engine/cache groups were rejected, and both public C0/R0 CLI previews emitted
  the longer count. Those actual-archive controls are retained in
  `commands/1790455948973790000-engine-v2-policy-controls`.
- `R0-09-longer-engine-samples` prepared the revised recipe and built/staged all
  25 binaries through 13 successful build commands, but **finalization failed**
  on shared-input file-census drift. The post-failure census retained in
  `diagnostics/R0-09-shared-census-drift.json` found 222 additions and no removals,
  all under the shared `/home/users/shuai/.cargo/registry`. CPU policies were
  restored exactly. This boundary is not finalized and its binaries must not
  qualify a baseline. Its prepared source census still contains 10,109 inputs
  and five unchanged repository identities; the seven changes from R0-08 are
  the revised harness/protocols, archive controls and this plan, not production
  code. That is a prepared-snapshot association, not continuous source proof.
- The next boundary will use a private Cargo home reconstructed from retained
  dependency bytes, not a changing live cache. Preserve the complete registry
  and Git trees, file modes and relative symlinks; do not filter the census or
  rewrite historical Git remotes. Empty read-only container mounts over the
  original shared registry/Git paths will reject fallback access while keeping
  `~/.cargo/bin` tool proxies available. Actual offline Cargo checks must prove
  this isolation before refreezing; no shared cache or foreign process is changed.
- Private-cache reconstruction completed and verified **118,493 entries**:
  118,489 independent regular-file copies (2,280,127,131 bytes) and four relative
  directory symlinks. Exact file contents/modes, symlink targets and full census
  checks passed before atomic publication. The original shared cache was not
  changed. `diagnostics/isolated-cargo-home-v1/report.json` retains the mapping
  and verification journals; historical Git origin URLs remain byte-preserved.
- The real pinned-Docker isolation smoke passed. Both old cache paths were
  observed as empty read-only tmpfs mounts and rejected writes with `EROFS`.
  Offline locked metadata for the workspace, transpiler and both probes resolved
  every external package manifest inside the private cache; fresh `arc-swap`
  and `sto-core` compilation also succeeded. See
  `diagnostics/isolated-cargo-smoke-v1/result.json`; this is not timing evidence.
- Fresh native prerequisites were built in
  `/var/tmp/mako-sto-cpp-shape-native/v2-private-cargo`, preserving the old build
  tree. The actual CMake cache has no differences from R0-09 after normalizing
  only the declared native-build and Cargo-home paths. Native and Rust dependency
  records contain no old shared-cache paths. The rebuilt transpiler still reports
  clean approved commit `20707e98a371fee772729ae0a6b516e2d360ddd7`
  (binary SHA-256 `94b1c0d3958ac5c39bdb6c33e07eede7b28f64b41cfbda803197a1d0b9612a4c`).
  `diagnostics/private-native-inputs-audit-01/result.json` retains these checks;
  bootstrap clocks were restored exactly. A new complete immutable boundary is
  still required; failed R0-09 was not rebased or finalized.
- With the private-cache/native-root recipe active, all 52 archive-analysis
  controls passed again across release and ThinLTO
  (`diagnostics/cpp-gap-v2-private-cache-controls-01/controls.json`).
  They remain explicitly synthetic, non-performance controls.
- `R0-10-private-cargo-and-longer-samples` finalized successfully: 25 read-only
  binaries, nine attestations and 13 successful build commands. All executable
  and attestation hashes were checked, and clocks were restored exactly.
  The full 10,109-entry source comparison and five repository identities show
  no production-source change: 18 non-production deltas from R0-03, eight from
  R0-08, and only `qualification.py` plus this plan from failed R0-09. See
  `diagnostics/R0-10-finalized-boundary-verification.json`; this is an immutable
  build boundary, not a new correctness run or performance qualification.
- The revised real-allocation launcher control passed all four cases, including
  an actual 16,777,215-iteration request rejected against the declared 16,777,216.
  Owned-launcher exclusion, descendant detection, shared retry budgets and
  fatal-error precedence passed; clock and LXD restoration were verified.
  `diagnostics/R0-10-diagnostic-launch-control-01/result.json` retains the proof.
- All 20 real longer-count runtime controls completed with valid state/counts
  and end-identity checks. Non-cache worker intervals ranged from 9.113 to
  387.830 seconds; the longest process took 389.490 seconds, below 600 seconds.
  Cache-ACK retained 262,144 iterations and capacity 524,288 per worker.
  **None qualified for timing:** all 20 failed the unchanged initial-load gate,
  with additional quiet/SMT failures. The operator launch window ended at
  load1 1.473, but the first actual opening guard observed 2.502. No cause is
  inferred from that timing difference alone. See
  `diagnostics/R0-10-longer-engine-control-audit.json`.
- The complete allocation campaign then exhausted its declared budget at
  `engine/items16/workers32/repetition1`: 29 rejected preflights and 31 valid but
  SMT-noisy executions, with no accepted sample in that cell. Overall it retained
  124 attempts (eight accepted, 55 quality-rejected, 61 preflight-rejected).
  ThinLTO was not launched; no budget was reset or partial campaign continued.
  Clocks and LXD were restored. See `diagnostics/R0-10-allocation-noise-exhaustion.json`.
- A five-interval idle/unchanged-sampler/idle/unchanged-sampler/idle observation
  found worker-SMT activity above 1% even with the sampler off (2.83–2.94%).
  Another user's `apas` task consumed 2.66–3.10% of a CPU across these intervals;
  endpoint locations included CPUs 96, 99 and 114. These observations do not
  establish its exact residency during the earlier allocation campaign.
  The guard-only explanation is insufficient; its thresholds and polling remain
  unchanged. All foreign processes were left untouched, and protected-Python
  affinity plus clock/LXD restoration were verified. Raw evidence is in
  `diagnostics/R0-10-monitor-interference-01/`. Stage 0 remains unqualified.
- The user selected **Quiet benchmark window** rather than remapping benchmark
  CPUs or changing hosts, and subsequently confirmed **Ready now** on 2026-09-27.
  Execution resumes with the existing technical preflight checks before a fresh
  complete campaign. No foreign-process changes are authorized. Placement,
  workloads and quality gates remain unchanged; the exhausted campaign remains
  immutable.
- In the confirmed window, a preliminary full-topology guard rejected the busy
  helper CPU occupied by the protected Python process and launched no benchmark.
  The subsequent complete allocation campaign used the collector's unchanged
  bounded preflight handling, not that extra admission gate.
- That campaign retained 123 attempts (eight accepted, 28 quality-rejected,
  87 preflight-rejected) before exhausting `engine/items16/workers32/repetition1`.
  Its failed cell had 57 rejected preflights and three valid-but-noisy executions.
  Foreign Cargo, rustc and linker activity was recorded during the window;
  ThinLTO was not launched. Clocks/LXD were restored and no foreign process was
  changed. See `diagnostics/R0-10-owner-window-allocation-audit.json`.
- One complete recovery campaign is predeclared after the authorized window
  recovers: no compiler/build process and load1 below two continuously for
  60 seconds, observed within 30 minutes. This read-only launch condition is
  not sample acceptance. Existing per-sample guards remain authoritative;
  prior campaigns remain closed, and no partial results will be continued.
- The recovery observer reached its declared launch condition (61.6 continuous
  quiet seconds, final load1 0.756), but new foreign Cargo/rustc builds appeared
  during the recovery campaign. The first cell, `engine/items1/workers1`, exhausted
  all 60 attempts at preflight: all failed the initial-load gate, 54 also recorded
  competing processes, and 43 also failed CPU quietness. No benchmark executed
  and ThinLTO was not launched. Exact clock/LXD restoration was verified; see
  `diagnostics/R0-10-owner-window-recovery-allocation-audit.json`.
- The single predeclared recovery campaign is closed. A brief quiet observation
  does not reserve this shared host. Under the then-current policy, Stage 0
  required a sustained owner-coordinated exclusion of competing build/test jobs
  or a separately approved measurement host. No further campaign reset,
  foreign-process change or production API migration was performed.

### Approved shared-host revision (2026-09-27)

- The user subsequently authorized continuing on zoo-002 with other cores
  occupied. The new frozen policy is `shared-host-worker-smt-v1`: host load,
  recognized background processes, and helper/monitor-only CPU activity remain
  recorded observations, not independent rejection criteria.
- Preflight CPU quietness applies to selected workers and their SMT siblings.
  Preserve the existing exact-identity protected-Python preflight allowance;
  it still provides no exemption from measured-sample guards. Do not change
  any foreign process or its affinity.
- Preserve fixed clocks/placement, the applicable measured-worker off-CPU,
  SMT and PMU thresholds, service checks, exact state/count validation,
  fatal-error precedence, balanced ordering, confidence method, workload counts,
  profiles and bounded attempts. Our own builds, tests and measurements remain
  serialized.
- This qualifies shared-host measurements only. Helper contention and shared
  cache/memory-bandwidth interference are not excluded; paired confidence bounds
  do not establish interference-free or all-host performance. Lanes without
  per-worker PMU/off-CPU instrumentation retain that limitation explicitly.
- Validate admission with actual off-core background work and rejection with
  actual worker/SMT contention, then freeze a new complete source/policy boundary
  before starting fresh matched baselines. Do not reinterpret old rejected
  samples, continue exhausted campaigns, or begin the API migration before
  Stage 0 qualifies.
- Integrated Docker validation passed Python syntax for 22 root helpers, eight
  protocol parses, the existing statistical controls and all 52 C++-gap archive
  controls. Command: `1790537680254807553-archive-harness-validation`.
- Four one-shot real interference controls passed using immutable R0-10 release
  binaries and separately retained revised helpers. A recognized benchmark on
  helper CPU 1 (0% idle) was admitted; contention on worker CPU 32 was rejected
  with 59.9% measured off-CPU time; 100% busy SMT sibling 96 was rejected.
  A deliberate expected-engine mismatch remained fatal under worker contention.
  All four real output records also rejected intentionally incorrect count
  expectations; no output or quality result was fabricated.
- These controls are not qualification. All owned noise children were reaped,
  original clock/LXD states restored, and old frozen assets plus current helper
  identities verified. See `diagnostics/shared-host-controls-audit-01.json`
  and `diagnostics/shared-host-policy-delta-01.json`.
- `R0-11-shared-host-admission` finalized 25 binaries, nine attestations and
  13 successful build commands. All executable/attestation hashes, eight
  referenced provenance manifests, build logs and build-receipt hashes were
  verified; original clocks were restored. Its prepared boundary compares all
  10,109 source entries and five repository identities with R0-10: exactly
  13 declared nonproduction changes, no added/removed inputs or production-source
  changes.
- All R0 executable hashes are unchanged from the real-control boundary.
  Three C0 ThinLTO executables differ bytewise; no reproducible-build claim is
  made, and future C++ comparisons must use the identical new frozen C0 anchor.
  See `diagnostics/R0-11-finalized-boundary-verification.json`. This establishes
  the new baseline boundary, not performance qualification or a fresh correctness
  rerun. Fresh allocation and C0/R0 campaigns now use shared-host admission
  directly, without the obsolete compiler-free/low-load launch prerequisite.
- The R0-11 campaign is now closed without qualification. Release allocation
  accepted 41/45 required samples in 119 attempts; all 41 accepted samples had
  load1 above the former ceiling, proving the obsolete host-load veto was gone.
  Every one of the 55 executed samples completed and validated its measurement.
  Actual worker/SMT overlap exhausted the 60-attempt budget for logged engine,
  32 workers, repetition 3; later lanes were not launched. Original clock and
  LXD states were restored. See
  `diagnostics/R0-11-shared-host-allocation-exhaustion.json`.

### Fixed sparse benchmark placement (2026-09-27)

- A fixed 60-second, six-interval read-only observation found busy SMT siblings
  in both contiguous 32-core blocks. Neither block supplied a clean full-width
  replacement. Actual topology shows 64 physical cores, paired logical IDs
  `c` and `c+64`, all on socket 0 / NUMA node 0.
- Forty-seven physical cores had both siblings at or below 1% activity in every
  interval. Select the first 32 eligible lower-thread IDs in ascending order:
  `0,2,3,5,6,7,9,10,12,13,14,15,16,18,21,22,23,24,26,27,28,30,31,32,33,34,35,36,37,38,40,42`.
  Reserve the next eligible core, CPU 43, for writeback where applicable.
  Remaining lower-thread IDs form the disjoint helper/monitor pool.
  See `diagnostics/fixed-sparse-placement-selection-01.json`; the observation
  SHA256 is `f99ad757a2e82260b9fdcda2b5fcfb4f981049d80afddc6f16ad838092aa2752`.
- Every matched source label/profile uses the same ordered prefixes. Never
  remap per sample, select by throughput, reduce worker counts, or change any
  workload, threshold or attempt budget. Docker remains restricted to CPUs
  0–63. No foreign process is moved, stopped or reprioritized.
- Add explicit CPU lists only to benchmark startup/pinning/reporting; retain
  standalone contiguous/default modes and native unpinned mode. Keep placement
  out of transaction loops and the mixed fixture's Copy configuration layout.
  Core/ordinary-cache inherited affinity remains honestly distinguished from
  individually pinned workers. Point/TPC reuse their existing physical-CPU-list
  interfaces; no new pinning instrumentation is claimed.
- This observation is neither future quietness nor performance qualification.
  Runtime worker/SMT/PMU/off-CPU, service, clock, state/count and bounded-block
  checks remain required wherever instrumented. Frozen interference controls,
  a complete new immutable boundary and fresh matched baselines are pending.
  The canonical API migration has not started.
- Pinned-Docker validation now passes all 20 engine, four point and 42 TPC-C
  driver regressions, plus Python/protocol parsing, statistical controls and
  all 52 C++-gap archive controls. Release R0/C0 benchmark builds succeeded.
- Real startup controls passed 12 positive / 31 invalid-input cases for R0
  and six positive / 15 invalid-input cases for C0. They exercise reversed
  sparse order, contiguous/default modes, native unpinned mode, and 32-worker
  short soaks with complete drain and fresh-process exact-state verification.
  Invalid lists fail without creating a database or manifest. The 32-worker
  reopens verified all 8,192 keys and respectively 234,976 R0 / 249,420 C0
  acknowledged commits; these counts are observations, not matched throughput.
  See `diagnostics/sparse-affinity-validation-01.json`.
- All 90 R0 release functional smokes passed, including 32-worker engine,
  logged/cache-ACK, mixed-adapter and native-scenario cases plus payload
  transitions. Exact ordered CPU metadata and applicable per-worker/observed
  CPU records were checked. All 15 matching C0 release engine/logged/cache-ACK
  smokes also passed. Receipts:
  `smokes/1790552973274636949-R0-release.json` and
  `smokes/1790553430849453562-C0-release.json`.
- These functional runs used pinned Docker without changing clocks, LXD or
  foreign processes. They do not qualify timing or the 960-second retention
  protocol. A full new freeze, frozen interference controls and fresh matched
  baseline measurements remain pending.
- **2026-09-28 immutable boundary and collector proof:** `R0-12-fixed-sparse-placement`
  finalized 25 binaries, nine attestations and 13 successful build commands.
  The 10,109-entry source census has exactly the 23 declared benchmark,
  collector, policy and documentation changes relative to R0-11, no added or
  removed entries, and the same five repository identities. Transaction/core
  source is unchanged. Final binary, provenance and clock-restoration checks
  are retained in `diagnostics/R0-12-finalized-boundary-verification.json`.
- All four one-shot frozen interference controls passed: helper admission,
  selected-worker rejection, real SMT-sibling rejection and fatal configuration
  precedence under worker contention. All 17 frozen collector smoke invocations
  passed: eight each for release and ThinLTO, plus TPC-C fat-LTO/unwind.
  These cover core/mixed/native probes, paired engine, cache/short-soak and both
  point-value modes, not full qualification matrices. See
  `diagnostics/R0-12-shared-host-controls-audit.json` and
  `diagnostics/R0-12-frozen-collector-smoke-audit.json`; clocks and LXD were
  restored after both scopes.
- **Fresh R0-12 baseline failed closed:** release allocation accepted two of
  45 required samples (first-repetition engine singleton at one/eight workers).
  The 32-worker cell exhausted its 60-attempt budget: 29 preflight rejections
  and 31 measured SMT-overlap rejections. All 33 executed measurements passed
  state/count validation. SMT CPUs 94, 96 and 97 account for most measured
  overlap; other siblings also exceeded the unchanged 1% ceiling. Global load
  remained observational and was not a rejection reason. Later release-engine
  and ThinLTO lanes did not start. Clock and LXD restoration passed.
  The closed campaign is `baseline/1790567715239638398-R0-frozen-samples`;
  the audit is `diagnostics/R0-12-sparse-allocation-exhaustion.json`.
- A subsequent read-only thread observation found active foreign threads with
  all 128 CPUs allowed, so an endpoint CPU observation is not a stable exclusion
  boundary. It does not attribute historical rejected samples to specific
  processes. No foreign process was signalled, reaffined or reprioritized.
  No samples are pooled, no exhausted campaign is resumed, and no retry budget
  is reset. Baseline qualification and canonical API migration remain blocked
  on an admissible full-width measurement window or an explicit policy decision.
- The subsequent bounded fixed-pool observation completed all 180 ten-second
  intervals without changing clocks, services or foreign processes. Only
  31 intervals met worker-idle ≥95% and sibling-busy ≤1%; the longest
  consecutive span was six intervals (60 seconds), not the separately
  predeclared five-minute observation window. This was an eligibility
  observation, not a new benchmark gate or performance sample. Receipt:
  `diagnostics/fixed-pool-window-observation-01.json`
  (`3fb62bd1d24d01329eeebd3e1b83d7ebcd133ce6505220965b760c964554c527`).
  No benchmark campaign was launched or resumed by this observation.

### Baseline prerequisites and approved exceptions (2026-09-24)

- The current source, including pre-existing user changes, is preserved in the
  qualification artifact at `/home/users/shuai/mako-sto-cpp-shape-20260924`.
  R0/C0 release and ThinLTO staging builds and separate R0 allocation builds exist;
  successful builds and functional smokes are not final performance acceptance.
- The C++ point benchmark required a pre-baseline compile correction:
  generic `MassTrans` comparator signatures now use their existing `value_type`
  instead of hard-coded `std::string`. String specializations retain the same
  type. C0 is frozen only after this prerequisite; the prior source is preserved.
- The original Miri baseline passed all 66 ordinary failure-test assertions but
  failed leak checking: 122 allocations came exclusively from the three existing
  `singleton_view` cases named below. Their fixtures deliberately inject
  failures that retain indeterminate/quarantined state.
- The user explicitly approved adding only
  `singleton_view::singleton_changed_targets_keep_each_failure_phase`,
  `singleton_view::singleton_target_borrow_panics_keep_each_failure_phase`, and
  `singleton_view::singleton_guard_and_finish_panics_keep_cleanup_boundaries`
  to the shared exact quarantine inventory (35 to 38 identities), then rerunning
  the exact-case gates. This is a narrow baseline qualification correction,
  not permission for additional suppressions or production quarantine changes.
  Ordinary cases retain leak checking; qualified cases still run individually.
  The revised Miri gate and all 38 exact ASan reruns passed, including the
  workspace audit of all 38 unique skip matches. Those focused results did not
  qualify the native sanitizer stages; complete execution is recorded below.
- On 2026-11-21 the user approved two further identities (38 to 40):
  `retained_participant_preserves_poisoned_and_unknown_outcomes` and
  `installed_participant_cleanup_failure_is_definitely_committed`. Both arrived
  with the retained participant API after the audit above and were never added
  to the inventory. They inject a panic into a retained participant's acquire,
  install and finish callbacks, the same faults as their listed local-commit
  counterparts, and the 61 allocations that Miri and ASan reported for
  `failure_protocol` are the transaction frames and lock plans those faults
  quarantine. No other suppression was added.
- On 2026-11-22 the user approved running the six multi-process protocol tests
  (`test_sto_tpcc_protocol_rust_none`, `_none_colocated`, `_paxos`,
  `_paxos_colocated`, `_raft` and `_raft_colocated`) with ASan's leak reporting
  off. Each starts whole server processes that keep replication, worker and
  runtime state for their whole life and release it by exiting; in CI the two
  unreplicated tests alone reported 2,088,600 bytes in 7,335 allocations and
  4,182,616 bytes in 14,671 allocations at exit. The runner runs exactly these
  six in their own stage, `ctest-rust-protocol-process-lifetime`, and records
  the exception in its manifest. Address errors stay fatal in every child
  process, and every other Rust-labeled test keeps leak detection. This is not
  permission to extend the list.
- The first runtime ASan baseline found two additional borrowed-owner fixtures
  missing from the separate adapter/cache inventory. Isolated runs attributed
  all 438 allocations (134,272 bytes) to
  `txn::borrow_tests::real_cleanup_panic_pins_original_for_abort_drop_operation_and_commit`
  and
  `txn::borrow_tests::uncertain_holder_keeps_original_backing_alive_after_pool_destruction`;
  the remaining ordinary library tests passed with leak detection enabled.
  The user separately approved adding only these two exact identities to that
  inventory. They still run individually with address instrumentation and all
  assertions; only leak reporting is disabled. The shared 38-case inventory,
  startup suppression roots, negative controls, and production ownership are
  unchanged. The subsequent complete execution is recorded below.
- Other pre-baseline corrections preserve production semantics: an unsigned
  empty-batch comparison now uses `== 0` for Clippy; the stale
  `abort-cleanup-poison-hidden` mutation selector now targets `cleanup?;`; and
  the native differential transcript projects the facade's documented derived
  observer bit back to the C ABI capability word while checking that relation.
  The corrected hook, recovery, and mutation gates pass. Benchmark output now
  retains exact Rust allocation totals alongside rounded per-commit rates;
  counters remain outside timing builds unless explicitly enabled.
- The subsequent ASan gate reached `mako-local`'s clock differential: assertions
  passed, but the documented native process-lifetime arena and table state
  produced five leaks (35,656 bytes). Its existing native LSan rules covered
  112 bytes of table state, but the optimized allocation stack no longer
  contained the old `Sto::transaction` rule's frame. The user approved a stable,
  non-inlined cold allocation helper, a narrower native rule, and a real
  owned-read-buffer negative control. Native allocation ownership and
  reclamation remain unchanged; the shared quarantine and Rust startup-root
  inventories are not expanded.
  The helper is now demonstrated by a positive clock differential with leak
  checking enabled and a negative control reporting exactly one lost 1,301-byte
  owned read. Optimized native frames require DWARF allocation unwinding
  (`fast_unwind_on_malloc=0`, 128-frame context); the narrow rules are unchanged.
- UBSan and TSan reached the native facade's forced-history fixture under
  `STO_RMW=OFF`, which advertises only point transactions. That fixture requires
  RYW and transactional scans. The corrected recipe preserves the default-OFF
  C++ gates and three exact OFF-specific Rust checks, then rebuilds a consistent
  ON native library and direct control for the complete facade suite. It
  requires the actual three-way differential driver and leaves the shared
  forced-history oracle unchanged. UBSan's three OFF checks pass. A subsequent
  C++ fixture, `MvInsertedValueCanGrowBeforeCommitOrCleanup`, also requires
  own-write composition: its whole body now uses the existing RYW profile guard.
  Every sanitizer must execute the full existing `test_silo_nontxn_api` CTest
  after the ON rebuild, with an explicit successful growth-case execution audit.
  The same CTest's existing ASan native lifetime qualification is retained;
  no new leak-disabled test identity is added. Focused UBSan confirmation passes
  the full OFF and ON CTests, including the unchanged growth assertions in ON.
  The later complete rerun covers all three sanitizer profiles.
- The next TSan workspace failure is a native RocksDB thread-pool mutex report
  during concurrent MRX opens. The actual system DSO is uninstrumented RocksDB
  8.9.1; source and ELF inspection show a function-local-static guard fast path
  and mutex-protected pool resizing. Matched Clang22 builds of the same patched
  8.9.1-2 source isolate the missing sanitizer-visible publication edge: all
  three control runs of a Rust/C-ABI guard probe report the mutex race, while
  all three instrumented runs pass. The original MRX test executable likewise
  reports it in all three control runs and passes all 14 tests in each of three
  instrumented runs. The archived TSan recipe selects the retained instrumented
  DSO using existing `ROCKSDB_LIB_DIR`/loader controls and checks its exact hash
  before and after execution. Unsanitized Loom removes that override; production
  and performance continue using the original system dependency. No RocksDB
  suppression, test exclusion, or serialized workaround is added.
- With that dependency, the full TSan run passes the 14-test MRX boundary and
  reaches `Record`'s release-decrement/acquire-fence destructor. TSan cannot
  observe fences; the pinned standard library uses an acquire load in `Arc`
  instead. `mrx-core` now follows that sanitizer-only pattern, detected by a
  stable Cargo build-script cfg bridge. Ordinary builds retain the original
  fence and representations. The retained last-reference regression fails
  before the correction under TSan and passes afterward; Miri passes both.
  A separate three-process probe changes from three reports to three passes,
  while all three deliberately racy negative controls still report a race.
- Both baseline09 ASan and UBSan complete their runtime gates, including the
  supported ON native suite, but fail the final source-stability check. Our
  concurrent fresh CMake builds share `rcc_rpc.h`: one generator can read the
  header while another has truncated it, losing all 18 existing wire IDs.
  A controlled overlap reproduces this; three serial regenerations preserve
  every byte. The original header is restored from the initial preserved
  source, not assigned a new protocol. The campaign launcher serializes gate
  invocations with a shared build lock and verifies selected source hashes
  after execution. These stale runs remain nonqualifying; the serialized rerun
  is recorded below.
- Fresh `baseline-10-record-and-serialized-builds` execution passes full Miri,
  all eight normal Loom models, and complete TSan, ASan, and UBSan gates.
  All five outer commands return zero and preserve the same 22 selected source
  hashes. Each native manifest reports `gate_status=passed`, both native ABI
  profiles pass, and each separate adapter/cache report has `verdict=pass`.
  ASan also confirms both real 1,301-byte native leak controls and the existing
  exact lifecycle tests. The TSan durability suite passes all 17 cases.
  Full manifests, cache reports, compiler commands, and sanitizer logs are
  retained under the artifact's `instrumentation/R0/` directory. These are
  unchanged-dirty-source diagnostics with the existing exact qualifications,
  not clean-commit evidence, finalized R0/C0 artifacts, or performance results.
- The freeze harness also avoids CMake's `--clean-first`, which deletes
  source-owned generated headers. A disposable actual CMake/Ninja reproduction
  confirms that behavior. The archived `native_rebuild.py` instead removes
  declared build-tree outputs, preserves external outputs and configuration,
  and rebuilds native objects without changing the source header's identity.
  Freeze attestation requires that exact source-preserving build command.
- Clock setup initially stopped before changing policy: Linux's
  `protected_regular=2` rejects root's create-open of the existing user-owned
  shared lock. Archived drivers now follow the existing checked-in engine
  driver's read-only `flock` convention, preserving the same inode and policy.
  The Docker control confirms exclusion while held, acquisition after release,
  unchanged lock metadata, and all 128 policies at fixed 2 GHz with boost off.
  The enclosing controller then verifies restoration of all original policies.
- The allocation counter interface executes 30 short scenarios: both release
  profiles, 1/8/32 workers, engine items 1/4/16, logged engine, and cache ACK.
  Exact integer counts, byte totals, and final state checks pass. These are
  concurrent-build smoke checks, not frozen allocation or timing measurements.
- Retained-source reconstruction and the private-layout recipe execute in
  Docker: the representative hot component is 56 bytes and full owner 128
  bytes, among 16 recorded size/alignment measurements. This is a prepared-source
  diagnostic, not a finalized performance baseline. Freeze preparation succeeds,
  but the newly corrected gate sources require a fresh boundary before building
  and finalizing R0/C0. No qualification timing has begun; fixed-clock setup and
  complete restoration have only been exercised by the separate control above.

## Decision

Keep the original STO concepts as the canonical vocabulary: **Transaction,
TransItem, TransProxy, TObject, and version/lock helpers**. Use Rust structs,
traits, typed payloads, and scoped borrows to implement those concepts. Do not
introduce a second C++-looking facade over the existing API, force virtual
per-item dispatch, or rebuild the transaction algorithm.

The migration is a clean source cutover: change selected definitions and every
caller together, with no compatibility aliases, deprecated trait paths, or
parallel old/new protocols. Existing C++ reference binaries, public native ABI,
cache record formats, and engine defaults remain unchanged.

**Priority order:** preserve correctness and ownership; preserve measured
performance; make the conceptual and source organization familiar. Exact C++
syntax or a six-method trait is not a reason to weaken either of the first two.

## What “familiar” means

An adapter author should be able to identify, without first learning the fast
paths:

1. the transaction that owns the tracking set and commit protocol;
2. the item for one object/key dependency;
3. the scoped proxy used to record that item's observations and writes;
4. the datatype object responsible for checking and installing those effects;
5. the version/lock helper that protects publication; and
6. the ordinary lock, predicate-check, read-check, install, unlock, cleanup flow.

The canonical source entry points should make that relationship visible.
Specialized batches, direct-token plans, and singleton bodies remain optimized
representations of the same protocol, not alternative public object models.

The source comparison uses [TObject](../../src/mako/sto/Interface.hh),
[TransItem/TransProxy](../../src/mako/sto/TransItem.hh),
[Transaction](../../src/mako/sto/Transaction.hh), and the
[TBox](../../src/mako/sto/TBox.hh) and [TCounter](../../src/mako/sto/TCounter.hh)
adapters. It is not a promise to port all historical datatypes or capabilities.

## Target concepts and bounded naming changes

| Current Rust | Target | Invariant |
| --- | --- | --- |
| `Transaction` / `TransactionFrame` | Keep names and ownership | Explicit worker borrowing, consuming completion, and core-owned phase state remain. |
| `TransactionalResource` | `TObject` | Same associated payload types, registration policy, static dispatch, and callback semantics. |
| `Entry<'_, A>` | `TransProxy<'_, A>` | Scoped typed access, first-error/unwind doom behavior, no freely escaping mutable item. |
| `TerminalReadEntry` | `TerminalReadProxy` | A distinct restricted proxy; cannot become a general writable proxy. |
| Private `ItemBox<A>` | Private `TransItem<A>` | The existing owned heterogeneous item, not a new allocation or universal physical row format. |
| Private `ItemData` / `TypedItemBatch` | `TransItemData` / `TypedTransItemBatch` | Existing split batch storage and sidecars, with unchanged ownership/layout. |
| `validate_read` on the object protocol | `check` | Final certification only; do not rename unrelated validation methods. |
| `upgrade_predicate` on the object protocol | `check_predicate` | Still returns the covering observation and performs commit-time upgrade; not a Boolean-only test. |
| `finish` on the object protocol | `cleanup` | Same exact-once and authorized drop-only/quarantine exceptions. |
| `FinishItem`, `FinishContext`, `FinishDisposition`, `PreflightFreeReadFinish`, `with_drop_only_committed_finish` | Corresponding `Cleanup*`, `PreflightFreeReadCleanup`, and `with_drop_only_committed_cleanup` names | Same phase restrictions, disposition values, and cleanup selection. |
| Proxy `record_read`, `record_predicate`, `stage` | `add_read`, `set_predicate`, `add_write` | Same transition checks; adding a write replaces an already-composed intent, not automatic operation merging. |
| `Runtime`, `WorkerContext`, `ObjectRegistration`, `RegisteredResource`, `ResourceClass` | Keep as supporting registration/lifetime machinery | Do not collapse runtime/object/resource-class identity into a pointer or an ambiguous object handle. |
| `AtomicVersion`, `TransactionLock`, direct capabilities, phase contexts | Keep their actual roles | Familiar object vocabulary does not require a new lock algorithm or a wrapper layer. |

The callback renames are trait-scoped. They must not touch `Hasher::finish`,
formatting-builder `finish`, unrelated cache/FFI lifecycle methods, or internal
methods that intentionally describe a different phase. Related private erased
forwarders can adopt the same terminology where they implement that exact
callback; no global textual replacement is permitted.

The current private datatype adapters implement the renamed `TObject` trait.
They are the registered object-protocol implementations, not a new facade.
Do not fuse a cloneable public datatype handle with its registered state merely
to put every method in one C++-looking struct; preserve existing ownership and
reference-count behavior.

`Observation`, `Predicate`, `Intent`, `Local`, and `Prepared` remain typed
associated data. The advanced `replace_read` successor operation retains its
explicit name and proof obligation; it is not relabeled as an unrestricted
C++-style refresh. The existing public datatype names such as `TxnCell` and
`TxnCounter` stay: familiarity must not imply that Rust's non-predicate counter
already implements C++ TCounter's predicate comparisons.

### One conceptual item, multiple physical representations

`TransItem` denotes a transaction-owned logical dependency. The heterogeneous
lane can store an owned `TransItem<A>`; a homogeneous batch can continue storing
its fields in dense arrays and sidecars. Do not add a full item, object pointer,
Arc, box, or vtable to every batch slot merely to make the diagram literal.
Adapters continue receiving the existing phase-scoped item views. This plan
does not expose arbitrary construction or mutable access to core-owned items.

The binding remains part of identity: conceptually `(object, key)`, with runtime,
object ID, and resource class retained internally. A Masstree table's record,
directory, and trusted-scan bindings must not accidentally merge. This is why
`RegisteredResource` is not mechanically renamed to `RegisteredObject`.

### Preserve the scoped proxy boundary

Keep `Transaction::with_item` and its resolved/batched variants as the canonical
operation entry. The proxy's name and methods become familiar, but the scope
continues to arm the transaction's doom state before fallible work and clear it
only after success. A plain returned `txn.item()` proxy would require a new
completion/error/unwind protocol. It is not part of this migration.

### Current protocol correction: datatype locking and publication

The original shape-only migration preserved deferred preflight and held all
write guards through installation. The separately approved native protocol
alignment deliberately supersedes that behavior, not merely its naming.
`TObject::lock` and `LockContext::try_lock` / `try_unique_lock` synchronously
acquire in access order. Core owns partial guards and releases in reverse order;
there is no globally sorted deferred lock plan.

Local `TxnCell` and `TxnCounter` may publish/unlock from install through
`InstallContext::try_publish_unlock`. Aliased physical locks defer publication
until every dependent mutation is ready. Table locks remain held, and retained
participants hold all locks through terminal finish, including reference
datatypes. No universal all-writes-held local guarantee remains. The current
contract preserves truthful irreversible/unknown outcomes and guard lifetimes
without reproducing unsafe C++ lifetime assumptions.

## Historical shape-only preservation rules

- No new heap allocation, payload copy, Arc clone, shared atomic, or dynamic
  dispatch in an existing hot path solely to support the new interface.
- No mandatory `Box<dyn TObject>`, no one-box-per-item representation, and no
  second object/proxy wrapper. The object trait remains statically typed.
- Preserve typed batch pooling, exact-binding capability checks, direct-token
  ownership/injectivity proofs, concrete singleton dispatch, and terminal-read
  typestate. No routing optimized operations through the generic fallback.
- Preserve core-owned guards, reverse-prefix release, certification cuts,
  checked version exhaustion, commit-ID policy per writing binding, and definite
  versus indeterminate outcomes. The original all-writes-held local publication
  requirement is superseded by the current protocol correction above.
- Preserve borrowed payload retirement, quarantine, explicit pre-ACK scratch
  recycling, stable registry allocation locality, and detached-owner charges.
- Do not change opacity support, conflict granularity, scan witnesses, own-
  publication repair, RYW, native sealing/lifetime, or worker admission limits.
- The shape-only migration excluded TLS transaction lookup, callback reentry,
  unrestricted `&mut Transaction` during commit, new distributed protocol,
  durable ACK and engine-default changes. Native distributed protocol alignment
  is now separately approved; this historical exclusion does not reject it.
- No benchmark-only shortcuts, weaker assertions, fake native ABI, changed work,
  reduced correctness checks, or favorable build/noise selection.

## Historical staged execution and pacing (closed; not restart instructions)

Advance by evidence, not by elapsed time or a desire to finish the rename.
Each stage is one reviewable change boundary. Preserve an immutable
pre-migration Rust baseline throughout. Early smokes do not substitute for
final qualification.

**User-approved execution-order amendment (2026-09-28):** proceed with source
migration while retaining every final gate. Immutable R0/C0 artifacts,
baseline correctness evidence and working collector controls permit the
vocabulary and module-organization stages to proceed while the shared host
blocks admissible performance capture. Run focused correctness checks and
retain each source/build boundary before advancing. Defer, do not waive,
baseline timing and each stage's R0 comparison; retain S1 separately so later
regressions can be attributed to vocabulary versus module organization.
Every originally required workload, profile, correctness check, quality
threshold, bounded repetition rule and performance acceptance criterion
remains mandatory before final acceptance. No pooling of failed campaigns,
silent budget reset, foreign-process modification or premature performance
qualification is authorized. A correctness failure or confirmed regression
still stops advancement.

### Stage 0: Freeze contracts and establish a reproducible baseline

1. Record the exact current source snapshot, including existing user changes.
   Do not reset or discard unrelated work to obtain a convenient baseline.
2. Freeze the symbol/ownership map in this plan and enumerate consumers,
   compile-fail fixtures, mutation selectors, docs, and benchmark import paths.
   Record type/record footprints and selected commit/ownership mechanisms.
3. Establish the measurement recipes and close the runner/probe gaps below.
   Every measurement-only change precedes baseline builds and is held identical
   for both Rust variants. Capture runner/probe sources with results; historical
   scores or a script path under `/var/tmp` are not a reproducible baseline.
4. Freeze the harness, lockfile, toolchain, compiler/linker, native archives,
   Docker image identity, features, allocator, build flags, and CPU policy.
   Build and retain **R0** (current production Rust implementation) and **C0**
   (frozen C++ reference) before changing Rust API names. Separate target
   directories and explicitly selected engine features prevent contamination.
5. Run the applicable baseline correctness gates and raw performance samples.
   Known unsupported modes stay unsupported; a missing required reference driver
   is a failure, not an accepted skip. Predeclare the acceptance analysis and
   bounded repetition policy before candidate timing.

**Exit:** immutable R0/C0 artifacts and provenance; runnable per-workload recipes;
predeclared filtering, repetition, and non-regression rules; a complete migration
census. No production implementation changes belong to this stage.

### Stage 1: Canonical vocabulary with unchanged representations

1. Rename the selected trait, proxy, owned-item, typed-batch, and cleanup-view
   symbols in the target table. Keep `Transaction` and registration identity
   unchanged. Migrate every production adapter, integration, test fixture, and
   selected callback call site in the same source cutover.
2. Rename only the object/proxy methods listed in the table. Preserve argument
   types, return types, bodies, callback order, error conversion, `inline`
   attributes, visibility, capability selection, allocation ownership, and field
   ordering. Do not rename an unrelated `finish` or `check` by spelling alone.
3. Keep the current module/file locations for this stage so vocabulary changes
   can be measured separately from module-path/code-placement changes.
4. Update affected API documentation and compile-fail fixtures. Preserve tests
   of borrow, typestate, identity, and failure behavior; do not add tests merely
   to assert the new spelling. Mutation selectors must still change the intended
   operation and fail their negative control, not silently match zero locations.
5. Run focused core/reference/native/integration checks and the early performance
   sentinels below after all callers are migrated. Inspect optimized code for a
   changed allocation, indirect-call, atomic, or copy path rather than demanding
   byte-identical binary hashes after symbol/debug metadata changes.

**Exit:** one canonical vocabulary, no old exported aliases or unmigrated
callers, preserved layouts/ownership and hot-path work, passing focused gates.
This stage must not change transaction outcomes or the supported capability set.

**Current S1 evidence, not stage acceptance:** the vocabulary, current API
documentation, and ownership/typestate fixtures are migrated; module locations
remain unchanged. Debug/release core-reference-history checks with default/all
features, both doctest profiles, strict API documentation, and the native/cache
integration aggregate passed. The qualification archive retains the eight-gate
receipt at `correctness/S1/1790779329528668766/result.json`. Five regenerated
diagnostic snapshots preserve the same Send/Sync and lifetime errors across all
19 UI fixtures.

`S1-01-canonical-vocabulary` stopped at the unchanged shared-input identity gate:
host OpenSSL/libevent upgrades changed frozen dependency bytes. The attempt is
closed, not finalized; clock restoration was exact. Isolated Docker controls
then verified all 479 retained dependency paths using 24 read-only file mounts,
actual crypto/TLS/event execution, pre-launch rejection of a corrupted private
copy, and unchanged host libraries. See
`validation/R0-12-frozen-library-controls-01/result.json` in the archive.
No reference artifact, host library, workload, or acceptance threshold was changed.

`S1-02-canonical-vocabulary` subsequently passed the original freeze checks and
retains 17 binaries with five attestations; clock restoration was exact.
Its finalized receipt SHA-256 is
`7eaf0f82ad75962c5b29bc6fd2a19e6c2eea7a52e692fa80a11f01e860f850f2`.
The actual release and ThinLTO binaries each passed all 90 functional scenarios,
including engine/logged/cache operations, mixed transactions, native scans,
payload transitions and 32-worker execution. Receipts are
`smokes/1790787106672011714-S1-release.json` and
`smokes/1790787439900733659-S1-thin.json` in the archive. These are functional
smokes, not accepted timing samples. The native/cache integration gate also
passed again with the exact retained library inputs.

All 16 private fixture size/alignment roles match R0 in release and ThinLTO.
The inspected general commit, cleanup and frame-recycling paths add no
allocation, copy, atomic or dispatch work. ThinLTO eliminated the standalone
`recycle_frame` symbol in R0; its failed default-selector diagnostic is retained,
and explicit surviving commit/abort callers were collected and inspected.
See `validation/S1-layout-code-attribution-review.json` for the bounded review
and its raw evidence. These inspections do not establish universal code-generation
parity or qualify timing. All deferred S1/R0 sentinels and final performance gates
remain mandatory.

`S1-reserved-01` subsequently stopped at the release engine sentinels: all six
accepted blocks at each of 1, 8, and 32 workers show a confirmed 5.4–5.9% loss
against immutable R0, with every upper 95% bound below 0.97. All 45 release
allocation samples match R0 request and byte totals exactly. That campaign is
closed; ThinLTO and core sentinels were not reached.

Attribution found unchanged native leaf-lookup bytes and matching executed
instruction counts in the selected lookup and transaction paths under
simulation, but higher hardware instruction-cache misses. The simple LRU cache
model predicts the opposite aggregate miss direction and is explicitly **not**
evidence of the actual hardware cause. Hardware decoder/opcache counters and
the bounded code comparison are retained, without claiming an exact cache
conflict or a universal code-generation equivalence.

A private S1-derived experiment changes only the existing private heterogeneous
fallback from `#[inline]` to `#[inline(never)]`; selected public protocol methods,
bodies, layouts, allocation ownership, and phase authority remain unchanged.
A fresh control first reproduced frozen S1 byte-for-byte. Two engine dispatcher
specializations shrink from 5,555 to 2,942 bytes. General-path helper calls use
fixed ELF relocations rather than trait-object selection, but still add a call
boundary that needs mixed-path qualification. All 90 unchanged release
functional scenarios pass, including materialization, reuse, mixed operations,
and 32-worker execution. Two single-process hardware event sets show candidate
cycles below R0; these are **nonqualifying diagnostics**, not accepted timing
blocks or permission to reset the failed campaign.

The same private attribute change is now applied to the final source tree.
Fresh R1 source qualification and all original timing gates remain required.
The original R0/C0, S1, and R1 freezes remain immutable. Evidence is retained in
`validation/S1-outline-counters-01-audit.json`
and `validation/S1-outline-check-01-audit.json`, with contrary simulation
evidence in `validation/S1-cache-simulation-01-audit.json`. All completed
diagnostic CPU/LXD/clock scopes restored; no protected foreign process was
modified.

`S1-03-outlined-fallback` now retains 17 binaries and five attestations under
the unchanged freeze checks. A private mount namespace preserves the S1 module
boundary without replacing the live R1 tree; Docker verified all 400 retained
crate files read-only. Only the declared private attribute differs from S1-02.
All eight focused correctness gates pass. Release functional evidence is reused
only after all three exercised binaries match the new attestation byte-for-byte;
90 new ThinLTO scenarios also pass. All 16 fixture layout roles match R0 in both
profiles. Bounded commit, cleanup, and frame-recycling reviews find no unexplained
work changes; the general acquisition call boundary remains explicitly attributed.
See `validation/S1-03-finalized-boundary-audit.json`,
`validation/S1-03-runtime-layout-audit.json`, and the separate
`S1-03-{release,thin}-mechanism-review.json` records.

`S1-reserved-02` is now closed, inconclusive at the first eight-worker core
cell. All 45 release allocation samples match R0 request and byte totals.
Release engine sentinels pass at 1/8/32 workers with S1/R0 geometric means
1.031996/1.031265/1.026216 and one-sided 95% lower bounds
1.023048/1.025286/1.019640. The confirmed release engine loss is resolved for
this new source; the old failed source/campaign remains immutable.

All nine single-worker early core cells pass. The single-item, eight-worker,
read-only core cell has six accepted balanced blocks in ten attempts, a mean
ratio of 0.976775 and one-sided 95% lower/upper bounds 0.904347/1.055003.
Four attempts were excluded by the original SMT guard. All 24 accepted runs
verify state/counts with zero aborts; both arms vary. The 20 ms warmup/100 ms
measurement smoke lacks per-worker CPU-placement/off-CPU evidence, so no
cause is assigned and no accepted sample is retroactively excluded.

No additional rounds ran. Seventeen release core cells and all ThinLTO
allocation/engine/core sentinels remain unexecuted. CPU/LXD/clock restoration
is verified and the protected foreign process remains absent. See
`validation/S1-reserved-02-inconclusive-stop.json`. No final performance
acceptance is claimed. All original final R1 obligations remain.

**User-approved early core duration amendment (2026-10-03):** run one fresh
54-cell R0/S1 core campaign using the already-defined full-work settings:
1,000 ms warmup and 3,000 ms measurement. Preserve the previous 20/100 ms
campaign unchanged and do not pool any of its core samples into the new one.
Only the core timing selection changes; frozen binaries, cells, seeds,
placement, quality filters, six clean blocks,
60-attempt ceilings, confidence method and 3% budget remain unchanged.
Retain the completed release allocation/engine evidence and execute the
previously unrun ThinLTO allocation/engine gates. A failed or inconclusive
new sentinel stops acceptance without extra rounds. This remains an S1
stage comparison, not final R1 qualification. The predeclared inputs are
`validation/S1-core-full-duration-01-protocol.json` and
`validation/S1-core-full-duration-01-policy.json`.

The first integration attempt passed that new protocol file to `--protocol`.
The frozen-harness guard rejected it before any block or benchmark process;
all scopes restored. This consumes no statistical sample budget. Preserve
`validation/S1-core-full-duration-01-preflight-stop.json` rather than treating
the failed preflight as timing evidence or weakening the source check.

The corrected integration retains the guarded collector and base protocol
byte-for-byte. `validation/full_duration_core.py` separately records its source
and calls the existing `config_for` with a copied namespace selecting full
work for core only. The outer collector remains nonfinal S1 smoke mode;
provenance checks, sampling, quality guards, analysis and budgets are unchanged.
The additional selector is explicit measurement-orchestration code, not
represented as historical frozen source. Its retained receipt binds the actual
configuration and collector-result digest. The corrected pre-sampling command
inventory is `validation/S1-core-full-duration-02-policy.json`; the requested
1,000/3,000 ms timing and all 54 cells are unchanged.

The corrected selector executed 12 release core cells with 288 quality-accepted
benchmark processes; all verified their configuration, state and counts.
The first 11 cells pass. This includes the formerly inconclusive read-only
eight-worker cell, whose full-duration lower 95% bound is 0.970848; its old
short-duration result remains unchanged.

The next cell, one item/eight workers/100% writes, is inconclusive: S1/R0 mean
0.998170, one-sided 95% lower/upper bounds 0.960919/1.036866, six clean balanced
blocks in six attempts. No extra rounds ran and no code regression is inferred.
The campaign is closed; 15 release and 27 ThinLTO core cells plus ThinLTO
allocation/engine remain unexecuted. All host scopes restored and the protected
process stayed absent. See
`validation/S1-core-full-duration-02-inconclusive-stop.json`.
The approved execution-order amendment permits independent fresh R1 source
qualification to proceed, but does not waive this unresolved comparison,
missing S1 coverage or any final performance gate. No further timing campaign
is authorized by this result.

Analysis of the existing 24 accepted processes found retry variability in both
arms: abort fractions span 0.28–14.98% in R0 and 0.21–18.01% in S1.
The benchmark retries the same materialized logical transaction without backoff;
its array locks and versions are per slot, not array-wide. The singleton
homogeneous path does not call the outlined ordinary-item fallback. These are
source and descriptive-data facts, not a scheduling or semantic cause.
See `validation/S1-full-core-closed-variance-attribution.json` and
`validation/S1-retry-mechanism-source-report.json`.

The user subsequently authorized one nonqualifying four-process R0/S1/S1/R0
diagnostic of the stopped cell, with scheduling and counter observations.
No new qualification campaign, pooled samples or extra statistical rounds were
authorized. The initial passive capability probe failed because `perf record`
could not map its declared 1,024-page buffer; the observed kernel allowance is
516 KiB per CPU. No benchmark launched, both binary identities remained unchanged,
and all CPU/LXD/clock scopes restored. The failure is retained at
`validation/S1-retry-diagnostic-01-preflight-stop.json`.
A separate corrected policy declares a 64-page buffer before execution.
Kernel limits, events, workload, quality filters and lost-event rejection are
unchanged; this is not a fallback that discards missing scheduling evidence.

The next passive preflight mapped that buffer, but exposed two control-integration
defects: perf's acknowledgement includes a terminal NUL, and perf can preserve
the parent's requested SIGINT as its exit status after flushing output.
The corrected operator validates the exact five-byte acknowledgement, records
requested termination separately, and still rejects unexpected exits, missing
counters, lost records and quality failures. Both earlier attempts launched
zero benchmarks and restored all scopes; their artifacts remain unchanged.

`S1-retry-diagnostic-03` then completed exactly four frozen R0/S1/S1/R0 launches.
All configuration, state/count, quality and raw-artifact checks pass. Abort
fractions are 1.81%, 14.39%, 18.58% and 10.17%; whole-process migration counts
are 26, 16, 19 and 15. Each capture identifies eight worker TIDs plus main,
with observed CPUs inside the declared pool. Maximum matched worker deschedule
gaps are 6.010, 7.218, 6.496 and 6.011 ms. These are **whole-process scheduling
observations**, not transaction latency or measured-phase off-CPU fractions.
There is no phase marker or abort-reason trace; coexistence of retry and
scheduling variability does not establish causality or a semantic regression.
See `validation/S1-retry-diagnostic-03-completed-audit.json`.
All CPU/LXD/clock scopes restored and the protected process remained absent.
No diagnostic process is a qualification sample. The closed S1 confidence result
and all missing comparisons remain unchanged; further qualifying timing still
requires an explicit protocol decision.

**User-approved fixed-precision amendment (2026-10-03):** predeclare one fresh
54-cell S1/R0 core campaign with **24 clean balanced blocks per cell**, retaining
the 60-attempt ceiling, 1,000/3,000 ms timings, seeds, placement, quality filters,
Student-t confidence method and one-sided 95% lower-bound requirement of 0.97.
The original collector already permits this count for nonfinal S1 runs; its
source and the final R1 six-block policy remain unchanged. A separate selector
and operator bind the declared count and full-work configuration.
Analyze each cell only after all 24 clean blocks; a failed or inconclusive gate
stops the campaign without additional rounds. Do not pool either closed core
campaign or any instrumented diagnostic into it. Reuse only the completed
release allocation/engine evidence and execute the missing ThinLTO lanes under
their original policies. All final R1 obligations remain mandatory.
The pre-sampling inventory is `validation/S1-core-fixed24-01-policy.json`;
authorization is retained in `validation/S1-core-fixed24-01-user-decision.json`.

**Fixed-24 outcome:** `ReservedS1Fixed24` stopped at release
`core/array/items=1/workers=8/write_percent=0`. The preceding nine single-worker
cells passed. This cell collected exactly 24 clean blocks in 24 attempts with
no exclusions: S1/R0 geometric mean **0.9920691131**, one-sided 95% lower bound
**0.9696824285**, below the required **0.9700000000**. The corresponding separate
one-sided 95% upper bound is **1.0149726305**. This is inconclusive, not a
demonstrated greater-than-3% regression; proximity to the threshold does not
permit rounding into a pass. No extra rounds ran.

All 10 collected cells completed their declared 24 clean blocks: 240 balanced
blocks and 960 accepted processes with verified state, configuration, counts and
quality. Six excluded attempts, all in the single-worker singleton 50%-write
cell, remain retained with their original CPU/SMT reasons. All 20 collection and
analysis commands exited zero in pinned Docker; the enclosing operator stopped
on the statistical gate, not a workload or analysis error. The selector, policy,
helpers and frozen executed binaries retained their declared hashes.

The stopped read-only cell's 96 processes all reported **zero aborts**, unchanged
final sum 4,999,950,000 and measurement durations near three seconds. The earlier
all-write retry diagnostic therefore does not attribute this read-only
variation; no source, scheduling or hardware cause is established.
CPU reservation, LXD and clock scopes restored successfully. The protected PID
identity remained absent before, during configuration and after the scope.
The immutable stop audit is
`validation/S1-core-fixed24-01-inconclusive-stop.json`.

This campaign remains closed and unaccepted. Its remaining **44 core cells**
(17 release, 27 ThinLTO), plus ThinLTO allocation/engine, were not executed.
Earlier completed release allocation/engine evidence was reused by identity;
no prior core or diagnostic samples were pooled. All final R1 performance gates
remain mandatory and unexecuted. Do not restart with successively larger counts
or silently waive this intermediate gate; further timing requires an explicit
protocol/scope decision.

**Approved read-only mechanism profile (2026-10-04):** execute exactly four
nonqualifying frozen R0/S1/S1/R0 launches of the stopped read-only cell, with the
same 1,000/3,000 ms timings, seed, keyspace and eight-CPU pool. Observe
task-inherited scheduling and userspace-cycle leaf samples at 99 Hz, alongside
the existing whole-process counters. A single bounded owned child/thread
capability probe must establish the instrumentation before any benchmark launch.
No system-wide recording, per-worker affinity intervention, kernel-setting
change, production edit or qualifying sample is authorized.
The separate frozen-source/emitted-code review may identify shared-state
operations, not infer a causal explanation from zero aborts or timing alone.
All observations include whole-process phases; do not divide counters by
measured-only commits or claim measured-phase off-CPU time. Original quality,
loss and counter-running checks apply, with no retries or replacement launches.
The authorization is `validation/S1-readonly-profile-01-user-decision.json`;
all closed campaigns and S1/final R1 acceptance gates remain unchanged.

**Read-only profiler preflight integration (2026-10-04):** the sole owned
capability probe completed, but operator 01 stopped before any frozen launch:
`perf --delay=-1` emitted a separate `dummy:u` tracking attribute alongside
`cycles:u`. Revision 02 correctly decoded their event IDs, then its offline
proof exposed three kernel-mode sampled IPs. The non-precise sampler's
`exclude_kernel` setting constrains counted events, not a guarantee about
sampled IP privilege; arbitrary skid is permitted by `precise_ip=0`
([Linux API](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)).
Their cause is not established. Revision 03 preserved these modes and periods,
then stopped because the pinned `perf script --fields sym` emitted blank lines.
The already-retained complete IP/symbol/DSO report contains all samples.
All failed operators, policies and outputs remain retained and unchanged.

Revision 04's pinned-Docker **offline** proof passed against the same capture:
30 samples (27 user, 3 kernel), 420,322,655 total adaptive period, including
2,711 for the kernel-mode IPs; 20 resolved and 10 unresolved symbols. Every
TGID/TID/CPU/IP count and period sum matched the binary decoder. Main and
inherited-thread scheduling and both event-role ID sets were verified.
No second probe, recording or benchmark was launched during these corrections.
`validation/S1-readonly-profile-04-decoder-proof.json` retains the proof;
revision 04 was declared against the original four-launch allowance with unchanged
workload/recording/counter commands and quality gates. Kernel-mode entries stay
separate from Rust attribution; no acceptance threshold changes.

**Read-only profile quality stop (2026-10-04):** revision 04 released only its
first R0 process, then closed under the unchanged no-retry rule. The process
exited zero and verified 35,858,749 commits/attempts, zero aborts and final sum
4,999,950,000, but SMT sibling CPU 74 had utilization
`0.012526096033402934`, above the `0.01` ceiling. It is **quality-rejected**.
No S1 profile exists; the remaining S1/S1/R0 launches were not released.
No replacement or automatic continuation is authorized by that closed invocation.

Instrumentation completed: 2,935 samples reconciled by identity/count/period
with the complete symbol report (2,889 resolved, 46 unresolved), scheduling for
main plus eight inherited TIDs, all five counters at 100% running, and no
recorded loss/throttling. All 34 raw artifact hashes matched. Kernel-mode IPs
remain separate (32 samples); all counters/profiles cover the whole process,
not just measured commits. CPU reservation, LXD and clocks restored, and the
protected PID identity was absent before/configured/after.
The audit is `validation/S1-readonly-profile-04-quality-stop-audit.json`.

Bounded observations from this **rejected R0 only**: the array `get` closure
accounts for 25.7827% of inherited USER adaptive sample period, with its largest
cluster immediately after an Arc refcount increment. Non-precise skid prevents
assigning that cluster precise instruction/cache-coherence cost. The two exact
488-byte R0/S1 closures have matching seven lock-prefixed instruction sites,
but different other bytes/targets; neither whole-code nor performance
equivalence follows. Frozen source confirms no OCC commit-ID allocation or
array lock acquisition for this read-only path; ArcSwap debt/refcount traffic
and generic preflight/check/cleanup remain. The worker wrappers differ in
size/layout. `validation/S1-readonly-profile-04-bounded-attribution.json`
retains the source, emitted-code and offset evidence. Two R0 threads also show
large whole-process switch gaps, not a proved measured-phase runnable delay.
Without an accepted R0/S1 profile pair, none of this explains the earlier S1
confidence-bound result. S1 and final R1 qualification remain incomplete.

**Authorized unused-launch continuation (2026-10-04):** the user selected
“Run only the unused three.” Revision 05 may release S1/S1/R0 once, separately
archived, keeping total physical frozen launches at four including rejected
R0 from revision 04. That rejected run remains excluded; no replacement,
fresh probe, pooling, clean-balanced-quartet claim or qualification sample is
authorized. Workload, acquisition/decoding, CPU pool, timings and all quality
thresholds stay unchanged. Any failure stops this continuation. Its whole-run
LXD journal/restart check covers the three-run envelope.
`validation/S1-readonly-profile-05-user-decision.json` and
`validation/S1-readonly-profile-05-policy.json` bind this revised scope.

**Unused-launch continuation completed (2026-10-04):** revision 05 released
exactly S1/S1/R0 and exited zero. All three processes passed the unchanged
functional, quiet, whole-process quality and instrumentation checks: zero
aborts, attempts equal commits, verified final sum 4,999,950,000, nine observed
TIDs each, all five counters at 100% running, and no recorded loss/throttling.
All 9,050 samples reconciled with symbol-report identities/counts/periods and
all 102 raw artifact hashes matched. CPU reservation, LXD and clocks restored;
the protected PID identity was absent before/configured/after. No new probe
or replacement ran. The original allowance is exhausted: four physical
launches total, one rejected R0 followed by these three accepted captures,
**not a clean balanced quartet**.

Separate descriptive observations (counters and scheduling cover the whole
process; throughput is the benchmark's measured-interval output):

| Run | Throughput, Mtxn/s | Task-clock, s | Worker preempted switch-outs | Summed matched worker switch gaps, s |
| --- | ---: | ---: | ---: | ---: |
| S1 first | 10.8455 | 27.7096 | 1,460 | 4.5387 |
| S1 second | 12.4512 | 31.3511 | 180 | 0.8729 |
| R0 | 12.6863 | 30.7001 | 397 | 1.5395 |

The slower S1 capture coincides with substantially more descheduling
observations. These gaps are not measured-phase runnable delay and do not
prove causation for the earlier confidence-bound miss. All three profiles
show the same major read/commit/version-check/cleanup groups. The array `get`
closure represents respectively 26.653%, 27.718% and 25.988% of inherited USER
sample periods; dominant offsets are the same post-increment and
pointer-load-following sites. Statistical skid prevents precise attribution.
No evidence-backed production repair, noninferiority result or demonstrated
greater-than-3% regression follows; no old samples were pooled.

`validation/S1-readonly-profile-05-completed-audit.json` and
`validation/S1-readonly-profile-05-attribution.json` retain the receipts and
per-run analysis. The authorized mechanism investigation is complete.
The fixed24 read-only cell remains inconclusive; 44 untouched core cells and
ThinLTO allocation/engine coverage remain unexecuted, as do final R1 performance
matrices. Their acceptance gates remain unchanged.

**Authorized untouched S1 coverage (2026-10-04):** the user selected
“Collect untouched S1 coverage.” A new immutable campaign declares only
17 untouched release core cells and all 27 ThinLTO core cells, with exactly
24 clean balanced blocks per cell, the unchanged 60-attempt ceiling and
1,000/3,000 ms work. The ten closed release cells are excluded by identity;
their nine passes and one inconclusive result remain unchanged. No old samples,
diagnostic captures or new cells are pooled.
The existing lane order is retained: release core, ThinLTO allocation and
engine, then ThinLTO core. Original quality/confidence rules and
stop-on-failure/inconclusive behavior remain in force; no extra rounds.
Release allocation/engine evidence is reused only after its existing identity
and acceptance checks. ThinLTO allocation collects 45 samples for comparison
with the frozen R0 reference; engine uses the original 1/8/32-worker sentinels.
`validation/S1-untouched-fixed24-01-user-decision.json` and
`validation/S1-untouched-fixed24-01-policy.json` bind this scope. Collecting
missing coverage does not waive the closed read-only cell or final R1 gates.

**Untouched campaign quality-budget stop (2026-10-04):** the first new cell,
release singleton / 8 workers / 50% writes, exhausted its unchanged 60-attempt
budget with **23 clean blocks**, short of the required 24. The campaign closed;
there was no 61st attempt, continuation, confidence analysis or acceptance
claim. This is incomplete quality coverage, not a statistical regression result.
The 60 attempts comprise 23 accepted complete blocks, 36 rejected complete
blocks and one opening-guard rejection that launched no process.
All 236 physical runs (118 R0 and 118 S1, internally labeled `R1`) returned zero
and passed configuration, count and final-state checks. All raw benchmark
outputs were independently reconciled with the retained measurements;
92 process observations belong to accepted blocks and 144 remain excluded.
One opening and 17 between-process quiet checks failed; 35 process windows
breached the SMT utilization rule. Whole-process sibling CPU 64 utilization
peaked at 21.7865%, versus the unchanged 1% ceiling. Reason counts overlap;
the audit retains their exact breakdown. No categorized competing runner was
reported, which does not establish absence of host activity or its cause.

`validation/S1-untouched-fixed24-01-quality-stop-audit.json` records the stop,
944 raw-artifact hashes, verified frozen/input/history identities and restored
CPU/LXD/clock scopes. The protected PID identity remained absent throughout.
A later passive host observation changed no controls or foreign processes;
it cannot attribute the earlier exclusions. The ten previously closed core
cells retain their nine passes and one inconclusive result. In addition to
this new incomplete cell, 16 release and 27 ThinLTO core cells remain
unattempted under the fixed-24 policy. ThinLTO allocation/engine and all final
R1 performance qualification remain unexecuted. No source repair, extra
rounds, pooling, filter change or gate waiver was made.

**Authorized passive isolation diagnostic (2026-10-04):** the user selected
“Diagnose host isolation”: one 60-second CPU/thread/IRQ observation under the
existing reversible reservation, LXD pause and fixed-clock controllers.
The observer stays on CPU 63; procfs counters and process/cgroup metadata are
buffered in memory, with `pidstat` output buffered in an owned tmpfs directory.
There are no benchmark launches, capability probes, foreign-process changes,
IRQ/workqueue-affinity changes or qualification samples. The later passive
window cannot establish the cause of historical exclusions or certify future
quietness. Scope and limits are bound by
`validation/S1-host-isolation-01-user-decision.json` and
`validation/S1-host-isolation-01-policy.json`.

**Passive isolation diagnostic completed (2026-10-04):** the single authorized
window ran for 60.067483 seconds and retained 60 procfs snapshots, plus 593
thread rows across 28 `pidstat` reporting intervals. Both observers shared
CPU 63, which was 94.4324% busy; this is not zero-overhead observation.
The pidstat child was stopped at the deadline, not relaunched.
All CPU/LXD/clock scopes returned zero and verified restoration; the protected
PID identity remained absent. No benchmark or capability probe ran.

All 9,830 observed `user.slice` process records (171 PID/start-tick identities)
had main-thread affinity within the declared housekeeping pool, and all 60
effective-mask snapshots matched it. That reservation does not encompass
system services or kernel workers: thread reports on the focus CPUs included
`redis-server.service`, `mysql.service`, `janus.service`, resolver/journal
services and RPC/writeback workers. Last-CPU fields are not complete residency
traces. Network/storage interrupts also reached reserved CPUs; recorded IRQ
affinity endpoints were unchanged.

The historical CPU 64 pressure was **not reproduced**: that sibling accumulated
zero accounted busy ticks in this later passive window. The eight focus
siblings' largest whole-window busy fraction was about 0.10%; across the full
reserved pool it was 0.3334% on CPU 78. Short sampled intervals still reached
4% on CPU 82. These are diagnostic observations, not qualification windows or
guarantees of future quietness. The capture establishes the reservation's
coverage boundary, but no causal repair for the closed campaign.
`validation/S1-host-isolation-01-completed-audit.json` retains the detailed
CPU/thread/IRQ evidence, source identities, observer limits and restoration
receipts. All closed-cell and missing-performance blockers remain unchanged;
the one-window authorization is exhausted.

**Authorized loaded-block attribution (2026-10-05):** the user selected
“Trace one loaded block”: one non-qualifying attempt of the closed release
singleton / 8 workers / 50% writes cell, permitting at most R0/S1/S1/R0.
The frozen collector retains its full 1,000/3,000 ms work, configuration,
benchmark I/O path, quality rules and fatal-error stops. A separately declared
one-block/one-attempt diagnostic budget permits no replacement block; an
opening-guard rejection can consume it without launching a process.
Host CPU/thread/IRQ observation surrounds that one collector invocation,
using the proven passive-read helpers on CPU 63 and buffering its own output
until collection ends. Existing CPU/LXD/clock controls are unchanged.
Rejected data is retained only for attribution. No acceptance analysis,
pooling, capability probe or new host-isolation change is authorized.
`validation/S1-loaded-block-01-user-decision.json` and
`validation/S1-loaded-block-01-policy.json` bind this scope; neither closed
qualification cell gains samples or changes disposition.

**Loaded-block attribution completed (2026-10-05):** all four frozen
R0/S1/S1/R0 launches ran with the full declared work and passed independent
configuration, count and final-state checks. All four process-window quality
guards passed. The sole block was nevertheless rejected: the quiet guard
between the second S1 and final R0 reported CPU 74 at 87.562189% idle, below
95%. The collector's nonzero exit and zero clean blocks are preserved.

The enclosing three counter intervals contain 25 I/O-wait ticks, one user
tick, 275 idle ticks and zero system/IRQ/softirq/steal ticks on CPU 74.
The frozen guard deliberately defines non-idle as `1 - idle / total`, including
I/O wait; this rejection is I/O-wait-dominated, not evidence of sustained SMT
execution saturation. Those intervals enclose rather than exactly match the
guard, so their counters are not prorated into its boundaries. The requesting
task, device and file remain unidentified. Storage/network interrupts and
journal/writeback/shim sightings do not establish causation; a post-run query
shows the archive on NFS, not the local journal device. The historical CPU 64
21.7865% guard value also means non-idle, but lacks retained per-category
counters and cannot retrospectively be attributed to I/O wait.

The observer retained 552 snapshots and 9,698 thread rows over 552.274 seconds,
including about 520.280 seconds before the first opening guard; the guarded
block through the final process occupied 28.664 seconds. CPU 63 was 96.4116%
execution-busy, so this is not zero-overhead measurement. All 91,432 sampled
`user.slice` main-process affinity records and 552 effective-mask snapshots
matched the reservation. Three IRQ affinity endpoints changed during capture;
their transition times and causal effects are unknown.

All CPU/LXD/clock scopes verified restoration; the protected PID identity
remained absent. `validation/S1-loaded-block-01-completed-audit.json` retains
the attribution, 16 raw sample-file hash checks, 74 input/provenance checks,
92 closed-history checks, observer hashes and restoration receipts.
No production repair, guard change, acceptance analysis or pooling occurred.
The one-block/four-launch authorization is exhausted. The closed fixed24
inconclusive cell, the 23-clean/60-attempt quality-exhausted cell, 43 untouched
core cells, missing ThinLTO allocation/engine coverage and all final R1
performance gates retain their prior dispositions.

**Authorized I/O-origin trace (2026-10-05):** the user selected “Trace I/O
origin”: one new non-qualifying R0/S1/S1/R0 block, retaining the original
collector I/O, full work, guards and fatal-error stops. No retry, pooling,
capability probe or new host control is authorized. Existing tracefs event
metadata and visible `io_schedule` symbols were read without opening events;
`kernel.sched_schedstats` and `kernel.task_delayacct` remain zero.

The declared observation uses 23 explicit global I/O tracepoints and two
scheduler tracepoints on the 16 worker/sibling focus CPUs. Two owned perf
sessions use CPU 63, monotonic timestamps and bounded RAM output, with kernel
callchains only for scheduler events. After frozen provenance checks, a private
tmpfs socket gates the first original quiet guard on both real sessions'
enable acknowledgements; subsequent guards check session health. Unavailable
tracing stops before benchmark launch. This setup is not a separate probe.
`validation/S1-io-origin-01-policy.json` binds the recipe and preserved inputs.
NFS/block events lack a guaranteed unique cross-subsystem request key, so
unmatched work, missing endpoints, losses and ambiguous ownership must remain
explicit. Neither closed qualification cell receives samples or changes status.

**I/O-origin diagnostic audited (2026-10-05):** exactly four full-work frozen
R0/S1/S1/R0 launches passed independent configuration, count, final-state and
all original process/quiet-guard checks. No rejection was reproduced. The
closest quiet guard was CPU 66 at 95.0248756% idle, above the unchanged 95%
requirement; this instrumented block is non-qualifying by its declared scope.

The trace records 18 CPU 66 switch-outs through
`io_schedule -> rq_qos_wait -> wbt_wait` during the guarded block, in an
unbound kernel worker performing local ext4 writeback. Ten lie within the
near-threshold quiet guard. The corresponding background writeback invocation
on `bdi 8:0` names cgroup inode 520332; a post-run metadata lookup resolves
that inode to `systemd-journald.service`. The captured journal process dirtied
two inodes whose post-run open-file metadata matches `system.journal` and
`user-2008.journal` on the local ext4 filesystem (`8:2`). Its PID/start-tick
identity remained unchanged across the lookup. No journal contents were read.
These observations identify a concrete writeback path outside the
`user.slice` reservation, not an individual block-request-to-file binding or
the original log-message producers. They do not retrospectively identify the
cause of the earlier CPU 74 rejection or historical CPU 64 peak. Separately,
each benchmark process had an observed NFS flush stack during process exit;
this is not evidence of STO hot-loop blocking.

Both actual perf sessions acknowledged enable and disable; all four guard
readiness checks succeeded and both children exited without forced kills.
The operator nevertheless returned nonzero because its strict shutdown rule
rejected both owned SIGINT return codes (`-2`). Those incomplete-lifecycle
receipts remain unchanged. Offline decoding independently validated both
file structures and reconciled all 988,047 I/O and 20,676 scheduler sample
records, with no recorded loss markers. Decoder field-selection warnings
remain retained; absence of loss markers is not a claim of universal event
coverage. The 60.609-second post-collector trace tail, including observer
archive serialization, is excluded from guarded-block attribution.

The CPU observer retained 839 snapshots over 840.679 seconds, including
809.442 seconds before the first guard; the guarded block occupied 28.664
seconds. CPU 63 was 93.5736% execution-busy. All 130,461 sampled `user.slice`
main-process affinity records and all effective-mask snapshots matched the
reservation. All CPU/LXD/clock scopes verified restoration; the protected PID
identity remained absent. `validation/S1-io-origin-01-completed-audit.json`
and `validation/S1-io-origin-01-attribution.json` retain the bounded findings,
30 explicit I/O-wait switch-outs and their stacks, decode evidence, 16 raw
sample-file hash checks, 107 input/provenance checks, 92 closed-history
checks and restoration receipts.

The one-block/four-launch authorization is exhausted. No production change,
new host control, guard change, acceptance ratio/CI, pooling or replacement
block occurred. Both closed qualification cells, 43 untouched core cells,
missing ThinLTO allocation/engine coverage and all final R1 performance
gates retain their prior dispositions.

**Authorized workqueue-reservation trial (2026-10-05):** the user selected
“Validate workqueue reservation”: one new non-qualifying R0/S1/S1/R0 block,
with the original full work, collector I/O and guards; no retries or pooling.
The sole new host control temporarily restricts global unbound kernel
workqueues to the existing 64 housekeeping CPUs, mask
`fffffa80,221a0912,fffffa80,221a0912`. It does not isolate IRQs, bound kernel
threads or independently scheduled kernel services, and can affect other
users' unbound kernel work. No individual foreign-process change is authorized.

`validation/workqueue_reservation_01.py` nests inside the existing CPU scope
and outside LXD/clock scopes. It persists the original mask before writing,
requires configured readback before child launch, and restores/verifies on
exit. An ordered `ExecStopPost` fallback attempts workqueue restoration before
CPU restoration even if one command fails. Unrelated external mask changes
are not overwritten. The operator records mask readbacks at every trace gate
and observer snapshot. Existing tracepoints and focus CPUs remain unchanged.

The new `io_trace_recorder_02.py` records raw signal statuses and recognizes
owned SIGINT shutdown only after successful enable/disable acknowledgements,
without premature exit, forced kill or prior error. This lifecycle verdict
does not prove data finalization or loss-free capture; offline decoding is
still required. The prior executed helper and failed receipts are immutable.
`validation/S1-workqueue-01-policy.json` binds the one-attempt/four-launch
budget and input identities. This trial does not reopen either closed
qualification cell or demonstrate statistical elimination of rare noise.

**Workqueue trial closed at admission (2026-10-05):** the requested global
mask was applied, verified and restored, but the sole opening guard rejected
CPU 64 at 86.9346734% idle against the unchanged 95% requirement. Zero
benchmark processes launched. The one attempt is exhausted; four unused
nominal launch slots do not authorize a replacement attempt.

Enclosing CPU 64 counter intervals contain 26 I/O-wait ticks and zero
user/system/IRQ/softirq/steal execution ticks. Within the failed guard, the
trace records two `io_schedule -> bit_wait_io -> jbd2_journal_commit_transaction`
switch-outs from PID 1550 (`jbd2/sda2-8`), with block writes to device `8:0`.
This independently scheduled filesystem-journal thread retained affinity
`0-127`; it is not an unbound workqueue and is distinct from systemd-journald.
Next-observed switch-in gaps were 14.993 and 252.234 ms, not uniquely measured
device latency. Counter intervals are not exact guard boundaries, and no
unique original file or dirtying producer is established.

All 604 workqueue-mask/effective-slice snapshots and 94,760 sampled
`user.slice` main-process affinity records matched the reservation. Observed
writeback events during the guard ran on housekeeping CPUs 25, 29, 68 and
121; no unbound-worker switch-out was observed on the focus CPUs in that
short guard. However, eight `kworker/u256` identities retained `0-127`
affinity in 3,409 snapshot rows. Global mask readback therefore is not proof
that every kernel-worker affinity is restricted, nor that independent kernel
services or IRQs are isolated.

Both new recorder lifecycles completed with retained owned-SIGINT statuses
`-2`, no forced kills and successful acknowledgements. Offline decoding
reconciled all 513,896 I/O and 17,874 scheduler sample records, with no recorded
loss markers. The 44.713-second post-collector trace tail is excluded from
guard attribution. The host observer ran for 605.556 seconds, including
599.930 seconds before the opening guard; CPU 63 was 96.0770% execution-busy.
No loaded benchmark behavior or performance acceptance was exercised.

CPU/workqueue/LXD/clock restoration all verified; ordered `ExecStopPost`
workqueue/CPU restoration checks also returned zero. The protected PID
identity remained absent. `validation/S1-workqueue-01-completed-audit.json`
and `validation/S1-workqueue-01-attribution.json` retain the failed guard,
kernel stacks, 145 input/provenance hash checks, 92 closed-history checks,
observer/trace hashes and restoration receipts. No production change,
guard change, retry, pooling or acceptance analysis occurred. All closed
qualification outcomes and missing performance coverage remain unchanged.

**Read-only guard-accounting review completed (2026-10-05):** the user selected
“Review guard accounting,” authorizing source review and offline analysis only.
No guard, host control, benchmark or historical eligibility changed.
`run_sto_tpcc_compare.py:618-643` deliberately retains idle alone, while
`run_mako_engine_compare.py:65-72` retains idle plus I/O wait. The engine
campaign also applies the stricter shared outer `Quality`/`Monitor`, so both
definitions currently constrain it. All four reviewed root driver files still
match their R0-12 frozen source hashes.

The executed offline replay covers ten retained observer intervals, not exact
guard endpoints. Across their enclosing aggregates, non-idle versus
execution-plus-steal was 8.6379% versus 0.3322% on the loaded diagnostic's
CPU 74; 3.3223% versus 0.3322% on the I/O trace's CPU 66; and 5.7650% versus
0% on the workqueue trial's CPU 64. These are arithmetic comparisons, not new
sample verdicts. Linux's versioned `/proc` documentation warns that per-CPU
I/O-wait attribution is unreliable and its counter can decrease.

`validation/guard-accounting-review-01-proposal.json` recommends an explicit,
not-yet-approved `shared-host-worker-smt-execution-v2` policy: count
user/nice/system/IRQ/softirq plus steal against the existing 5% preflight and
1% sibling ceilings; retain true idle and I/O wait separately without treating
I/O wait as execution. Use inclusive integer threshold comparisons and reject
invalid counter evidence rather than clamping it. This changes eligibility
and relinquishes implicit I/O quiescence; it does not prove storage/network
interference absent. Core/native still lack engine-style per-worker off-CPU
and PMU coverage.

Any adoption requires separate approval, bounded Docker guard validation and
a new immutable measurement boundary before a predeclared fresh matched
campaign. Old archives and both closed cells remain unchanged; no pooling,
selective failed-cell rerun or relaxation of the 0.97 lower confidence bound
is proposed. The read-only CI miss is not repaired by this accounting review.

**Execution-accounting implementation and validation approved (2026-10-05):**
after resuming the interrupted session, the user selected “Implement and
validate policy.” This authorizes the explicit execution-plus-steal policy,
Docker contract checks, and one attempt each of four real guard scenarios:
quiet, worker contention, SMT contention, and owned I/O wait. It does not
authorize a qualification campaign, retries-to-pass, new host controls or
historical reclassification. The decision is retained in
`validation/guard-execution-v2-01-user-decision.json`.

The implementation uses `scripts/sto_cpu_accounting.py` and a separate
`guard-v2/` collector/protocol family; executed collectors remain immutable.
The engine CLI names the metric `--max-sibling-contention`. Standalone
point/TPC-C idle-only contracts are unchanged. The runtime recipe retains
the two-second preflight and original monitor cadence; each monitor wraps
one six-second sleeping child, not a benchmark. Owned contention actors
target worker CPU 0 or its actual SMT sibling 64. The I/O case uses only an
owned local direct-write file, bounded to 20 seconds and 128 MiB; insufficient
I/O-counter contrast is inconclusive, not a reason to repeat it. Existing
CPU/LXD/clock scopes must restore. Global workqueue masks, IRQs, journal
threads and individual foreign processes are not changed.

**Execution-accounting implementation and bounded validation closed
(2026-10-05):** the shared helper now emits named counters, boot identity,
clock-tick frequency and monotonic read brackets under
`linux-cpu-accounting-v2`. Execution plus steal excludes I/O wait; inclusive
integer comparisons retain the 5% preflight and 1% sibling ceilings. Invalid
counter evidence fails closed. The engine and new `guard-v2/` family use this
contract; standalone point/TPC-C idle-only behavior and executed collectors
remain unchanged. No Rust/C++ production source changed in this scope.

The pinned Docker contract run passes all 42 tests and 11 separate-process
CLI import checks. The first run exposed an existing Python 3.12 argparse
edge: an explicit `--worker-cpu-start 0` escaped the mutually exclusive
placement check because it equaled the parser default. The driver now
normalizes the default after parsing. Existing regression cases and actual
compare/variants CLI invocations reject both placement flags together.
Both contract attempts and their source bytes are retained.

The first host setup stopped before any runtime container, guard window or
pressure actor: Docker's lowercase missing-object diagnostic did not match
the wrapper's text check. Its scopes restored. A separate corrected wrapper
uses structured container listing and was checked before launch. This was
setup repair, not repetition of a guard scenario.

The single actual runtime launch executed all four cases once:

| Case | Observed behavior | Declared outcome |
|---|---|---|
| Quiet | Worker/sibling execution plus steal 0%; both guards accept | Pass |
| Worker contention | Worker CPU 0 at 100%; preflight rejects; sibling monitor accepts | Pass |
| SMT contention | CPU 64 at 100%; preflight and sibling monitor reject | Pass |
| Owned I/O wait | CPU 64 monitor records 4 I/O-wait ticks of 612, 0 execution-plus-steal ticks; both guards accept | Admission contrast inconclusive |

The I/O actor completed 37 one-MiB direct synchronous writes on the owned
local file. Accounted I/O wait is separately visible at 0.6535947712%, but the
old 1% sibling ceiling also accepts that interval. The stronger predeclared
contrast therefore did not pass; it was not weakened or retried. Twenty-four
write calls wholly inside the monitor brackets occupied 1.343482184 seconds
of syscall wall time. That is not per-CPU I/O-wait time or proof of unique
attribution: Linux's aggregate counter is not per-task I/O latency.

The offline audit independently recomputes all 16 counter intervals and
checks 27 raw artifacts, 41 executed source identities, 92 closed-history
identities and 17 original collector identities. All seven children were
reaped with exit zero and no forced kills; the owned I/O directory and
container were removed. CPU properties/kernel mask and clocks restored
exactly; LXD's declared unit-state fields restored. The protected identity
remained absent, and the global workqueue mask did not change.

`validation/guard-execution-v2-01-completed-audit.json` records the closed
outcome and restoration evidence (SHA-256
`a7acb231276dec386a7822242be49e232433713e353a12441cfff5e5c83a0649`).
`validation/guard-execution-v2-01-source-snapshot-02.json` retains the exact
41-source closure. The implementation and authorized four-case execution
are complete, but the declared I/O admission contrast remains inconclusive.
No benchmark, qualification sample, historical reclassification or fresh
measurement boundary was produced. Any further runtime control or fresh
qualification needs a new explicit scope; the old CI miss, exhausted cell
and missing performance coverage remain unchanged.

**Stronger I/O-only control approved (2026-10-05):** the user selected
“Authorize one stronger I/O control.” The separate
`validation/guard-execution-v2-io-02-policy.json` allows one attempt with
one-MiB direct synchronous writes, at most 16 starts per second, a
20-second issue deadline and a 256-MiB cap. The two-second preflight,
six-second sleeping child, 5%/1% execution-plus-steal ceilings, CPU placement
and existing CPU/LXD/clock scopes are unchanged. Actual monitor endpoints
must show positive I/O wait and idle-only non-idle above 1%, while both new
guards accept. The previous four-case result remains closed. No retry,
additional host control, benchmark or qualification campaign is authorized.

**Stronger I/O control passed and audited (2026-10-05):** the single actual
Docker launch completed one case, with no retry. CPU 64's monitor interval
contained 612 total ticks: 588 idle, 21 I/O wait, and three execution ticks
(one user, two system; no steal). Both new guards accepted. Execution plus
steal is 0.4901960784%, below the inclusive 1% ceiling; idle-only non-idle is
3.9215686275%, which would reject the same interval. This is an arithmetic
old-policy contrast on the new control's exact endpoints, not a rerun or
reclassification of an archived sample.

The owned actor completed 140 one-MiB direct synchronous writes in
9.106844007 seconds. Its minimum write-start spacing was 62.562480 ms, within
the declared rate and byte/time bounds. The independent audit checks all
four counter intervals, 14 raw artifacts, 43 executed source identities and
188 retained-history identities. Both children exited zero and were reaped
without forced kills; the owned directory/container were removed. CPU
properties/kernel mask, clocks and declared LXD state restored; the
protected identity remained absent and the workqueue mask did not change.

`validation/guard-execution-v2-io-02-completed-audit.json` retains the evidence
(SHA-256 `59e25b009fd73d9699b2c8b671876015872ca0125ff11ce9a802b8e006ef3503`).
Together with the earlier quiet/worker/SMT controls and 42 passing Docker
contract tests, the declared guard-control behaviors are now demonstrated.
The earlier 4-Hz I/O case remains inconclusive. These are guard controls, not
performance qualification: no benchmark or qualification sample ran.
Fresh matched measurement boundaries and a finite coherent campaign still
need separate authorization; all closed performance outcomes remain intact.

**Fresh matched guard-v2 qualification authorized (2026-10-05):** the user
selected “Freeze and run fresh qualification”; the decision is retained in
`validation/guard-v2-qualification-01-user-decision.json`. This permits new
matched C0/R0/S1/R1 boundaries and the complete prescribed qualification
sequence, not reuse of old-filter performance results. The fresh S1 core
campaign retains all 54 cells, 24 clean balanced blocks per cell and the
60-attempt cap. Other frozen workloads/budgets and the 0.97 confidence-bound
gate remain unchanged. Fatal, exhausted or inconclusive gates stop progress;
no pooling, selective retry or historical reclassification is authorized.

The prospective operators live in `guard-v2-q01/`, separate from executed
collector families. The existing freeze format requires actual builds, so
new boundaries rebuild all arms rather than relabel retained ELFs. Retained
R0/S1 crate sources use private read-only mount namespaces; the live R1 tree
must remain unchanged. The new family and operator bytes join every arm's
immutable-input census. Existing CPU/LXD/clock scopes remain the only host
controls. Boundary construction and campaign execution are not yet complete.

**q01 real builds finalized; qualification stopped at host-control postcheck
(2026-10-05):** the integrated pinned-Docker preflight passed all 42 guard
contracts and 16 clean-process CLI checks. It reconstructed the exact 400-file
crate tree for each retained arm, including R1's two deleted paths, and verified
that all 54 S1 cells retain their original order, full-work configurations and
24-clean/60-attempt design. See
`validation/guard-v2-q01-preflight-01/result.json`.

The unchanged freeze factory then performed actual matched builds:

- `R0-13-guard-v2-q01`: 13 successful build commands, 25 binaries, nine
  attestations; finalized SHA-256
  `09a0ad297b39800011e8c30976a653841747665c51e2e75819d0991a18601576`.
- `S1-04-guard-v2-q01`: 11 successful build commands, 17 binaries, five
  attestations; finalized SHA-256
  `672ed1c40efa3e318ce59077cf3f7e98396694ae0b11655675055f1b02c950a8`.
- `R1-06-guard-v2-q01`: 11 successful build commands, 17 binaries, five
  attestations; finalized SHA-256
  `beddbd26d755b553b8c8667c6ab56bfd941c7d9887bbeb54866ee0aa9d51e29b`.

All three share immutable-input SHA-256
`84c8ef89c30f62a5d4060b90ebf21de0ccf668e19293cab4e473e1f5badd831c`.
Five actual Docker probes per boundary verified the complete read-only crate
view; each live-worktree census was unchanged after private mounts were removed.
S1 and R1 each rebuilt byte-identically to all 17 of their old executables.
R0/C0 matched 22 of 25 old executables; three C0 ThinLTO executables differed.
These comparisons do not reuse old timing or establish new performance acceptance.

The R1 factory and source cleanup completed, but its enclosing command exited 1:
`cpu_reservation.py` rejected reuse of numeric PID 3043244 during the final
protected-process check, after restoring CPU access. The original identity
`(PID3043244, UID2008, start_ticks370015719)` was absent before reservation and
after configuration. A read-only postcheck found a different, stable kernel
worker identity: UID0, start_ticks526960411, `kworker/22:0-rcu_par_gp`.
Neither identity nor any kernel-worker affinity was modified.

The retained CPU controller receipt remains a failure; it is not rewritten as
a successful restore. Independently, its saved restored properties and the
live `user.slice` properties/raw `cpuset.cpus` exactly match the original state;
LXD and clock restoration also passed. The full closure and read-only evidence
are bound in `validation/guard-v2-q01-build-closure-and-halt.json`. No fresh
functional cases or qualification samples ran. The fatal-stop rule applies:
do not resume qualification, patch executed controller/family bytes, or waive
protected-identity checks without an explicit new scope.

**Identity-aware controller and fresh qualification authorized (2026-10-06):**
the user selected “Repair guard, refreeze, qualify”; see
`validation/guard-v2-qualification-02-user-decision.json`. The separate
`guard-v2-q02/` family will freeze the new
`validation/cpu_reservation_identity_v2.py` and its regression tests, then
perform real matched rebuilds and the complete finite qualification. All
workloads, budgets, confidence thresholds and existing host controls remain
unchanged. No q01 receipt or executed program is modified.

The controller distinguishes stable numeric-PID reuse from the protected
PID/UID/start-time identity and records the unrelated occupant without querying
its thread affinity. Matching start time with unexpected UID, or identity
changes during observation, still fail closed. The original reservation,
restoration, protected-thread checks and lock identities are preserved.

Pinned Docker passed all 49 contracts, including seven identity-transition
tests, and 17 CLI checks. Exact source reconstruction and all 54 S1 full-work
configurations/order still match. That first preflight remains failed overall:
its final read-only report attempted an unsupported `/proc/1/ns/pid` read.
Only the read-only smoke was repeated, comparing accessible Docker
`/proc/self/ns/pid` against an independently retained host namespace witness;
no capabilities or controls were added. The protected numeric PID was then
absent. A distinct owned-process fixture exercised the changed stable-occupant
branch against real `/proc` data with an explicitly synthetic historical
identity; it did not induce or claim a real kernel PID-reuse event.

`validation/guard-v2-q02-identity-validation-audit.json` binds the passed
contracts, both successful read-only checks, preserved metadata failure and
unchanged controller/family hashes (SHA-256
`3653ac2b938382f0096135c9ba2b061f5927550743c95639f3791d11f21c1cad`).
No new reservation or qualification sample ran during this validation.
Fresh q02 boundaries and qualification remain required.

**q02 matched rebuilds and fresh functional checks completed (2026-10-06):**
`R0-14-guard-v2-q02`, `S1-05-guard-v2-q02` and `R1-07-guard-v2-q02`
completed 35 real build commands, producing 59 executable roles and 19
attestations. All share immutable-input SHA-256
`52bf16c902b12404974d5e9094afc2f62528edef83ac45e56aefa02b0d77a7b0`.
Fifteen actual Docker probes verified exact, read-only 400-file source trees;
private mounts were removed and live source censuses remained unchanged.
Every enclosing supervisor exited zero and all CPU/LXD/clock controls restored.
`validation/guard-v2-q02-matched-boundaries-audit.json` binds these boundaries
(SHA-256 `e434e3d2ab396a5e7e40a6f88ef3f465f40c330b20adb3e9077e3e92514f0dbe`).

The new frozen binaries then passed all 570 functional cases in pinned Docker:
15 per C0 profile and 90 per R0/S1/R1 profile, each release and ThinLTO.
Raw outputs, exact commit counts, sparse CPU mappings, executable identities
and all command exits were audited. The supervisor exited zero; controls
restored and the protected identity remained absent and untouched.
`validation/guard-v2-q02-functional-01-audit.json` records the results
(SHA-256 `da821227da321e2f818e85fb7e01fbeaace369c95a54385ffb417425c02db0b9`);
the separate post-source audit confirms all launch inputs stayed unchanged.
Two offline audit assumptions were corrected without rerunning workloads:
the existing `BENCH_RESULT=` prefix is valid, and successful native functional
cases may contain retries. Actual abort counts remain retained for the final
candidate/baseline review; no zero-abort requirement was added.

All 17 R0 and 17 R1 executable roles are byte-identical to their previously
qualified counterparts. R1 production/core-test sources, repository identities,
native recipes, shared inputs and native output manifests also match exactly.
`validation/guard-v2-q02-source-qualification-association.json` therefore binds
the existing correctness, instrumentation, example, layout and bounded-code
reviews to this rebuild (SHA-256
`084d142ce5c44929ac780122fc156682829592e9787440fe51ec609bf172d439`).
Those suites were **not rerun**; all original scope limitations remain.
No historical timing is reused. Fresh C0/R0 baseline, full finite S1 and final
R1 qualification remain required; these build/functional results contain zero
performance-qualification samples.

**q02 baseline completed; five-percent successor authorized (2026-10-06):**
the fresh release and ThinLTO baseline completed 90 allocation samples in
90 attempts and all 64 C0/R0 singleton cells. Each cell has six clean balanced
blocks: 384 accepted blocks from 389 attempts, with five whole blocks excluded.
All 1,556 engine processes completed; 1,536 belong to accepted blocks.
Original 3% classifications remain unchanged: all 32 release cells are existing
C++ deficits, while all 32 ThinLTO cells meet that budget. These are baseline
classifications, not an R1 acceptance verdict. Exact workloads, counters,
affinity, retained measurements, source/binary identities and exclusions were
audited; all host controls restored and the protected original identity remained
absent at the recorded checkpoints.
`validation/guard-v2-q02-baseline-01-audit.json` binds the evidence (SHA-256
`e6aec63c13d4b51642767a94722d82015884487d09326a9bd7e3ff290630dd86`).

The user then selected **“Adopt 5%”**. The new authorization is
`validation/guard-v2-qualification-03-user-decision.json` (SHA-256
`ed87971ef42d6e467a00aa229d4cff40a454639c2a6363519bb8cf1ee09a225d`).
q02 closes before S1 or final R1 timing; its data and every older outcome remain
immutable and will not be reclassified or reused as five-percent qualification.
The separate `guard-v2-q03/` family must freeze the revised policy before fresh
matched qualification. Its one-sided 95% lower throughput bound must be at
least 0.95 against the original R0 baseline, not a compounded allowance at
successive stages. This permits a real 3–5% slowdown; it is not merely a report
format change.

Only the throughput margin changes. Confidence, sample counts, finite attempt
budgets, workload dimensions/order/seeds, quality filters, CPU/clock/LXD
controls, correctness, allocation/copy/atomic/dispatch, memory, native-ABI and
abort-cause gates remain unchanged. Corresponding C++ budget thresholds become
0.95; existing-deficit no-supported-worsening and baseline-straddling rules
remain. New-family validation, actual matched boundaries and fresh qualification
are still required; no five-percent performance acceptance is claimed.

**Five-percent policy validation completed (2026-10-06):**
the active successor is `guard-v2-q03-v2/`. In pinned Docker, all 49 inherited
guard/controller tests, 24 acceptance-behavior tests and 17 clean-process CLI
checks passed. Actual synthetic analyzer CLI runs accepted a uniform 0.96
ratio and rejected 0.94 at the unchanged one-sided 95% confidence level.
The exact 54 S1 configurations/order and 24/60 budgets match the prior design;
all three retained source parents reconstructed exactly 400 files each.
Protocol comparison found only the approved throughput-threshold changes.
A real read-only host-namespace observation classified the protected original
identity as absent; no host control changed and no benchmark sample ran.

The first preflight remains failed and immutable: 49 inherited and 21 new
tests passed, but three new assertions required 12-decimal agreement with a
hard-coded Student-t quantile. The existing SciPy approximation differed by
`6.638662952695995e-10`. The successor replaces that brittle oracle with an
independent closed-form df5 CDF check, not a change to production statistics
or acceptance. All earlier source families and outcomes remain untouched.
`validation/guard-v2-q03-five-percent-validation-audit.json` retains both
results and the exact protocol/source evidence (SHA-256
`98e22cb9f7d4eb7da3c3dca6c9ff664b5751fc7984a16e085be955ad3daba156`).
This is policy/harness validation only; actual matched builds, fresh baseline,
S1 and final R1 qualification remain required.

**Fresh five-percent boundaries and functionals completed (2026-10-06):**
the actual matched boundaries are `R0-15-guard-v2-q03-v2`,
`S1-06-guard-v2-q03-v2` and `R1-08-guard-v2-q03-v2`. All three ran the original
prepare/build/finalize factory: 35 build commands, 59 executable roles and
19 attestations, with 15 actual Docker proofs of the exact read-only 400-file
source view. Their common immutable-input SHA-256 is
`0f83efb644cef35ce2f4b81b584964102aae7b5a1010699ee1fe9484ef4ea5d3`.
The complete boundary audit is
`validation/guard-v2-q03-v2-matched-boundaries-audit.json` (SHA-256
`7f842917c876305cf11e63067f91c98ac430a67b64cd6f403cf643821eca3277`).

All 17 Rust executable roles per stage are byte-identical to their earlier
qualified counterparts. Three C0 ThinLTO executable hashes differ only in GNU
build-id and nonallocated static symbol/string sections; every other section's
type, flags, address, size and payload hash matches. Native recipes, shared
inputs and native output manifests also match. The exact-source association
retains all 10,111 original files, with only the three approved existing
documentation/guard changes and 40 new harness files; no production/core-test
source changed. Earlier correctness, instrumentation, layout and bounded-code
evidence is associated, **not rerun**, with its original qualifications:
`validation/guard-v2-q03-v2-source-qualification-association.json` (SHA-256
`e26d9da5a63c7373367643d4c77e417ec9d51ec42b9e0a2e883525a9706bfdf2`).

All 570 newly executed native functional cases passed in pinned Docker:
15 C0 and 90 R0/S1/R1 cases per profile. Raw outputs, exact commit counts,
ordered/observed CPU placement and frozen binary identities were audited.
Actual abort counts remain recorded; no zero-abort requirement was added.
Every build/functional scope restored CPU reservation, LXD and clocks, and
the protected original identity remained absent at the recorded checkpoints.
`validation/guard-v2-q03-v2-functional-01-audit.json` retains the evidence
(SHA-256 `bc46c11cfda6079e92174441f24c5558d9b34112a16f6dbf60ae1847167d0002`).
These results contain no performance-qualification samples. Fresh C0/R0
baseline, complete S1 and final R1 finite campaigns remain required.

**q03-v2 baseline completed; ten-percent successor requested (2026-10-07):**
all four fresh baseline lanes completed: 90 accepted allocation samples in
92 attempts and all 64 singleton engine cells, with 384 clean balanced blocks
from 403 attempts. The 19 excluded whole blocks remain excluded. All 1,612
engine processes completed; 1,536 belong to accepted blocks. Under the frozen
5% rule, release has 31 existing C++ deficits and one inconclusive cell;
all 32 ThinLTO cells meet that budget. The release 29-worker cell's R0/C0
geometric mean is 0.9202378237, with separate one-sided 95% lower/upper bounds
0.8879268628/0.9537245551. These are baseline classifications, not S1/R1
regressions or candidate acceptance.

Accepted workloads, exact counters, CPU placement, balanced-block coverage,
quality evidence, retained sources/binaries and all input identities were
audited. CPU reservation, LXD and clocks restored exactly, and the protected
original identity remained absent at all recorded checkpoints.
`validation/guard-v2-q03-v2-baseline-01-audit.json` binds the evidence (SHA-256
`377de3ccc5209b83e4dbc17d12a9cce2c01bd7c46317b75c376635a3d465310e`).

The user then asked to relax the performance gap to **10%**. The authorization
is `validation/guard-v2-qualification-04-user-decision.json` (SHA-256
`982f41666906a5a268562cfff3b03400e2134f8166e678d09151d9bbd54c7334`).
q03-v2 closes before any S1 or final R1 timing. Its 5% classifications and every
older outcome remain immutable; no old timing is reclassified or reused.
The separate `guard-v2-q04/` policy requires a one-sided 95% throughput lower
bound of at least 0.90 for both S1/R0 and final R1/R0 against the same original
Rust baseline, without compounding stage allowances. The corresponding C++
threshold becomes 0.90; existing-deficit no-supported-worsening and
baseline-straddling rules stay unchanged.

This permits a real slowdown up to 10%; it does not guarantee statistical
acceptance or waive incomplete coverage. Workloads, order, seeds, confidence,
24-block S1 core and six-block final coverage, 60-attempt caps, measurement
quality and all correctness/resource/abort/native-ABI gates remain unchanged.
New-policy validation, real matched boundaries and fresh complete qualification
are required. No ten-percent performance acceptance is claimed.

**Ten-percent policy validation completed (2026-10-07):**
`guard-v2-q04/` passed all 49 inherited guard/controller tests, 24 updated
acceptance-behavior tests and 17 clean-process CLI checks in pinned Docker.
Actual synthetic analyzer CLI runs accept 0.91 and reject 0.89; the boundary
tests retain inclusive 0.90 acceptance, unchanged one-sided 95% Student-t
confidence, existing-deficit/straddling rules and no compounded stage allowance.
The exact 54 S1 full-work configurations/order and 24/60 budgets are unchanged.
All three retained source parents reconstruct exactly 400 files each, including
the R1 deletion entries. Protocol comparison changes only throughput-budget
declarations; point/TPC-C and allocation-diagnostic protocols are byte-identical.

All 64 preflight input hashes remained unchanged. The real read-only host
identity observation found the protected original process absent; no host
control changed and no benchmark sample ran.
`validation/guard-v2-q04-ten-percent-validation-audit.json` retains the evidence
(SHA-256 `e2f22c7b3c6cb4deec50cf0d50d37c7273172173d748c13131b71baf23e58096`).
This validates the policy, not performance. Actual matched builds, fresh
functionals/baseline, complete S1 and final R1 qualification remain required.

**Fresh ten-percent boundaries and functionals completed (2026-10-07):**
`R0-16-guard-v2-q04`, `S1-07-guard-v2-q04` and `R1-09-guard-v2-q04` ran the
original prepare/build/finalize factory: 35 actual build commands, 59 executable
roles, 19 attestations and 15 Docker proofs of the exact read-only 400-file
source view. R1 also preserves its two deletion entries. All three boundaries
share immutable-input SHA-256
`62efa2e8dceb966552046c1b7d827724353e8a16c3243a5a5ccfdb76bd830a52`.
The aggregate audit is `validation/guard-v2-q04-matched-boundaries-audit.json`
(SHA-256 `d0ce2254de0811f46db7ab65f8266282bf2c3300a344e9afc440b808deef1e77`).

All 17 Rust roles per stage match their prior executables byte-for-byte.
Three C0 ThinLTO binaries differ only in GNU build-id and nonallocated static
string-table payloads; all other section types, flags, addresses, sizes and
payloads match. Native recipes, shared inputs and native output manifests
match. The first library-bundle invocation retained an old-family import and
was rejected by the namespace precondition before creating its directory or
running Docker. Its receipt remains failed; the corrected invocation performed
the actual q04 Docker inspection and prepared the fresh R0-16 bundle.

Direct comparison to the original qualified R1-05/R0-12 retains all 10,111
source-census entries, permits only the three existing documentation/guard
changes and 40 declared new harness inputs, and finds no production/core-test
change. Original correctness, instrumentation, layout, example and bounded-code
evidence is associated, not rerun, with every original qualification retained:
`validation/guard-v2-q04-source-qualification-association.json` (SHA-256
`25757177b9cba485e4054f040b338ee838ecef65b09506cb391888639aa1a6a6`).

All 570 fresh compiled functional cases passed in pinned Docker. Raw outputs,
exact commit counts, actual backend identities, native integration metadata,
ordered/observed sparse CPU placement and frozen binary hashes were verified.
No zero-abort gate was added. All 64 declared inputs stayed unchanged through
the builds/functionals; each scope restored CPU reservation, LXD and clocks,
and the protected original identity remained absent at recorded checkpoints.
`validation/guard-v2-q04-functional-01-audit.json` retains the evidence (SHA-256
`21e9588764f70527b750f101c6aa8a3cb0981e9c4f82ca0eac664ae56d3c15d2`).
These are not performance-qualification samples. Fresh baseline, complete S1
and final R1 finite qualification remain required.

**Ten-percent baseline completed; C++ straddling rule blocks final acceptance
(2026-10-07):** all 90 allocation samples completed in 90 attempts. All 64
singleton engine cells collected six clean balanced blocks: 384 clean blocks
from 400 attempts, with 16 whole blocks excluded. All 1,600 processes completed;
1,536 belong to accepted blocks. Work/counts, source/binary identities,
placement and quality evidence were audited; all launch inputs stayed unchanged
and CPU reservation, LXD and clocks restored.
`validation/guard-v2-q04-baseline-01-audit.json` retains the evidence (SHA-256
`7f331010b27c882948e375bbad52e64a3a985bf4cb9359a271a36c48192469ad`).

At the declared 0.90 C++ boundary, release has **29 inconclusive baseline
cells**, one existing deficit and two within budget. All 32 ThinLTO cells meet
the budget. Release R0/C0 geometric means range from 0.8887381420 to
0.9247899247. These measure the original Rust baseline against C++, not a
candidate slowdown.

The unchanged `protocol-cpp-gap.json` rule says a baseline threshold-straddling
cell is **inconclusive regardless of candidate; never pass**. Its implementation
in `analyze_cpp_gap.py::compare_cells` therefore prevents final acceptance in those
29 cells even if a later candidate improves. Further S1/final R1 timing is
stopped pending an explicit policy disposition; neither has been launched and
no extra rounds ran. This is not a candidate failure or a blanket performance
waiver. Current and older sources, observations and classifications remain
immutable; a changed policy requires its own declared fresh qualification.
`validation/guard-v2-q04-baseline-cpp-straddling-blocker.json` records the exact
rule, cells and limits (SHA-256
`d05c20741e586ad2362f7b19c37e9fceadb0db7ecc3d699f7b3cad7fb3077f23`).

**Candidate C++ floor authorized for uncertain baselines:** the user selected
“Require candidate to meet C++ floor.” The prospective `guard-v2-q05/` policy
changes only the baseline-straddling branch: candidate R1/C0 one-sided 95% lower
bound ≥0.90 passes this C++ rule; upper bound <0.90 fails; otherwise inconclusive.
Baseline uncertainty alone no longer vetoes a qualifying candidate. The 10%
budgets, direct S1/R0 and final R1/R0 checks against the same original R0,
existing-deficit Welch rule and every other gate remain unchanged.

The decision is recorded in
`validation/guard-v2-qualification-05-user-decision.json` (SHA-256
`9d67d68326266f95f60bc6ba019735f2b433e2a410cba4779084070e718d4cd4`).
The q04 blocker note's function label `gap_cells` was a metadata typo;
`compare_cells` is the actual function. Its recorded rule and 29 cells stand.
q04 and older programs, data and outcomes remain closed and immutable. No
reclassification or timing reuse is authorized. Fresh policy validation,
matched builds, functionals, baseline, S1 and final R1 qualification are required.

**Candidate-floor policy verified in Docker:** all 73 tests and 17 clean-process
CLI checks passed. The actual comparison API, fed synthetic balanced-block
archives for all 32 singleton cells, accepts candidate 0.91 and inclusive 0.90,
rejects 0.89, and retains inconclusiveness for uncertain candidate bounds.
The 54 full-work S1 configurations/order and 24/60 budgets are unchanged;
all three original source parents reconstruct exactly 400 files each. Only
the two C++ acceptance descriptions change among protocol fields. All 64
preflight inputs remained unchanged; the real read-only protected-identity
snapshot completed without host mutation. These controls are not measured
performance and contribute zero qualification samples.

Evidence: `validation/guard-v2-q05-candidate-floor-validation-audit.json`
(SHA-256 `c14637a1d1c3b8b7518d2e070d27ee93a344e89853ebd3ec346374fa48d2a08c`);
Docker command `commands/1791419377590806932-guard-v2-q05-preflight-01`,
exit 0. Fresh matched builds and performance qualification remain required.

**Zoo-004 relocation authorized; maximum 30 workers:** the user selected
“Move to zoo-4, maximum 30 workers.” `guard-v2-q06/` is a fresh family; q05
closes before any matched build or timing. Workers use physical CPUs0–29,
writeback CPU30 and helper/monitor CPU31; corresponding worker SMT siblings
are CPUs32–61. Every former 32-worker point becomes 30, and the singleton sweep
is 1–30: no qualification of counts31/32 is claimed. Other worker counts,
per-worker work, seeds/order, 10%/95% rules, candidate C++ floor, 24/60 S1 core
and 6/60 final budgets remain unchanged. Both S1 and R1 must use the same
freshly rebuilt original R0. No zoo-002 timing is reusable.

Authorization: `validation/guard-v2-qualification-06-user-decision.json`,
SHA-256 `75ba7cae50c3e212d7e04b6ebafeb82e0af126be9f8cd51c4a191f50575d83f0`.
The authenticated host has 32 physical/64 logical Broadwell CPUs. The private
Docker daemon and pinned image are installed; native prerequisites and fresh
CMake configuration succeeded. Only retained tool/cache/source inputs were
transferred, not old Mako build trees or qualification binaries. Zoo-002
user.slice/LXD/protected-PID exceptions are not transferred. No foreign
process, service, IRQ, workqueue or affinity changes are authorized.

**Real host guard validation passed, not performance qualification:** the
integrated fifth preflight passed 88 tests, 27 help CLIs and eight analyzer
invocations. Actual loaded Docker smokes on CPU0 and CPU29 passed the unchanged
30-worker SMT guard, with PMU running ratios1.0 and off-CPU fractions
0.000748843/0.000135692. Normal exit and deliberate child exit23 both restored
all64 clock policies and Intel `no_turbo` exactly, including supervisory
readback. Perf paranoia and NMI-watchdog controls were unchanged.

The first two loaded smokes remain rejected. The observer was contributing
host PID1 work: `systemctl show --property=NRestarts` used `Properties.GetAll`,
and opening a new peer per poll retained substantial setup cost. Q6 now
serializes one private sd-bus connection per collector and freshly queries
the same UInt32 `NRestarts` property each time. No counter-value cache,
absence-to-zero fallback, extra retry, polling/journal change or weaker guard.
Real concurrent reads, connection/property failures and descriptor cleanup
were exercised; the actual libsystemd target and loader closure are retained
by the fresh input factory. Evidence:
`validation/guard-v2-q06-host-runtime-validation-01.json`, SHA-256
`5adfb11a25df29023b2d5d61fd15f62fefa948629000c0436af51442b5a6cff7`.

**Complete relocation preflight passed:** the sixth preflight covers all56
active sources, with 88 behavioral tests, 33 help CLIs, eight analyzer CLIs and
the real full-correctness inventory CLI. Its 14 gates/19 commands/six reports
preserve the original gate bodies exactly. Q6 now binds those gates, the full
instrumentation suite, public example, matched layout/code collection and
complete source/native postchecks to explicit fresh boundaries and library
provenance. CTest3.31.6 and pinned nightly/Miri components are available.

The distro-patched RocksDB9.11.2-1 Clang22 TSan dependency was actually rebuilt,
namespace-verified and pinned before freezing. Its C API smoke performed a
binary-value write, synchronous WAL flush, close/reopen and exact readback
without a sanitizer report; temporary smoke source/binary/database were removed.
This does not replace the full native sanitizer gates.
Evidence: `validation/guard-v2-q06-relocation-preflight-01.json`, SHA-256
`22e985bb617885486f938e49bd7d2fd805e4facd4d8e6e4882f257b6eff2d04b`.

These observations contribute zero qualification samples. Fresh matched
boundaries, 570 functionals, full native correctness/instrumentation, baseline,
complete S1 and final R1 matrices, mechanism/resource attribution and acceptance
remain required. Keep the settled source family unchanged through those builds
and campaigns.

**Q6 build and functional attempts (2026-10-08):**
`R0-17` stopped before building because the new Ninja tree had no dependency
census. The actual native prerequisite targets were then built in Docker from
the retained R0 source, without adopting zoo-002 native outputs.
`R0-18` and `S1-08` finalized, but unattended host upgrades changed five
`linux-libc-dev` headers during `R1-10` preparation/build admission
(`7.0.0-34.34` → `7.0.0-38.38`). The complete 3906-header path census and 400
runtime/tool-library inputs were unchanged; five headers' contents were not.
No host update service was disabled and no host file was rolled back.
That build generation is closed in
`validation/guard-v2-q06-build-generation-01-closure.json`.

A complete post-update matched set (`R0-19`, `S1-09`, `R1-11`) then finalized.
Its functional campaign passed both C0 profiles (30 cases) and R0 release's
15 engine cases, then rejected the first mixed probe before workload execution:
CPU0 was outside its inherited helper-only CPU31 affinity mask. This was a
launcher defect, not a reason to relax probe admission. That generation and
its partial functionals are closed in
`validation/guard-v2-q06-build-generation-02-closure.json`.

The raw functional launcher now supplies the explicit worker-pool mask before
probe admission and per-worker pinning, matching `measure_probes.command_for`.
The global Docker helper/monitor policy, workload, seeds and validation are
unchanged. Actual corrected-launch validation passed all 90 R0 release cases:
15 engine, 16 mixed and 59 real-native cases at the prescribed 1/4/30-worker
points and payload transitions. Exact measured commit counts, ordered CPU maps,
native-build flags, executable identities and normal/supervisory clock
restoration were checked. This host-specific runtime regression exercise is
not a mocked argv test and contributes zero qualification samples.
Evidence: `validation/guard-v2-q06-affinity-admission-validation-01.json`, SHA-256
`e82b83589daa3561170679ea40fbdc92e8450e3b30e14281e86f6daa15c767c4`.
The corrected source family requires complete preflight, new matched builds
of every arm and all 570 functionals before the remaining gates; none of the
closed generations' partial cases or timings is carried forward.

**Q6 complete functionals/correctness and sanitizer startup repair (2026-10-08):**
The next matched generation (`R0-20`, `S1-10`, `R1-12`) passed all 570
functional cases: 120 engine, 96 mixed and 354 real-native cases. Both profiles,
exact commit counts and CPU maps were checked; clock restoration passed.
The final Rust source then passed all 14 full-correctness gates, 19 commands
and six fresh reports. Fifteen complete source checks and native-input checks
passed. Evidence:
`validation/guard-v2-q06-functional-validation-02.json` and
`validation/guard-v2-q06-correctness-validation-01.json`.

Miri passed, but the address gate stopped at its native LSan negative control,
before the test reached `main`. Actual Docker debugger evidence attributes
the crash to LLVM22 libunwind's unbounded fallback scan beyond Ubuntu26.04's
loader `.eh_frame`, matching
[LLVM #226351](https://github.com/llvm/llvm-project/issues/226351).
The full source census remained unchanged, the private source mount was
removed, and all 225 production-native input records were unchanged.
UBSan, TSan and standalone normal-Loom were not reached in that attempt.

A diagnostic selecting the system GNU unwinder isolated the failure without
changing the retained executable. A subsequent fresh nightly/build-std native
control build used an explicit Clang configuration, **without a preload**:
all test assertions passed, LSan reported exactly one 1301-byte owned buffer,
the expected allocation frame was present, and no ASan error occurred.
Loader binding evidence identifies `libgcc_s.so.1`; all retained native
archives/configuration inputs were unchanged. The permanent sanitizer recipe
now passes that configuration through scoped Rust/Rustdoc flags and native
executable/shared/module link flags, retaining the single Clang sanitizer
runtime, slow unwinding, leak detection and the original suppression policy.

The diagnosis and both controls are retained in
`validation/guard-v2-q06-native-lsan-diagnosis-01.json`.
Generation03 is closed before timing in
`validation/guard-v2-q06-build-generation-03-closure.json`.
No performance samples or acceptance budgets were consumed. These diagnostics
do not pass the interrupted sanitizer gate: the changed recipe/docs require
new matched boundaries and complete required qualification before baseline,
S1 and R1 performance acceptance.

**Q6 repaired matched family and closed baseline timeout (2026-10-09):**
Generation04 finalized `R0-21`, `S1-11` and `R1-13` with identical common
production/toolchain inputs. All 570 fresh functional cases passed, followed
by the full 14-gate/19-command/six-report correctness run and all five
instrumentation gates: Miri, ASan, UBSan, TSan and normal-Loom. The native
point and RMW LSan controls each retained exactly one 1301-byte owned-buffer
leak with the intended frame; the GNU-unwinder repair adds no suppression.
Existing quarantine and process-lifetime exceptions remain qualified.
Evidence: `validation/guard-v2-q06-matched-inputs-04.json`,
`validation/guard-v2-q06-functional-validation-03.json`,
`validation/guard-v2-q06-correctness-validation-02.json` and
`validation/guard-v2-q06-instrumentation-validation-02.json`.

The actual public example prints `committed balances: from=90, to=60`.
All 16 corresponding layout roles agree in both profiles. Independent bounded
release and ThinLTO commit/cleanup/recycling reviews found no unexplained added
work in the inspected mechanisms; actual surviving callers cover the missing
standalone ThinLTO recycling helper. These are not whole-program equivalence
or performance claims. The complete post-correctness census matched 10,168
source files and five repository identities; all 225 R1 native records remained
unchanged. Evidence:
`validation/guard-v2-q06-public-example-validation-01.json`,
`validation/guard-v2-q06-attribution-validation-02.json` and
`validation/guard-v2-q06-post-correctness-validation-01.json`.

The first fresh C0/R0 baseline, `guard-v2-q06/campaigns/R0-01`, then stopped
fatally during release allocation collection. Eleven samples passed; attempt12
was the first repetition of `engine-logged`, one item, 30 workers, with
16,777,216 measured iterations per worker. It hit the collector's 600-second
deadline despite admitted CPU quality. Its stdout/stderr are empty: the runner
reports workers only after the completion barrier and ordered joins, so this
does not distinguish slow progress from a stall. The completed one/eight-worker
logged diagnostics took 12.772/72.456 maximum worker seconds; these instrumented
durations are not throughput qualification or evidence of the missing result.

Normal and supervisory clock restoration passed. The private Docker daemon
reported no running containers afterward. Retrospective kernel output contains
Docker-default ptrace-read denials, but establishes no timeout cause. All 58
frozen campaign inputs and retained diagnostic binary/provenance hashes were
verified unchanged before this documentation update. The closed attempt and
all raw evidence are retained in
`validation/guard-v2-q06-baseline-timeout-audit-01.json`.
No C0/R0 throughput comparison, ThinLTO allocation, S1 or final R1 campaign ran.
The fatal attempt is not retried, its partial samples are not pooled, and no
deadline, workload or acceptance threshold has been changed. Further execution
needs a separately declared diagnostic or fresh-campaign scope; overall
performance acceptance remains incomplete.

**Q6 single bounded timeout diagnostic (2026-10-09):**
The user authorized exactly one nonqualifying invocation of the same frozen
R0 release allocation binary, `engine-logged`, one item, 30 workers, with the
unchanged 16,777,216 measured iterations and 65,536 warmup iterations per
worker. The 600-second deadline, CPU placement, quality guards and reversible
clock controls remained unchanged. The owned inferior was killed at 600.011
seconds and reaped; observer success is not workload completion. Both normal
and supervisory clock restoration passed. The sole launch is consumed.

Owned-thread stacks at 30/120/300/540 seconds show 28/29/29/25 workers in
`mako_clock::Clock::enter_general`, all using the same clock word. The frozen
machine code matches its pause/load/compare-exchange gate; observed words
advance and timestamp-allocation owners change. At the last stack, workers
20/21/22 are at the **measured completion barrier**, not the warmup barrier.
Exact binary return-site attribution establishes at least 50,331,648 successful
measured iterations, but not complete final-key verification. CPU activity
alone is not this progress proof.

[INFERENCE] The diagnostic is progressing under severe shared HLC
certification-gate contention rather than remaining in one whole-run deadlock.
Four debugger stops do not measure a CPU-time percentage or establish the
sole cause of the earlier untraced timeout. No full result, total allocation
counts, completion-time estimate or performance acceptance is claimed.
Evidence: `validation/guard-v2-q06-timeout-diagnostic-audit-01.json`,
`validation/guard-v2-q06-timeout-static-code-01.json` and the raw
`guard-v2-q06/diagnostics/q06-timeout-observer-01` artifacts. Production
synchronization is unchanged. The original baseline stays closed, no samples
are pooled, and any further workload or longer deadline needs a new explicit
execution scope.

**Q7 fresh-campaign authorization (2026-10-09):**
After reviewing the bounded diagnostic, the user selected **Fresh
3600-second campaigns**. Q7 must settle and validate its isolated source
recipes, freeze a fresh matched C0/R0/S1/R1 family, and execute full fresh
baseline, S1 and final R1 campaigns. The 3600-second cap applies to performance
sample/allocation-diagnostic processes, including nested execution caps; it
does not reduce iterations, warmup, measurement or soak durations. Maximum
30 workers, placement, all quality/correctness/memory gates, 10% margins,
one-sided 95% confidence, S1 24/60 and final 6/60 budgets remain unchanged.
A further fatal timeout still closes its campaign.

Q6 R0-21/S1-11/R1-13 supply authenticated source bytes only. Their 400/400/402
crate records, including R1's two deletions, match the exact original stage
parents; they are not adopted builds or new timing evidence. Existing Docker
infrastructure and restoration-verified clocks are retained without host
service, security, IRQ, workqueue or foreign-process changes. Neither the
eleven old baseline samples nor debugger observations may be pooled into Q7.
Authorization and source lineage:
`validation/guard-v2-qualification-07-user-decision.json`,
`validation/guard-v2-qualification-07-family-origins.json` and
`validation/guard-v2-q07-source-parent-lineage-01.json`. No Q7 execution or
acceptance is claimed by this authorization.

**Q7 execution and terminal S1 result (2026-10-11):**
The fresh family finalized `R0-22-guard-v2-q07`,
`S1-12-guard-v2-q07`, and `R1-14-guard-v2-q07`, with a newly captured
`guard-v2-q07/libraries/q07-r0-01/manifest.json`. The matched-input audit
verified 9,821 equal noncrate source identities, five repositories, 224 common
native inputs, 82,266 shared toolchain/sysroot inputs and 447 immutable
workload/harness inputs. The sole permitted stage-specific native input is the
TPC-C bridge archive. Historical builds and timing samples were not adopted.

Fresh nonperformance execution passed 570 functional cases, the full
14-gate/19-command correctness selection and its six reports, and all five
instrumentation gates: Miri, address, undefined, thread and normal-Loom.
The public example printed `committed balances: from=90, to=60`.
All 16 compared layout roles retained size/alignment in both profiles.
Bounded R0/R1 release and ThinLTO code reviews found no unexplained added work
in their selected commit/recycling/abort/poison paths; this is not a whole-library
code-equivalence claim. Existing sanitizer/leak qualifications remain unchanged.

The full fresh R0 baseline completed: 45 accepted allocation samples and
30 C0/R0 singleton cells per profile, each cell with six clean balanced blocks.
Release used 180 attempted blocks; ThinLTO used 181, rejecting one whole block.
The retained 1,444 engine observations include that exclusion. All 60 baseline
cells classify as pre-existing C++ deficits; these classifications define the
later gap comparison, not an independent veto of the refactor. The formerly
timed-out 30-worker logged allocation cell completed: measured maximum-worker
durations were 723.891–765.530 seconds in release and 655.294–678.930 seconds
in ThinLTO. No iterations, warmup or measurement window was shortened.

S1 policy preparation initially rejected an output path outside Q7 before
launching any samples. Only the runtime destination and corresponding `--policy`
argument changed, to `guard-v2-q07/campaign-policies/S1-01.json`; frozen helpers
and statistical/workload policies did not change. The generated policy still
declared all 54 full-duration core cells with 24 clean/60 attempted blocks.

S1 release allocation completed with 45 accepted samples from 46 attempts.
Every request count and requested-byte total matched its fresh R0 counterpart,
including intentional TxnCell snapshots. The release engine sentinel then
collected six clean blocks at each of 1/8/30 workers, with no rejected block:

| Workers | Geometric mean S1/R0 | One-sided 95% lower bound | One-sided 95% upper bound | Classification |
| --- | --- | --- | --- | --- |
| 1 | 1.000380 | 0.997392 | 1.003378 | Meets budget |
| 8 | 0.990253 | 0.978046 | 1.002613 | Meets budget |
| 30 | 0.878645 | 0.815950 | 0.946157 | Inconclusive |

The 30-worker bounds straddle 0.90: non-inferiority is not established, but
neither is a statistically supported deficit beyond the 10% margin. All six
block point ratios are below one. All 72 engine observations passed their
quality gates; per-worker PMU running ratios were 1.0. At 30 workers, median
cycles/commit were 1,219.291 for R0 and 1,349.204 for S1, while median
instructions/commit were 2,952.437 and 2,948.315. These observations do not
identify a cause or exclude shared-cache/memory-bandwidth/helper interference.
No quality rule was weakened to accept these samples.

The frozen operator stopped without extra rounds. S1 release core, all S1
ThinLTO lanes and every final R1 performance/allocation lane remain **unrun**.
Both campaigns restored the exact pre-run clock state in normal and supervisory
checks. A source-only closure verified all 10,223 source entries, five
repositories and 225 native inputs unchanged; private mounts were removed and
the live/host source views restored. No workload was repeated for that audit.
This plan update was published only after closure, not during frozen execution.

Evidence under `/home/users/shuai/mako-sto-cpp-shape-20260924`:
`validation/guard-v2-q07-baseline-validation-01.json`,
`validation/guard-v2-q07-s1-terminal-audit-01.json`,
`validation/guard-v2-q07-post-campaign-source-validation-01.json` and
`validation/guard-v2-q07-closed-family-01.json`, with linked raw receipts,
source/build identities, counter observations and independent paired-ratio
checks. The closure SHA-256 is
`987f5880f42bff267788e5243f319d7b4ada3cda89443c10f3feac108a2eb1ba`.
Q7 is closed and not performance-qualified. It must not be resumed, selectively
extended or pooled into a successor campaign.

**Separately authorized bounded attribution (2026-10-11):**
The user selected **Bounded attribution** after the Q7 closure. This scope
required frozen release R0/S1 code comparison first, followed by at most one
R0–S1–S1–R0 profiling block at 30 workers, with unchanged work and a 3600-second
per-process cap. It did not authorize another qualification campaign or pooling.
Authorization:
`validation/guard-v2-q07-s1-attribution-user-decision-01.json`.

The fresh static collection under
`diagnostics/q07-s1-w30-attribution-01/static` authenticated the actual binaries
and LLVM tools, retained full indexes/disassembly and reviewed 45 symbol pairs.
It confirms the already documented private fallback outlining: two dispatchers
shrink from 5555 to 2942 bytes, with stack reservations changing from `0xa8`
to `0x78` and real register/spill/code-placement differences. The inspected
singleton typed-success path still calls prepare, activate, proxy and shape
tracking; it does not traverse the new ordinary-item fallback. Specifically
matching inspected bodies show no added operations, but this is not whole-program
equivalence or proof of the slowdown's cause. All 45 ranges have emitted FDE
coverage; the release binaries have `.eh_frame` but no `.debug_*` sections.

Metadata-only Docker preflight passed. The frozen method selected
`perf 7.0.14`, `cpu-clock:u`, 49 Hz, DWARF 16KiB stacks and a 256-page ring,
without another hardware PMU event, rebuild or host-policy change. Runtime
mapping permission and usable call chains were explicitly unproven by preflight.
The single attempt then stopped at the first R0 profiler with exit 255:
`Permission error mapping pages`, for `-m 256`. No engine launch token,
identity, ready or exit receipt was created; benchmark stdout/stderr and
`perf.data` are empty. Exactly one profiler and **zero benchmark processes**
launched. No stacks, paired timings, retry or alternate profiler were collected.

Read-only inspection found 4096-byte pages, `perf_event_mlock_kb=516`, an
8MiB soft/hard `RLIMIT_MEMLOCK`, `CAP_PERFMON` present and `CAP_IPC_LOCK`
absent. No limit, capability, sysctl or security policy changed. A smaller
fixed buffer is a possible new-scope remedy, not a proven mapping/loss result
or permission to reset this attempt. Normal and supervisory clock restoration
were exact; the owned main/metadata containers and profiler identity are gone.

The consumed token and all failed artifacts remain immutable. See
`validation/guard-v2-q07-s1-static-attribution-validation-01.json`,
`validation/guard-v2-q07-s1-attribution-metadata-validation-01.json`,
`validation/guard-v2-q07-s1-attribution-resource-limits-01.json` and
`validation/guard-v2-q07-s1-attribution-terminal-audit-01.json`
(SHA-256 `c9374eef8f8eb3d7f91eb0456103de7a6144317b1d86bc6837c8263d8f15180e`).
The static differences are attributed to known source lineage; the performance
slowdown remains unresolved and Q7 remains unqualified.

**Separately authorized 64-page attempt (2026-10-11):**
The user selected **One fresh 64-page attempt**, with a new declaration, output
root and consumed token under `diagnostics/q07-s1-w30-attribution-02`. The first
attempt remains immutable. Metadata preflight authenticated all 32 source
receipts and confirmed unchanged binaries, tools, work, sampling rate, DWARF
stack size, guards, placement and deadlines; only the ring changed from 256
to 64 pages, alongside the new authorization and evidence identities.

The ring mapped successfully. The first R0 engine and recorder both exited 0.
The engine verified 503,316,480 measured commits plus 1,966,080 warmup commits
across 30 workers, with all original count, PMU, worker, opening and whole-process
checks passing. Its single profiled throughput is 49,886,202.944 transactions/s,
with 1222.381 cycles and 2954.238 instructions per commit. This is an unpaired
diagnostic observation, not another qualifying sample or a comparison with S1.

The declared decoder then failed: its output field list requested `cpu`, but
the recording lacked that attribute. `perf script` exited 255 with
`Samples for 'cpu-clock:u' event do not have CPU attribute set. Cannot print 'cpu' field.`
This is a recipe mismatch, not a benchmark failure. The preceding header-only
decode exited 0 but warned `Invalid HEADER_EVENT_DESC: nre=1 sz=144 (min 64)`;
that warning remains unresolved. Exactly one R0 benchmark and recorder ran.
No S1 or second R0 slot launched, and no corrected decode or recording retry ran.
The retained `perf.data` is 244,992,580 bytes, SHA-256
`c7b094296b3518a74880ad02f0b54292b6fa6672307e699c5a5ed418edb61892`.
Decoded stack count is zero; loss, throttling and unwind coverage remain
unassessed because the block stopped before those checks.

The terminal audit authenticated raw stdout and all 30 WORKER rows, 35 retained
files, all 32 declared source receipts and the preceding attempt's 23 raw files
plus consumed token. Owned containers and recorded process identities are gone.
Normal and supervisory clock restoration are exact, and a subsequent readback
matches the saved pre-run state. No host limit, capability, security or production
source changed. See
`validation/guard-v2-q07-s1-attribution-terminal-audit-02.json`
(SHA-256 `aca1db2f5cd28d3ce95279f2c3ad8c428310a88e836e12279f49fc4de4602d9b`).
This attempt is closed, not accepted. Offline reprocessing of the retained R0
capture requires a separate scope; it would not reopen this block, supply S1
evidence or establish performance qualification.

**Separately authorized offline-only decoding (2026-10-11):**
The user selected **Offline decode only**. New output under
`diagnostics/q07-s1-w30-offline-01` leaves both consumed attempts and their
capture bytes unchanged. The pinned Docker decoder omits the unrecorded CPU
display field while retaining PID/TID, time, IP, symbols and DWARF processing.
All four commands (`evlist`, stack script, raw-event script and report) exit 0
with empty stderr. Recorded attributes confirm no CPU field, but do contain
PID/TID, user registers and the original 16KiB user-stack snapshots.

Raw and decoded counts agree: **14,736 samples**, including **14,729 engine
samples across 30 TIDs** and seven Python-supervisor samples. Of the engine
samples, **14,626 (99.30%) have no decoded frames**. Only 103 have any frames;
46 (0.312%) have multiple frames. The 221 emitted engine frames include 13
unknown frames, including 11 addresses below 4096. This is not representative
call-chain coverage, despite the commands succeeding and some engine stacks
being multi-frame. Empty stacks are missing evidence, not zero execution cost.

The raw dump has no recorded loss or throttle/unthrottle events, and the report
states zero lost samples. Unreported loss and unwind truncation remain unknown.
The complete 736-byte event-descriptor section accounts for one 144-byte
attribute, its padded name and 64 IDs; its attribute and IDs equal the main
header's copies. This follows the
[documented perf.data format](https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf.data-file-format.txt).
The earlier header-only warning is preserved; the exact pinned-reader cause is
not established, and the warning alone does not demonstrate a corrupt capture.

R0 leaf-IP context is retained separately from the sparse callchains. The
whole-capture, period-weighted report assigns 10.32% to singleton direct commit,
8.31% to `Transaction::put`, 7.76% to Masstree `reach_leaf`, 7.73% to
`find_unlocked` and 5.92% to `commit_with_optional_hook`. These include startup
and warmup and do not explain an S1 difference: no S1 capture exists.

The terminal audit hashes 36 output files, verifies all 41 declared inputs and
the prior diagnostic's 32 source receipts unchanged, and confirms unchanged
clock policy plus absence of owned containers/decoder processes. No benchmark,
recording, rebuild, host-policy mutation or consumed-block reopening occurred.
See `validation/guard-v2-q07-s1-offline-terminal-audit-01.json`
(SHA-256 `233fdaf76b0576a8a9707fd7344a1cd5de8d9ce3d62dca957df2d000828d8fbd`).
The offline scope is complete. S1 slowdown attribution and the original
performance acceptance remain blocked; this scope authorizes no further run.

**Prospective fifteen-percent policy (2026-10-12):**
The user increased the allowed performance regression to **15%**. The inclusive
one-sided 95% lower-bound floor is now **0.85**, with both S1 and final R1 still
compared against the same original R0; stage margins cannot compound. The
candidate C++ floor likewise becomes 0.85 when the baseline meets or straddles
the budget. The existing-deficit Welch no-supported-worsening rule is unchanged.

`validation/guard-v2-qualification-08-user-decision.json` authorizes the
prospective policy. `guard-v2-q08` contains the successor analyzers, protocols,
S1 enforcement and complete source-closure controls. Workloads, ordering,
confidence, block/attempt counts, 30-worker limit, 3600-second process deadline,
correctness, allocation, representation, quality and host-safety gates are
unchanged. All 53 copied Q7 source/configuration files and its closed-family
receipt remain unchanged; no historical timing is pooled or reclassified.

Final pinned-Docker preflight `validation/guard-v2-q08-preflight-02` passed
**89 behavior tests**, including 23 acceptance tests, 33 CLI-help checks and
**eight actual analyzer CLI checks**. Four synthetic cases exercise 30 cells
each through the engine API/CLI and C++ floor API: 0.86 passes, exact 0.85 passes,
0.84 fails, and uncertainty remains inconclusive. These are synthetic policy
checks, not performance measurements. The preflight also syntax-compiles 44
Python files, verifies source-closure integration and reconstructs all 400
retained files of each R0/S1/R1 source parent. The final suite removes one
inherited constant-echo test; the earlier successful preflight and its source
evidence remain retained separately.

See `validation/guard-v2-q08-margin-validation-01.json`
(SHA-256 `018ef4257ddf6c07a91925a88538c722fb8b465aaa72bded89539d3645623bc3`).
Both owned preflight containers are gone; all 64 clock policies match the
previous restored state. No production change, build, benchmark or recording
was performed for this policy update. Q7 remains closed and inconclusive: its
30-worker lower bound **0.815950** is also below 0.85. Fresh matched Q8 boundaries,
builds, host smokes and the full unchanged gate sequence remain required before
performance acceptance.

**Prospective eighty-percent throughput floor (2026-10-12):**
The user's subsequent request, “further decrease to 80,” lowers the retained
throughput floor to **0.80**, equivalent to a **20% regression margin**. The
one-sided 95% confidence rule, same original R0 for S1 and final R1, and
no-compounding requirement remain unchanged. The corresponding candidate C++
floor is 0.80; existing-deficit Welch treatment and every non-throughput gate
remain unchanged.

`guard-v2-q09` implements the successor policy under
`validation/guard-v2-qualification-09-user-decision.json`. Its declared Docker
preflight passed **89 behavior tests**, including 23 acceptance tests, 33
CLI-help checks and **eight actual analyzer CLI checks**. Synthetic cases across
30 cells each confirm 0.81 passes, exact 0.80 passes, 0.79 fails, and uncertainty
remains inconclusive. Source-closure integration, 44 Python syntax compilations
and three 400-file retained-source reconstructions also passed. All 53 Q8
source/configuration files remain unchanged, the preflight container is gone,
and all 64 clock policies match the prior restored state.

See `validation/guard-v2-q09-margin-validation-01.json`
(SHA-256 `087181d75008bc5a08140211e321c902e8202059ae2e70755b19e1ac86698251`).
This verifies the revised policy, not performance: no production change,
build, benchmark or recording was part of this policy preflight. Q7's recorded
30-worker lower bound **0.815950** is numerically above 0.80, but its closed
outcome is not reclassified and its samples are not reused. Fresh matched Q9
qualification must still cover all required S1 and final R1 gates.

**Fresh Q9 execution and terminal quality stop (2026-10-12):**
Fresh boundaries `R0-23-guard-v2-q09`, `S1-13-guard-v2-q09`, and
`R1-15-guard-v2-q09` completed 35 build commands, producing 59 binaries in
19 attestations. Matched-input evidence authenticates 82,266 shared inputs,
502 immutable inputs, 224 common native files and five repository identities.
The stage-specific TPC-C Rust archive is not claimed identical. Two intentional
noncompiled documentation corrections between R0 and S1/R1 fix
`TransProxy::replace_read` and explain its three typed field borrows; the
matched-input receipt records those differences rather than asserting that
every noncrate file is byte-identical.

Fresh pinned-Docker execution passed:

- **570 functional cases** across C0, R0, S1 and R1 and both release profiles.
- **14 correctness gates / 19 commands / six reports**, including native
  integration, ABI checks and hook-mutation behavior.
- Full **Miri, ASan, UBSan, TSan and normal Loom** gates. The public adapter
  example actually printed `committed balances: from=90, to=60`.
- R0/S1 and R0/R1 **release and ThinLTO** layout comparisons: all 16 instantiated
  roles retain their size/alignment. Four bounded emitted-code reviews found
  no blocking unexplained allocation/copy/atomic/dispatch mechanism in the
  selected routines and inspected ownership/cleanup callees. Unresolved
  indirect and data targets remain explicit limits; static review is not a
  dynamic allocation count, arbitrary-adapter layout guarantee or timing result.

The baseline campaign `guard-v2-q09/campaigns/R0-01` stopped in its first,
release-allocation profile. Its journal retains **83 attempts: 22 accepted
allocation samples, 55 prelaunch quiet rejections and six execution-quality
rejections**. All 28 launched workload processes returned zero, reported
verified results and supplied valid integer allocation counters. The profile
required 45 accepted samples and did not complete.

The failing `engine/items=16/workers=8` cell consumed **60 attempts total across
its repetitions**, not 60 per repetition: repetition one accepted once;
repetition two accumulated 55 quiet rejections and four execution-quality
rejections. Quiet records report selected-worker/SMT activity; executed
rejections exceed the unchanged 1% sibling-contention ceiling. The other two
execution-quality rejections occurred in an earlier cache-ack cell. No timeout
or workload correctness failure caused this stop, and the retained recognized
competitor snapshots do not identify the source of the activity.

**Zero C0/R0 throughput cells and zero S1/R1 performance cells launched.**
This is not a failure of the 0.80 floor or evidence of an S1/R1 regression.
Allocation parity, the complete throughput matrix, retained-memory and abort
integration, cache/soak/recovery acceptance and final performance qualification
remain outstanding. The independent S1 layout/code work completed after the
baseline stop without adding allocation or throughput attempts.

Normal and supervisory clock restoration passed; the final actual readback
matches all 64 original policies. Before this publication, the live-source
census matched all **10,077 project rows and five repository identities** to
fresh R1. The final container query found no Q9 containers. Only the current
campaigns' 24 functional and five allocation test databases were removed;
all raw measurements, attempts, rejected-quality evidence and command outputs
remain retained. A later ten-second read-only observation at restored clocks
reported the selected CPUs 100.00% idle at display precision. It neither
identifies historical contention nor certifies another qualification window.
No foreign process, service, IRQ, workqueue or security setting was changed.

The authenticated rollup is `validation/guard-v2-q09-closed-family-01.json`
(SHA-256 `9d01c33f7fc1c16e813c3190b3b0b049234d9c88294e4c9cc3f17d40c0d33601`).
It links every boundary, nonperformance result, bounded review, terminal
allocation audit, source check, restoration and cleanup receipt. Q9 is closed:
no retry, budget extension, historical reclassification or sample pooling.
Any additional diagnostic or qualification execution needs a separately
declared scope; the complete staged performance requirements are not waived.

**One-shot host-contention attribution (2026-10-12):**
The user authorized one nonqualifying R0 engine run with read-only CPU/task
sampling. `validation/guard-v2-q09-host-attribution-execution-01.json` declares
the separate scope; no Q9 source, protocol, campaign or prior receipt was edited.
Pinned-Docker preflight exercised both actual three-second observers and
11 output-validator cases. An initial preflight caught a shared-filesystem
lock-descriptor error before any Docker or observer launch; opening the existing
lock inode read/write corrected it without replacement or truncation.

The single quiet admission passed. Exactly one frozen **R0 release-allocation**
process ran with **eight workers, 16 items, 16,777,216 iterations and 65,536
warmups per worker**, the original placement and a 3600-second deadline.
It exited zero with `verified=true`, **134,217,728 commits** and **2,048 verified
keys**. There were no retries. All eight SMT siblings, CPUs 32–39, exceeded the
unchanged 1% execution-contention ceiling: **4.90–5.82%** across the guarded
process interval.

Independent `mpstat` captured activity on the same siblings. `pidstat` captured
two overlapping `go-ycsb` process groups, **TGIDs 2857686 and 2857808**, with
69 and 49 sampled threads respectively, owned by **UID 2001 / `zyang2`**.
Active thread last-CPU snapshots cover every guarded sibling. Sibling activity
rose during their captured lifetimes relative to the earlier benchmark
interval. The original known-runner detector recorded no recognized competitor;
its policy was not changed. Our benchmark was confined to Docker CPUs 0–31,
with sampled benchmark threads on 0–7 and 31; both observers stayed on 31.

This identifies overlapping foreign work in the new capture, not exact
per-CPU attribution of every tick or proof that the same processes caused the
earlier baseline failures. Task last-CPU snapshots are not execution traces,
full foreign commands/parent identities were not captured, and the observers
add helper-CPU overhead. No throughput penalty or candidate regression is
inferred. The one-shot token is consumed; it contributes **zero qualification
samples** and does not reopen Q9.

Normal and supervisory restoration and a final actual readback verify all
64 original clock policies. The private Docker daemon has no running containers.
No foreign process, affinity, service, IRQ, workqueue or security setting was
changed. Before any fresh complete qualification, coordinate a reserved window
covering the measured physical cores and their SMT siblings with the owner of
the overlapping work, or separately approve a dedicated-host plan.

Findings and authenticated raw-source references:
`validation/guard-v2-q09-host-attribution-findings-01.json`
(SHA-256 `28240ce706a32f77c94bf4765858516caa72ece95e6a3fd79e67739c94c2e14d`).
The 0.80 floor and every remaining staged acceptance requirement are unchanged.

**Q10 shared-z00 bootstrap and namespace verification (2026-10-13):**
The user selected **Proceed on shared z00** after disclosure of its running
Proxmox guests, Ceph/NFS services, all-64-policy clock impact and observed SMT
activity. `validation/guard-v2-qualification-10-user-decision.json` authorizes
fresh private provisioning and the complete matched sequence; it does not
authorize moving foreign work, changing global security or weakening quality
guards. The 0.80 floor, one-sided 95% confidence, original-stage R0 reference,
worker placement, workloads, process deadlines and finite attempt budgets remain.

The newly private `mako-sto-z00-q10-docker.service` runs on helper CPU 31 with
overlay2 and the original immutable image. Install/start/load receipts verify
the exported executable/library bytes, loaded image metadata, shared lock inode
and unchanged protected clock/security/network settings. No global packages,
alternatives, loader configuration or existing services were reconfigured.

Debian's host `/usr` is not used as the build sysroot. A coherent private copy
of the existing Ubuntu 26.04 userspace, alternatives, loader metadata and dpkg
database was installed under `/var/tmp/mako-sto-z00-q10/userland`; all **170,943**
declared file/directory/symlink entries matched the retained source manifest.
The original image remains the envelope. A separate **86,023-entry** source-only
import supplies offline Cargo sources, CMake/CTest 3.31.6 and the two declared
third-party runtime libraries, not old Mako/native outputs. The final import
left **27,440,844,800 bytes** on the local filesystem above the protected 8 GiB
reserve. Native build-peak capacity still requires monitoring.

Actual Docker execution loaded RocksDB 9.11.2 and libsystemd, and queried
production packages RocksDB **9.11.2-1**, DPDK **25.11-2**, and glibc
**2.43-2ubuntu2.4**. The existing system-Python analysis contract ran with
Python 3.14.4, NumPy 2.3.5 and SciPy 1.16.3. CMake/CTest 3.31.6, Rust 1.95.0
and Clang 22.1.8 executed. Initial bootstrap corrections are retained: an
incidental NumPy check used the wrong, Homebrew interpreter; the first native
asset staging rejected the separately required CTest path before final-root
creation. Neither failure launched a benchmark or consumed a quality attempt.

The Q10 factory now enumerates shared inputs, resolves native dependency
aliases, verifies source/configuration identities and queries package metadata
in the actual Docker userspace. The old host-only check failed on the absent
Debian `/usr/include/rocksdb/db.h`; the new host-to-Docker dispatch accepted the
authenticated private header. A real-filesystem smoke rejected eight relevant
mutations, including equal-byte symlink retargeting, added headers, optional
roots/configuration, runtime bytes and missing native dependencies. This is
bootstrap/namespace evidence, not fresh matched-boundary or performance evidence.

The unchanged typed service observer failed at `sd_bus_start` with
`Permission denied`. Independent Unix-socket creation failed before connecting.
The kernel audit identifies `docker-default`, `family="unix"`, `operation="create"`
and `info="failed protocol match"` under AppArmor parser 4.1.1 / kernel
6.17.13-2-pve. This matches the documented
[AppArmor network-ABI incompatibility](https://github.com/containerd/containerd/issues/12726).
No service absence-to-zero fallback, capability addition, AppArmor disablement
or parser/profile modification was made. The proposed correction is a uniquely
named, enforced Docker-compatible profile with a pinned compatible ABI, selected
only for Q10 containers—not a global parser change or replacement of existing
profiles. It remains subject to authorization and actual guard verification.

The fresh RocksDB TSan source archives and exact production package bytes are
prepared, but no new instrumented dependency or matched boundary is claimed.
No Q10 clocks were changed and no Q10 benchmark/qualification sample ran.
PMU, clock restoration, full preflight, matched builds and every nonperformance
and staged performance gate remain required. Q9 and its diagnostic stay closed.
The bootstrap rollup is
`validation/guard-v2-q10-private-bootstrap-rollup-01.json`
(SHA-256 `b2074f2b3994eaa3e1259cb963c88c5ca5907ada239b98850733f41039d70afb`).

**Q10 scoped AppArmor correction and service proof (2026-10-13):**
The user selected **Use scoped enforced profile**. Separate authorization
`validation/guard-v2-q10-apparmor-user-decision-01.json`
(SHA-256 `08b70b89670127a3b03c8fb894669e0eca426f5d9a02123b325e42c517d9c050`)
permits only a uniquely named, enforced Docker-compatible ABI-4.0 profile for
Q10 containers. It does not replace the original shared-host authorization.

The selected template is Docker's actual `github.com/moby/profiles/apparmor`
**v0.1.0**, commit `c936cc7b4074219137bc0bee45670f5e4618d462`; its complete
template bytes occur exactly once in the authenticated installed daemon.
Standard rendering preserves every rule, substitutes the unique
`mako-sto-z00-q10` name and observed daemon peer, and prepends `abi <abi/4.0>,`.
The host parser compiled and added that one profile with cache writes disabled
and **without replacement**. Before/after evidence confirms all **123**
pre-existing profile identities, global parser/macro inputs, protected sysctls
and all 64 normal clock policies were unchanged. The new policy is enforcing.

Both Q10 launchers now select that profile. Actual Docker inspection confirms
the enforcing label, `network=none` and unchanged sole added capability
`CAP_PERFMON`. Unix-socket creation and connection to `/run/systemd/private`
succeeded. The unchanged observer returned four freshly queried UInt32
`NRestarts=0` values across a deliberate unsupported-property error, closed its
failed peer and reconnected. Journal observation returned no records in its
actual two-second interval. A read-only open of `/proc/sysrq-trigger` remained
denied; no read or write was attempted. No absence-to-zero fallback or service
mutation was introduced. Preflight, frozen boundaries and campaigns retain the
authorization, exact policy, upstream provenance, load receipt and installer.

Resolution evidence:
`validation/guard-v2-q10-scoped-apparmor-resolution-01.json`
(SHA-256 `c8622afb10b326c16605bb0b250ae510e51e1efd70efaf68270ca7ebe3e57bdd`).
Fresh RocksDB TSan preparation has now launched in Docker. This is not a
qualification result: clock/PMU proofs, full preflight, all fresh matched
builds, nonperformance gates and the complete staged performance sequence
remain mandatory. No Q10 clock-control execution or benchmark sample is claimed.

**Q10 dependency, preflight and actual host controls (2026-10-13):**
The fresh Docker TSan rebuild of distro-patched RocksDB **9.11.2-1** completed,
followed by separate immutable identity verification. Its receipt retains
2,130 source files, 15 explicit inputs and 139 runtime identity rows; every
shared translation unit is instrumented, the libstdc++/SONAME-9.11 ABI and
production C exports are preserved, and no second TSan runtime is bundled.
The selected DSO SHA-256 is
`93ff74aee9f9a44efcdfe58fd658be10cac87800627963f6e5871da578188baa`.
Receipt `guard-v2-q10/dependency-artifacts/rocksdb-9.11.2-1-thread-01/receipt.json`
(SHA-256 `8a4d98bc436f813e9a61c2f7f7c73f40c68e96dee7db08b207505f4b2a8fe2f4`)
was pinned before preflight; no historical instrumented binary was adopted.

Full Docker preflight passed **91 tests**, **33 help CLIs**, eight analyzer
CLIs, 45 Python syntax compilations and the full correctness-inventory CLI.
Its 61-source closure stayed unchanged, including the separate AppArmor
authorization and policy provenance. Retained R0/S1/R1 reconstruction and
unchanged full-work/statistical contracts passed. A separate actual Docker
source-controller smoke verified 10,173 source files across five repositories
and all three authenticated source parents, with unchanged source identities.
Its exact throwaway source is retained in the launch receipt, not left as an
extra runnable script.

The integrated 30-worker host smoke stopped **before launching either PMU
worker**: its two-second selected-worker/sibling quiet observation rejected CPU1,
which recorded **29.4416% execution** against the inclusive **5% preflight**
ceiling. This is a failed quiet observation, not a hardware or benchmark pass;
the **1%** idle-sibling ceiling applies during measurement, not this preflight.
No foreign workload was identified or altered from that CPU-only evidence.

Independent hardware checks exercised real grouped `cycles:u` and
`instructions:u` syscalls on CPUs0/29. The fixed-2GHz positive scope exited0,
with PMU running ratios **1.0/1.0** and observed off-CPU fractions
**0.0010589915/0.0000324987**. The deliberate exit23 scope also completed as
designed. All three clock scopes—the initial quiet rejection, deliberate
failure and independent hardware success—verified exact normal and supervisory
restoration of all64 original policies. Postcheck found no private containers,
unchanged protected controls and 27,436,560,384 local free bytes.

These controls do **not** reclassify the failed quiet interval or qualify any
performance sample. The approved shared-host policy still applies each
selected-worker cell's original bounded preflight, measured SMT/off-CPU/PMU,
service and correctness gates; an extra all30-core admission veto over smaller
cells is not introduced. Source/native builds are independent of transient
quietness and make no measurement-quality claim.

Evidence: `validation/guard-v2-q10-policy-validation-01.json` and
`validation/guard-v2-q10-host-controls-validation-01.json`
(latter SHA-256 `5216354e035e139f4a24513ca902a16b05605a54bd7aa598567d0293ff8ef998`).
No transaction benchmark or Q10 qualification sample has launched.

**Q10 fresh matched builds and nonperformance checkpoint (2026-10-13):**
All three boundaries were rebuilt in the private Docker namespace:
`R0-24-guard-v2-q10` (25 binaries, nine attestations, 13 commands),
`S1-14-guard-v2-q10` and `R1-16-guard-v2-q10` (17 binaries, five attestations,
11 commands each). The prerequisite native bootstrap was not a qualification
arm; every frozen arm rebuilt afterward. Capacity supervision retained the
8-GiB local reserve and 9-GiB brake. No historical native output was adopted.

`validation/guard-v2-q10-matched-inputs-01.json` authenticates all three
finalized receipts: 9,881 noncrate source files, five repository identities,
224 common native rows, 82,266 shared inputs and 507 immutable inputs match.
The sole declared native difference is each stage's `libsto_tpcc_ffi.a`;
Docker recipes differ only by their exact read-only source-view bind.
The new R0 library bundle verified 400 live dependency files in Docker;
zero replacement mounts were necessary. This is not a claim of 400 new copies
or reuse of an old bundle.

Fresh executions completed:

- **570 functional cases**, all successful and `verified=true`, across eight
  C0/R0/S1/R1 profile jobs. The 24 owned smoke databases were removed after
  provenance checks. These functional rates are not qualification timings.
- **14 correctness gates**, 19 commands and six fresh reports, with all 15
  complete source checks passing. Native inputs and host/live sources were
  unchanged after the private source mounts were removed.
- **Miri, address, undefined, thread and normal-Loom** gates in full, using
  the new Q10 RocksDB TSan dependency. Every command/output receipt was
  authenticated; dependency and source restoration checks passed.
- The actual public example compiled and printed
  `committed balances: from=90, to=60`. The post-correctness source/repository
  census and raw report integrity checks passed.
- Separate fresh **R0/S1 and R0/R1** layout/code collections in release and
  ThinLTO. All 16 representative fixture roles retain size and alignment
  in each comparison/profile; hot data is 56 bytes, owner and batch 128.
  These are fixture sizes, not universal item sizes or allocator overhead.
- Four independent bounded manual reviews inspected the actual surviving
  commit, cleanup, abort, committed-poison and recycling paths, with relevant
  frozen source and consumers. They found no introduced allocation, payload
  copy, atomic/reference-count or dispatch mechanism in that inspected scope.
  ThinLTO recycling was inspected in its actual callers, not a fabricated
  standalone symbol. Existing intent/adapter cleanup and identity decrements
  remain path-dependent work; conditional whole-frame destruction is not
  unconditional per-commit work. Production `panic=abort` means source
  `catch_unwind` does not establish actual Rust-panic recovery. Uninspected
  callbacks, transitive code and monomorphizations remain outside the verdict.

The final host check found the same boot, private daemon and image, no private
containers, the exact enforced AppArmor identity, unchanged protected controls,
all 64 normal clock policies, 13,905,924,096 local free bytes and
1,596,691,841,024 bytes free on the workload filesystem. Functional clock
restoration was exact through both normal and supervisory paths. The exercised
throwaway S1 client was removed; its exact source and executed copy remain
archived. No frozen production, harness, protocol or authorization input was
edited for these collections; this documentation checkpoint follows their
complete source checks.

Rollup: `validation/guard-v2-q10-nonperformance-rollup-01.json`
(SHA-256 `b8701728daf9af7397c272750251b78a6d0caa51e9930a3610f9553b0c692f38`).
This is dirty-local nonperformance evidence, not clean-commit or performance
qualification. `validation/guard-v2-q10-baseline-launch-01.json` records the
now-launched fresh baseline: 45 allocation samples/profile, 30 C0/R0 singleton
cells/profile, six clean blocks, the unchanged shared 60-attempt cell ceiling
and fatal 3,600-second process deadline. All per-sample guards remain required;
the earlier rejected all-30-worker quiet smoke stays rejected and creates no
extra all-topology admission veto. No baseline acceptance or S1/R1 performance
execution is claimed here.

**Q10 terminal baseline quality-budget failure (2026-10-13):**
The declared baseline exited with failure after the release allocation cell
`engine/items=1/workers=30` consumed its shared **60-attempt** budget at the
first repetition. This is a terminal quality failure, not an observed
throughput regression or a correctness failure. The 0.80 floor was never
evaluated. No retry, budget extension, guard relaxation or sample pooling
is authorized by this record.

The complete journal contains **65 attempts**:

- One worker: one accepted first-repetition sample.
- Eight workers: three measured-quality rejections, then one accepted
  first-repetition sample.
- Thirty workers: **38 quiet-preflight rejections** without workloads and
  **22 measured-quality rejections**, with zero accepted samples.

All **27 actual workloads** exited zero, reported `verified=true`, and passed
the exact configuration, commit, acknowledged and final-key checks. Their
raw stdout/stderr hashes and measurements were authenticated; all 65 journal
entries match the individual attempt receipts. The 25 measured rejections
reported SMT-sibling contention above the unchanged **1%** ceiling; quiet
rejections used the unchanged **5%** selected-worker/SMT admission ceiling.
These CPU observations do not identify which foreign task or kernel activity
caused the contention. The two accepted R0 allocation observations are partial
evidence only; neither completes its required three-repetition cell, and
rejected observations remain rejected.

No C0/R0 throughput comparison, ThinLTO allocation collection, S1 performance
campaign or R1 performance campaign launched. The full allocation, retained
memory, abort, GC/soak/recovery and statistical gates remain mandatory.
The nonperformance checkpoint above remains valid within its stated scope;
it does not override the closed baseline or qualify either migration stage.

Normal and supervisory clock restoration both passed, with exact before/after
equality for all 64 policies. A separate final host check confirmed no private
containers, the same enforced AppArmor identity, unchanged protected controls
and 13,895,421,952 local free bytes. Every frozen campaign-input hash still
matches. No foreign work or service was stopped or reconfigured.

Closed rollup: `validation/guard-v2-q10-baseline-closed-rollup-01.json`
(SHA-256 `3d728b8c301240fe6b891884884a858037613d8553dafe35bb0fcbac9df49ca3`).
Raw evidence remains under `guard-v2-q10/campaigns/R0-01/`.
Further qualification needs a separately authorized successor context;
Q9, its diagnostic and Q10 remain closed.

**Q10 one-shot guest-contention attribution (2026-10-13):**
The user selected **Authorize one bounded diagnostic**. The separate declaration
`validation/guard-v2-q10-host-attribution-execution-01.json` retained the exact
failed R0 release-allocation recipe: 30 workers, one item, 16,777,216 iterations,
65,536 warmups, one quiet admission, at most one workload and a 3600-second
process deadline. No frozen helper, campaign, protocol or production source
was edited. Actual pinned-Docker preflight completed both three-second observers
and 11 output-validator cases using retained output only as parser input.

The **single quiet admission was rejected**. Its token is consumed; **zero
workload processes** launched and no retry occurred. During
16:34:22.928–16:34:25.273 UTC, exact CPU counters exceeded the inclusive 5%
admission ceiling on these guarded CPUs:

| CPU | Role | Contention ticks / total | Contention |
|---|---|---|---|
| 0 | worker | 13 / 194 | 6.7010% |
| 9 | worker | 10 / 199 | 5.0251% |
| 35 | worker 3's SMT sibling | 13 / 198 | 6.5657% |
| 50 | worker 18's SMT sibling | 14 / 196 | 7.1429% |

Independent `mpstat` samples show guest execution on all four CPUs, including
13.27% on CPU 0, 8.08% on CPU 9, 11.22% on CPU 35 and 10.31% on CPU 50 in
nearby one-second intervals. `pidstat` identifies overlapping **UID 0,
TGID 1016020, `kvm`** threads: `CPU 2/KVM`, `CPU 5/KVM`, `CPU 0/KVM` and
`CPU 7/KVM` have active last-CPU snapshots on 0, 9, 35 and 50 respectively.
The retained rows also show other KVM, firewall, NFS and kernel activity.

Both observers remained on CPU 31. Its 25.63% observed contention was
**not an admission veto**: helper-only activity remains observational.
The observers and artifact I/O add overhead. Thread last-CPU snapshots are
not execution traces, and their intervals do not exactly match the guard
bookends; the capture does not assign every busy tick or prove the causes
of earlier Q10 failures. Full foreign commands, VM IDs and parent identities
were not captured. `/proc/interrupts` returned zero bytes and remains
unavailable; no security setting was changed to expose it.

Normal and supervisory restoration, plus a separate actual final readback,
verified all 64 original clock policies, the enforced AppArmor identity and
unchanged protected controls. No private containers remained. No foreign
process, guest, affinity, service, IRQ or workqueue was modified.
The exercised external client was removed; its exact source, executed copy
and every raw observation remain archived.

Findings: `validation/guard-v2-q10-host-attribution-findings-01.json`
(SHA-256 `117e8a1df075d7acb6135b2fb1d109cf95f32546b1b88383274aeeb17f1046cf`).
This contributes **zero qualification samples** and no throughput, allocation
or candidate-regression result. Q9, its diagnostic, Q10 and this diagnostic
are closed. Fresh complete qualification needs a separately authorized quiet
reservation covering the measured physical cores and their SMT siblings,
or a dedicated host; other users' guests/work must not be stopped implicitly.


### Stage 2: Familiar source organization and datatype walkthrough

1. Move the object protocol from `adapter.rs` to `object.rs` and private item
   implementation from `item.rs` to `trans_item.rs`, updating imports and
   path-sensitive tooling together. `transaction.rs`, version/lock helpers,
   direct commit, and singleton specializations retain their responsibilities.
2. Organize the object protocol's explanation around the C++ responsibilities:
   lock planning/acquisition, predicate checking, read checking, install,
   unlock/publication, cleanup. Keep the actual safety-driven phase separation;
   add no forwarding facade, supertrait stack, or generic all-powerful item view.
3. Use the existing `TxnCell` and `TxnCounter` as the author-facing examples.
   A side-by-side walkthrough must identify the same item lookup, RYW, staged
   write/delta, lock, check, install, and cleanup responsibilities as C++ TBox
   and TCounter. Explicitly mark Rust's missing counter-predicate and opacity
   capabilities rather than claiming equivalent feature sets.
4. Update the architecture's main narrative and detailed API appendices to the
   actual new names. Keep advanced direct/batch/token capabilities in their
   reference sections; they remain available to real external adapters.
5. Repeat focused correctness and performance sentinels against R0. Module moves
   can affect monomorphization, linking, and placement even without a new branch
   in source; investigate a change rather than assuming a rename is zero-cost.

**Exit:** a reader can navigate the familiar object model in source and docs,
without a new stored wrapper, erased adapter layer, or loss of specialized lanes.
The code remains a single Rust implementation, not C++ emulation on top of it.

**Current source evidence, not stage acceptance:** LSP file moves updated
`object.rs`/`trans_item.rs` and their callers without compatibility aliases.
The protocol rustdoc and architecture now explain the existing phase authority
and side-by-side cell/counter lifecycles, including unsupported counter predicates
and opacity. All 19 UI rejection contracts survived the module moves; six
compiler-generated snapshots changed for the new source/module paths.
The complete architecture transfer example ran in pinned Docker and printed
`committed balances: from=90, to=60`
(`diagnostics/1790795048683895069-R1-public-example/result.json`).
Its initial operator invocation used Cargo's library-output directory rather
than its declared dependency directory; that failed compile is retained, and
the corrected invocation reused the successful library build without changing
the example or production source. R1 freeze, full correctness/instrumentation,
module-boundary layout/code checks and every deferred timing gate remain
required.

`R1-01-canonical-source` is a closed failed freeze: native compilation/linking
completed, but module discovery regenerated CMake's graph inside the protected
rebuild boundary. The unchanged guard rejected the glob stamp's changed identity;
clock restoration was exact. Explicit pre-freeze configuration settles those
globs and recomputes derived Python-interpreter properties so CMake clears its
empty cached-discovery failure annotation.

`R1-02-canonical-source` is also closed and failed. Its protected native rebuild
completed, but the unchanged reference-recipe comparison rejected a second
configuration's added `-I/usr/include` in 272 of 446 commands. The project consumes
`LIBEVENT_CFLAGS` before rediscovering libevent, so an existing cached value changes
that initial-configuration recipe. The native identity object and `libmako.a`
also differed; the other 222 shared native inputs matched. Clock restoration
was exact. A real CMake rediscovery of those derived libevent entries restored
the complete reference cache, environment, and exact expanded-command digest
(`diagnostics/1790799997398639296-native-configuration-settle-control/result.json`).
The next fresh freeze must still prove all native/shared-input checks.
No rebuild guard, native recipe comparison, reference artifact, compiler flag
requirement or acceptance threshold is waived.

`R1-03-canonical-source` finalized successfully: 17 binaries, five attestations,
11 successful build commands, and exact equality of the reference native
recipe/environment and 224 shared native inputs. Its 180 frozen functional
smokes passed across release and ThinLTO with exact worker placement; these are
not performance samples. The full correctness run then passed six
core/reference/history and doctest gates before rustdoc rejected two stale
`crate::adapter` links in `direct_commit.rs`. Those links now use the existing
public root exports; the unchanged `-D warnings` documentation gate passed
(`correctness/R1/1790804696881049700/result.json`). The successful freeze and
failed qualification receipts are preserved, but this source correction requires
a fresh R1 freeze and the complete final gate set. No warning suppression or
partial-gate qualification is substituted.

`R1-05-outlined-source` now completes current-source qualification, not performance
acceptance. It retains 17 binaries and five attestations after 11 successful
build commands. Its finalized receipt SHA-256 is
`1e1f28d56500fe902754f8732c9236f458a0b880c2f53010113659d44427d553`.
Against R1-04, the only production-source delta is the private outline attribute;
the other two content changes are documentation.

All 180 release/ThinLTO functional cases and the full 14-gate correctness
selection pass: 19 successful commands and six fresh reports. Miri, full ASan,
native UBSan, TSan and the separate eight-model normal Loom run also pass.
All 22 watched source files match before/after each instrumentation gate.
The sanitizer qualification remains `dirty-local-diagnostic-unchanged`, not
clean-commit qualification; Miri does not execute C++ and native UBSan is not
Rust UBSan. The original leak/quarantine qualifications remain unchanged.

All 16 private fixture size/alignment roles match immutable R0 in both profiles.
Bounded release and ThinLTO commit/cleanup/frame-recycling reviews find no
unexplained added allocation, copy, atomic or dispatch work in the inspected
mechanisms; they do not establish whole-program equivalence or erase the
outlined fallback's real call boundary. The current architecture example runs
and prints `committed balances: from=90, to=60`. A complete post-execution census
matches 10,111 files and five repository identities with no added, removed or
changed entries; clock restoration is verified. This ledger addition follows
that census and changes neither the exercised code nor the example.

See `validation/R1-05-source-qualification-complete.json` for the bound receipts,
including `instrumentation/R1/outlined-source-01/result.json` and the separate
release/ThinLTO mechanism reviews. The final orchestration now points to this
exact candidate and retains every original R0/C0 comparison matrix
(`validation/R1-05-final-orchestration-readiness.json`); it has not collected
final performance data. The unresolved S1 comparison, missing S1 cells and all
final R1 gates still block stage and overall acceptance.

### Stage 3: Full correctness and matched performance qualification

1. Freeze **R1**, the complete candidate. Run the existing required correctness,
   native, history, differential, recovery, mutation, compile-fail, and applicable
   sanitizer/Miri gates in Docker after all source changes settle.
2. Measure R0 versus R1 on every required lane below. Compare R1 with C0 where
   workloads and semantics match. Use the same reference binaries/build inputs
   and collect balanced raw samples; do not compare a new run only with a table
   from an older report.
3. Exercise both the campaign's explicit ThinLTO profile and the workspace's
   production release profile. Neither an LTO-only win nor a different panic,
   checksum, payload, feature, or logging profile qualifies the other profile.
4. Apply per-cell regression gates. Retain checksums/final-state checks, aborted
   attempts, memory observations, and measurement-quality evidence with the
   throughput data. Failed correctness or unsupported arguments invalidate a
   sample; they cannot be counted as a completed workload.
5. If a regression appears, bisect the vocabulary and module-organization
   boundaries, attribute it, and change only the offending refactor. Do not
   compensate by weakening validation, narrowing the workload, changing default
   profiles, or adding an unrelated optimization.

**Exit:** exact-candidate gate results and a reproducible R0/R1/C0 report with no
unresolved regression or missing required coverage. Implementation presence,
a successful compile, or a singleton score alone is not acceptance.

### Stage 4: Complete the source cutover and record acceptance

1. Confirm that obsolete selected API names, module paths, and compatibility
   aliases are gone as callable APIs or authoritative current-use examples.
   Explicit former-to-current migration tables may retain the former names. Historical
   result artifacts keep their original bytes and names; mark their lineage,
   not a fake migration of archived evidence.
2. Verify that examples, adapter-author guidance, mutation drivers, public
   reexports, and external-crate fixtures use the canonical surface. Keep
   semantic tests; remove throwaway probes from the repository while retaining
   their exact sources and results in the qualification artifact.
3. Publish the final concept map, intended Rust-specific differences, supported
   profiles, per-cell performance results, and any explicitly unclaimed metrics.
   No production default or ABI/record-format change accompanies this refactor.

**Exit:** familiar concepts, one API/protocol, unchanged advertised behavior,
measured non-regression, and no temporary migration scaffolding.

## Work ownership and safe parallelism

One integration owner controls `sto-core` exports, trait signatures, item state,
and phase machinery. Consumer migrations can be split by crate only after that
exact symbol/signature map is fixed; no two workers edit the shared core boundary.
Docs and isolated consumer updates can proceed concurrently. Builds, formatters,
linters, tests, and benchmarks run centrally after each edit batch settles, not
against one another's half-migrated sources.

Use symbol-aware Rust references/rename when available. This environment currently
has no Rust LSP; an implementation run must either enable one or use syntax-aware,
qualified edits plus a complete compiler-checked caller census. Global text
replacement of `Entry`, `finish`, `check`, or `item` is not an acceptable fallback.
Each accepted boundary must build without old-name shims. Roll back only the
migration-owned change when necessary; never reset unrelated user work.

## Migration inventory

Source inspection found seven concrete non-test implementations of the object
protocol: core `CellAdapter`, `ArrayAdapter`, `CounterAdapter`; Masstree
`TableAdapter` and feature-gated `FixedAdapter`; and the external reference
`HashMapAdapter` and `SnapshotAdapter`. Ten additional fixture implementations
exercise the contract. Recount at execution time; this is a bounded initial
inventory, not permission to skip new callers.

| Area | Required work / boundary |
| --- | --- |
| [Core exports](../../crates/sto-core/src/lib.rs), object/item/runtime/transaction modules | Definitions, generic bounds, root and public-module paths, typed/erased dispatch, scoped operation APIs, doctests |
| [Direct plans](../../crates/sto-core/src/direct_commit.rs), [shared protocol](../../crates/sto-core/src/direct_protocol.rs), [singleton](../../crates/sto-core/src/singleton_direct.rs), [terminal reads](../../crates/sto-core/src/terminal_read.rs) | Migrate type references without changing concrete provider dispatch or borrowed-versus-owned state |
| [Core datatypes](../../crates/sto-core/src/txn_cell.rs), array/counter, [Masstree](../../crates/sto-masstree/src/lib.rs), [fixed-u64](../../crates/sto-masstree/src/fixed_u64.rs), [reference datatypes](../../crates/sto-test-datatypes/src/lib.rs) | All seven real implementations and their typed operation helpers |
| [Public adapter fixture](../../crates/sto-core/tests/public_adapter_contract.rs), transaction/local-version/failure fixtures and UI support | All ten fixture implementations; retain two resource classes sharing one physical lock and exact-binding tests |
| [Compile-fail driver](../../crates/sto-core/tests/compile_fail.rs) and `tests/ui` | Both root/public-module imports and diagnostic paths; a missing renamed import must not substitute for the intended borrow/phase error |
| [Frame reuse](../../crates/sto-core/src/frame_reuse_tests.rs) and [allocation behavior](../../crates/sto-core/tests/frame_allocations.rs) | Preserve observable pooling/ownership behavior; not a universal zero-allocation claim |
| [Upper Rust engine](../../crates/mako-engine-sto), [closed TPC-C FFI](../../crates/sto-tpcc-ffi) | Mostly behavioral consumers, not direct users of the renamed trait/proxy; rebuild and exercise, do not invent unnecessary API edits |
| [CMake](../../CMakeLists.txt), [quarantine manifest](../../scripts/ci/rust_sto_quarantine_tests.txt), mutation/native-health drivers | Recursive source globs already discover moved Rust files; exact test-name allowlists and source fingerprints remain auditable |
| [Architecture](../architecture/rust-sto.md) and dependent current docs | New canonical symbols, real callback signatures, item representation explanation, and updated local anchors/paths |

Keep existing test function identities unless their meaning actually changes.
A `finish`-named failure case can still test cleanup; renaming it unnecessarily
would disturb exact quarantine/Miri/sanitizer selectors. Preserve diagnostic
phase meanings and values rather than expanding this into an error-taxonomy
redesign. Incidental wording/private-layout tests do not earn new permanent
assertions: retain observable ownership/allocation and real ABI/safety contracts,
and use measured artifacts for private footprint comparisons.

## Correctness gates

All runtime tests and probes execute in Docker. Use the existing configured
native build with the pinned Rust toolchains and real native archives; do not
run host `ci.sh`. Gate existence below is source evidence, not a claim that the
planned candidate has passed it.

- Core/reference gates cover transactions, public external adapters, mixed
  resources, predicate upgrade, local/ordered versions, hooks, failures, frame
  reuse/allocation, strict-serializability histories, and compile-fail/rustdoc
  lifetime and typestate contracts. Preserve failures for their intended reason.
- `rust_masstree_native_integration` checks the safe wrapper, transactional tree,
  history oracle, fixed-u64, and closed TPC-C integration against real archives.
  Native registrations are process-lifetime; preserve its process isolation.
- `rust_sto_integration` is the build half of a larger gate. Its native target
  dependencies compiling is not evidence that their registered CTests ran.
  Execute the corresponding runtime CTests through the existing CI orchestration.
- Selected upper gates include `rust_sto_cache_integration`,
  `run_mako_engine_sto_tests`, `run_mako_engine_integration_driver_tests`,
  `run_mako_engine_sto_mutation_tests`, `run_mako_engine_cross_reopen_tests`, and
  the C ABI reference's `run_mako_local_rust_tests`. Cross-engine reopen is a
  functional record/recovery check, not a performance measurement.
- Preserve public native symbol/header/fingerprint checks and closed TPC-C
  exit/capacity/worker/topology rejection cases. No new opacity, replication,
  or multi-shard support is inferred from renamed interfaces.
- Run [Miri](../../scripts/run_rust_sto_miri.sh) and the applicable
  [sanitizer gates](../../scripts/ci/run_rust_sto_sanitizer.sh) with their pinned
  toolchains and exact leak/quarantine qualifications. Miri does not execute C++;
  native UBSan is not Rust UBSan; focused cache sanitizers do not instrument
  system RocksDB. Keep the separate Loom cases where sanitizer stack switching
  is unsupported. Never widen suppressions to make the migration pass.

Core/history feature matrices and selected-engine matrices remain separate.
Blanket workspace `--all-features` is invalid because `engine-sto` and
`engine-c-abi` are mutually exclusive. Correctness builds with failure/observer
hooks must not be timed as production binaries.

## Historical performance evidence and unfulfilled coverage

### Baselines and honest comparison

The [September 23 record](../performance/mako-cache-rust-sto-singleton-attribution-2026-09-17.md#10-production-fixes-and-scaling-verification-2026-09-23)
is a useful reference, not the new baseline: existing-key singleton engine
updates, 256 disjoint keys/worker, 128-byte values, checksum disabled, explicit
ThinLTO. It reports 29/32 counts within ±3%, 30/32 no more than 3% behind C++,
and a median deficit of 1.89%; 12 and 13 workers remain 3.18% and 3.11% behind.
Those are median estimates, not confidence bounds. Broader logged/cache/TPC-C
parity was not established by that result.

R0 versus R1 is the primary regression comparison. C0 shows whether the existing
C++ gap changes. Do not derive Rust-versus-Rust performance by subtracting two
independent noisy comparisons to C++. Do not reset R0 after each stage and allow
small regressions to compound. No new general C++ parity claim is required or
permitted merely because this naming refactor completes.

### Required lane matrix

For the authorized zoo-004 Q6 campaign, use worker counts 1/8/30 for early
engine/core sentinels; final singleton qualification covers every count 1–30.
Final broader core/point/cache coverage includes 1/4/8/16/30 where supported.
Workers0–29, writeback30 and helper31 occupy distinct physical cores. Counts31
and32 are explicitly omitted, not silently qualified by the older host's data.
TPC-C keeps its actual 1/2/4/8/16 paired range. Historical 32-worker records
above retain their original scope and do not qualify this relocated campaign.

| Lane | Existing mechanism | Required distinction / missing support |
| --- | --- | --- |
| Existing-key singleton engine | [engine_compare](../../crates/mako-cache-bench/src/bin/engine_compare.rs), [Rust variants](../../scripts/run_mako_engine_variants.py), [C ABI pairs](../../scripts/run_mako_engine_compare.py) | Direct R0/R1 plus frozen-C0 comparisons; preserve all final keys/counts; 128-byte engine-only result |
| Homogeneous scalar/core and multi-item engine | [sto_core_array_bench](../../crates/sto-core/src/bin/sto_core_array_bench.rs); `engine_compare engine --items N` | Exercise 1/4/16 items, reads and writes; the paired engine scripts do not currently forward `--items` |
| Binary and optional fixed-u64 point batches | [point driver](../../scripts/run_sto_masstree_compare.py) and [native point binary](../../crates/sto-masstree/src/bin/sto_masstree_compare.rs) | Eight-byte values and eager registry; all-read attempts select terminal reads, not ordinary read-item commit |
| Mixed adapters and representation transitions | Existing correctness/history suites; thin R0/R1 probe required | Exercise core cell + counter + external map composition, scalar/batch mixing, homogeneous-to-heterogeneous materialization, and reuse; no current isolated throughput driver |
| Ordinary reads, misses/mutations, scans, and payload tiers | Existing native semantic/history fixtures; thin R0/R1 probe required | Distinguish ordinary from terminal reads; scalar misses/insert/remove/RYW; forward/reverse and trusted/ordinary scans; 8/38/39/128/129/160/161/1024-byte payloads and relevant transitions |
| Engine plus log and foreground ACK | `engine_compare` modes `engine-logged` and `cache-ack` | Singleton only; checksum-none foreground diagnostics; ACK ends without drain/reopen and is not sustained or durable throughput |
| Broader cache work | [cache benchmark](../../crates/mako-cache-bench/src/main.rs) acceptance/scaling profiles | Multi-key 1/4/16/64 operations and read/write/RMW where actually supported; compare selected STO engines, not weaker MRX/raw RocksDB semantics as if equivalent |
| Sustained application and recovery | [paired soak](../../scripts/run_mako_engine_soak.py), [gc_soak](../../crates/mako-cache-bench/src/bin/gc_soak.rs), [protocol](../../crates/mako-cache-bench/gc-soak.md) | Bounded queues, production CRC32C, full drain and fresh-process exact recovery; a short smoke is not the full 960-second GC protocol |
| Local TPC-C | [TPC-C driver](../../scripts/run_sto_tpcc_compare.py) and closed bridge | Single shard, no replication; driver ceiling 18 warehouses/workers is stricter than Rust bridge admission; official tpmC/distributed performance is not claimed |

The thin probes are bounded measurement fixtures, not a new benchmark framework
or new production adapters. Reuse public datatype APIs and existing fixture
workloads; compile identical probe logic against R0 and R1, vary data rather
than timing a constant-foldable no-op, validate the observable result, and archive
source/configuration with the samples. They are required coverage work in Stage
0, not already existing gates. Ordinary/direct/terminal and registry/value-policy
selection must be evidenced rather than inferred from the workload's name.

### Runner gaps to close before baseline capture

1. `run_mako_engine_variants.py` accepts only Rust engine results; a C ABI binary
   is not a third variant. Its timing controls are fixed, whereas the C ABI-pair
   driver exposes iterations/warmup/perf. Reuse these mechanisms for direct
   Rust comparison and frozen-C++ pairs, but add only the missing orchestration
   and option forwarding needed to give each comparison matched controls.
2. Neither checked-in engine runner accepts shifted CPU placement, `--items`,
   arbitrary payload sizes, scans, or mixed adapters. `engine_compare` itself
   supports 1–256 items only in `engine` mode; it rejects multi-item logged/ACK
   requests. Use the separate cache matrix for those workloads, not invented
   flags or a renamed singleton result.
3. The September campaign's shifted worker CPUs 32–63 and per-sample SMT/off-CPU
   filtering are in external `/var/tmp/mako-gap-fix.py` artifacts on zoo-002,
   not flags of the checked-in runners. Verify and archive the retained runner
   and probe sources if available; otherwise implement the necessary controls
   in the existing runners before freezing measurement builds. Do not claim a
   standard invocation reproduces that campaign.
4. The point driver accepts one C++ and one Rust binary, not two Rust variants.
   TPC-C's driver accepts one combined binary and selects its embedded backend.
   Direct R0/R1 measurements and an identical frozen C0 executable therefore
   need small orchestration changes; unchanged C++ source/native archives are
   not the same claim as an identical executable. Preserve existing validation
   and environment guards rather than constructing a replacement framework.
5. `gc_soak` can vary value sizes at its binary interface, but the paired runner
   fixes 128 bytes. Keep isolated tier probes separate from the sustained
   profile. The cache acceptance matrix also has different queue and timing
   semantics from foreground ACK; retain separate results.

Every harness change precedes R0/C0 capture and is held identical for R1. A
harness/probe defect requires re-establishing both comparison arms, not patching
only the candidate. Driver-test success validates orchestration, not throughput.

### Build profiles

- Workspace production release: Rust 1.95, opt-level 3, one codegen unit,
  `panic=abort`, **LTO off**. Do not silently replace it with the campaign build.
- September-style comparison: explicitly matched ThinLTO, frame pointers and
  debug information, real native ABI, identical native archives and allocator
  inputs. Historical zoo-002 used `target-cpu=znver1`; the authorized fresh
  zoo-004 Q6 campaign uses `target-cpu=broadwell`. Record image digest/identity,
  not just `latest`; no cross-host timing reuse.
- Closed TPC-C: its CMake static library uses **fat LTO and panic=unwind**;
  exception containment remains observable there. Preserve that profile,
  with explicit Broadwell targeting for Q6.
- PGO is separate: if claiming the deployed PGO profile, regenerate comparable
  training for R0 and R1 under one pinned policy. Renaming functions/modules can
  invalidate old profile matching; silently reusing an old profile is not fair.
- Allocation instrumentation is a separate diagnostic build, not the timing
  binary. Disabled counters are zero by construction. Rust worker allocation
  counters omit native/background allocations, frees, live bytes, and RSS.

### Acceptance rules and stop conditions

The intent is no regression. The user-authorized successor campaign uses **20%
as the maximum throughput non-inferiority margin** against the original Rust
baseline, not permission to compound 20% at each stage or hide added hot-path
work. Every executed 3%, 5% and 10% campaign, and the prior 15% policy, retain
their original declarations and outcomes. The revised margin requires a newly
frozen policy and fresh matched data.

1. Freeze workload cells, iteration counts/durations, seeds, order, filters,
   repetition count and confidence method before timing. Cap each cell at 60
   attempted balanced comparison blocks; any larger campaign requires a newly
   declared protocol and fresh matched arms, not selective continuation.
   Use at least six clean balanced comparison blocks per final acceptance cell.
   Additional rounds may repair missing clean coverage under the frozen policy,
   never continue merely until a desired winner appears. Hitting the bound with
   inadequate precision is **inconclusive**, not a pass.
2. For each cell/profile, compare paired R1/R0 throughput ratios. Use a
   one-sided 95% lower confidence bound for the geometric mean paired ratio:
   take log ratios and use a Student-t bound with `n - 1` degrees of freedom,
   then exponentiate the lower endpoint. Independent process-run blocks are
   samples, not millions of operations within one run. This estimates measured
   run variability under the log-ratio model, not all-host performance. The
   lower bound must be at least 0.80. Report medians, raw observations, uncertainty and
   every exclusion; do not substitute a whole-matrix average for a slow cell.
3. In singleton cells where fresh R0 meets the C++ budget or its confidence
   bounds straddle 0.80, require candidate R1/C0 one-sided 95% lower bound
   ≥0.80. Candidate upper bound <0.80 fails; candidate threshold-straddling is
   inconclusive. Baseline uncertainty alone is not a veto.
   Existing deficit cells, including the historically borderline 12/13-worker
   region, must not show a statistically supported worsening of their C++ gap.
   A Rust lead is not a regression: the historical symmetric
   29/32 statistic is descriptive, not an acceptance rule that punishes speedups.
4. Preserve fixed-clock/affinity policy and serialize our own builds, tests and
   measurements. A separately authorized fresh matched boundary must use the
   validated `shared-host-worker-smt-execution-v2` policy; closed v1 campaigns
   retain their original rules and outcomes. Foreign work may coexist: retain
   host load/process/helper-activity evidence without a host-wide veto, and
   enforce the inclusive 5% worker/SMT preflight execution-plus-steal ceiling.
   During measurement, enforce the inclusive 1% idle-SMT-sibling
   execution-plus-steal ceiling and retain I/O wait separately, without an
   I/O-wait veto. The September-style lane's per-worker off-CPU ceiling remains
   1%, symmetrically for all arms. Retain raw runs and reject a comparison block
   if a required arm fails quality. Standalone point/TPC-C idle-only guards are
   unchanged and not equivalent. Shared caches, memory bandwidth, storage and
   helper contention remain uncontrolled shared-host limitations.
5. Preserve each protocol's recorded PMU threshold: the September campaign used
   98% running time; checked-in foreground runners require 99.5%. Report the
   chosen protocol, never quietly relabel one as the other. Native memory,
   worker limits, capacity exits, health, exact counts, and final state must be
   valid before throughput can be accepted.
6. Warm-path allocation/copy/atomic/dispatch and retained-memory measurements
   must show no added work attributable to a cosmetic interface. Intentional
   TxnCell snapshot allocations are not regressions; require the same workload
   and reuse state. Abort rates/conflict coverage must not acquire a new semantic
   cause. Genuine ABI/unsafe-layout obligations remain enforced.
7. Report latency only where actual per-operation distributions are measured;
   run-duration variation is not transaction p99. Do not claim unmeasured latency
   parity, leak freedom, sustained writeback, recovery, or durable ACK from a
   foreground-throughput result. Failed or missing required lane coverage blocks
   overall qualification even if the singleton result is excellent.

## Self-review and decisions after critique

| Tempting approach / weakness | Why it is unsafe or insufficient | Decision in this plan |
| --- | --- | --- |
| Add a C++-looking facade and leave the old protocol underneath | Creates two APIs to maintain and can add wrappers/dispatch without fixing source comprehension | Clean rename and caller cutover; one canonical protocol |
| Make every item contain one complete `TransItem` | Dense storage deliberately separates intent/preparation; the representative 64-bit fixture has a 56-byte hot component versus a 128-byte complete owner | Rename the existing owner; preserve split representations and measure footprints, rather than materializing a mirror |
| Give every callback the full mutable item and transaction | Broadens authority, permits reentry or removal of owned intent during install unwind, and weakens borrow contracts | Keep scoped proxy and phase-specific views/contexts |
| Make `txn.item()` return a normal mutable proxy to match C++ syntax | Can lose doom-on-error/unwind guarantees or keep references across materialization/reuse | Retain `with_item` and sessions; a returned-proxy redesign is outside scope |
| Treat the former preflight callback as acquisition by renaming it | Planning alone cannot implement datatype-managed locking | Current alignment implements synchronous `TObject::lock`; core still owns guards and deduplication |
| Assume every local datatype holds locks through all installs | This was the shape-only contract, not the current protocol | Local cell/counter publication is allowed; aliases defer and participants retain locks through finish |
| Rename predicate upgrade into a generic Boolean check | Loses covering observations and conflates execution with commit | `check_predicate` retains its typed upgrade result and phase; opacity remains unsupported |
| Fuse public datatype handles with registered object state | Can change ownership, initialization, and reference-count work merely to reduce visible structs | Preserve the handle/state split; the existing private adapter becomes the `TObject` implementation |
| Rename a registered resource to “the object” | Same adapter type/ObjectId can have record/directory/scan roles or share a physical lock | Keep explicit resource-class binding identity and supporting registration names |
| Assume a symbol/module rename has no runtime effect | Inlining, monomorphization, profile matching, and code placement can change | Separate vocabulary/module stages; compare optimized code and matched measurements |
| Allow the full margin after every accepted stage | Compounds losses and can widen an already borderline C++ gap | Keep immutable R0; final per-cell and C++-gap gates |
| Accept aggregate medians or the old 29/32 result | Hides slow worker counts, build changes, and unmeasured general paths | New exact-candidate matrix, raw data and uncertainty; broader required lanes |
| Reuse existing runners as though they support every mode | Rust-only variants reject C++; logged/ACK reject multiple items; TPC-C uses one combined binary | Close only identified orchestration gaps before baseline capture; no fabricated flags/results |
| Build R0 before fixing the measurement harness | Confounds production refactoring with changed workload/measurement code | Freeze common harness/probes first, then build every comparison arm |
| Treat any compile-fail result as success | Missing renamed imports can hide loss of the intended lifetime/phase restriction | Check the failure reason; retain valid-use counterparts and real extension tests |
| Re-pin old wording/layout assertions or rename every test | Adds maintenance without behavior proof and can invalidate exact quarantine lists | Preserve meaningful behavior/safety tests and stable test identities; measure private footprints separately |
| Use checkpoint/reopen or short ACK smoke as durability/performance proof | Those surfaces measure different state and timing boundaries | Separate functional recovery, foreground ACK, sustained application, and unclaimed durable ACK |

The review narrows the work to a recognizable object model with unchanged
execution mechanisms. It rejects the more invasive “literal C++ class clone”
interpretation. A materialized owner, a typed trait, and a scoped proxy give the
familiar concepts without forcing the C++ representation or unsafe API into Rust.

## Historical definition of done (not fully met)

- `Transaction`, `TransItem`, `TransProxy`, and `TObject` are identifiable in the
  actual source, not merely aliases or names on a diagram.
- Existing datatype implementations and their callbacks express those roles;
  a C++ STO reader can follow item lookup, RYW, checking, installation and cleanup.
- Complete-owned, dense, direct-singleton, and terminal representations remain
  distinct where that distinction saves work or enforces a safety contract.
- All selected definitions, public paths, consumers, fixtures and current docs
  have migrated; unrelated names and immutable historical artifacts have not.
- Advertised semantics, ABI/record formats, ownership/failure behavior, feature
  admission, and defaults are unchanged; all required correctness gates pass.
- Final R1 passes the required R0/C0 performance gates for the recorded profiles,
  with no unresolved regression, missing lane, or substituted workload.
- Exact source/build/runner identities and raw results are retained. No claim of
  unmeasured latency, full-library C++ parity, opacity, distributed execution, or
  production-default approval appears in the acceptance record.

## Historical plan verification record

The planning pass inspected current source, public exports, consumer and fixture
boundaries, driver interfaces, C++ datatype callbacks, and retained benchmark
records. It did not change production Rust/C++ code or execute a new benchmark,
full suite, or migration. The proposed stages and performance thresholds above
are future acceptance requirements, not reported completed results.
