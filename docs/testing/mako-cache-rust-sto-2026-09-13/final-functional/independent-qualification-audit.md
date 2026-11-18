# Independent final qualification audit

Audited the completed frozen-source reports on 2026-09-13. No production
files were edited and no sanitizer results were re-audited.

## Accepted results

| Gate | Audited inventory |
| --- | --- |
| `qualified-engine` | 35 positives: 20 library, one differential, one history, 12 native, one value-boundary. One existing intentional LSan control ignored. |
| `qualified-cache-sto` | 237 positives: 206 library, 18 non-model integration, eight Loom models, five doctests. |
| `qualified-cache-c-abi-retry` | 236 positives: 205 library plus the same 31 integration/model/doc tests. |
| `qualified-engine-miri` | Nine named native-free tests, including deterministic prefetch state preservation and both ownership tests. |
| `qualified-engine-mutations` | All 19 current mutation definitions reported killed; all 12 distinct baseline tests passed. |
| `qualified-cache-mutations` | All 15 selected mutations compiled successfully, each ran exactly one designated test and failed with its expected signature. No timeout, survivor or harness error. |
| Clock | Current-source hash matches the 12/12 mutation report. The all-features log names all 21 passing tests. |
| `qualified-clippy` | `-D warnings`, all features and targets for mako-clock, mako-engine-sto, sto-core and sto-masstree. This does not claim a warning-free cache crate. |
| Standalone driver tests | 48 methods across the exact six modules, split 7 + 12 + 7 + 12 + 4 + 6. |
| `qualified-cross-direct-retry` | Both main binaries passed 24 unit tests each. Cross report contains all 12 workload cases, 16 three-stage state cases and four wrong-checksum rejections. |

For engine and both cache suites, executable `--list` inventories match the
named log inventories. The expected-panic application-history test is present
and passed in both cache logs. Child subprocess summaries were not counted
again as extra parent tests. The engine differential cannot silently skip a
missing C++ driver under the recorded native launcher's required-driver mode.

The cross report has exactly both engine directions, both checksum policies,
all three workload shapes, both foreground modes and both retention states.
Every state record has the expected engine, role and exact row count.
Retained plus reclaimed records equal the recorded sequence; reopen advances
sequence and HLC, and final verification preserves them. All four rejection
tripwires are present. Report SHA256:
`d1560e304b1610d41579cc3b27c1b1d516b0fe94db6691f08e53c7a454ebac6c`.

## Source and native identities

All eight accepted `qualified-*.json` command records have identical before
and after snapshots. Their 233 source hashes and 18 native-input hashes match
the files at audit time. The native inputs comprise 17 archives and one build
manifest. Log, checked-runner and native-launcher hashes also match. The
restored native verification log contains the original fingerprint
`9a8e66f56c12c3a6cc0a554735084cb61db66625c571eebde17fc3dcfaa070ea`.

Mutation-specific integrity checks also match current files: 107 engine
dependency/source inputs, 22 cache files and 166 cache dependency inputs.
Engine and clock report inventories exactly match their current 19 and 12
mutation definitions. Their audited runner logic rejects compilation errors,
timeouts and failures that do not identify the designated test. Unlike the
cache report, engine and clock reports retain summary outcomes rather than
each raw failure transcript, so this audit relies on that hash-bound runner
classification for the individual failures.

Cross executable hashes observed after the successful run:

```text
c-abi 371d771177344842a40e1e3bf39cd7c221f1058ade5e209527f6b83ccc94f380
sto   e3aa556cbed639f7fd342aace519daadb7656502229c07f09588568399ee940f
```

These are post-run observations. The cross report itself records executable
paths and engine names, not pre/post executable hashes.

## Qualifications and excluded attempts

The standalone fixed driver log records 48 successes but has no checked-run
JSON. Two of its six test modules are outside the 233-file snapshot:

```text
scripts/test_mako_engine_benchmark_drivers.py
b0127937771fcaf4df9b930cbf86715b47634672a162c4b5bf74027cca955ef9
scripts/test_run_rust_sto_miri.py
f4f651f5d8e22de920b01989be7a0a507123b0a9addefcec95a41d42bdde5649
```

Their current inventories match the log total. The planned driver-only
follow-up should record its full command and all driver input hashes. This
does not invalidate the source-checked runtime gates.

Do not include `interrupted-cross-command-raw.json` or
`qualified-cross-reopen.json` as completed evidence, even where their wrapper
recorded subprocess exit zero. Also exclude the first
`qualified-cache-c-abi.json` and `qualified-cross-direct.json`, which failed
while four intermediate native object files were absent, and the first
`driver-tests.log`, which had two Python import errors. The accepted retries
above supersede those attempts; they do not erase them.

This audit covers the frozen runtime source. Later CI-only driver changes
need their own checkpoint and should not be presented as runtime changes.
