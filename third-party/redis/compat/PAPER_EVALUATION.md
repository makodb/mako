# Redis-over-Mako paper evaluation protocol

## Claim under test

This evaluation measures the cost and scale-up behavior of the Redis command
compatibility layer relative to direct Mako on the same host. A completed Redis
`GET` or `SET` counts as one operation. It does not count as a Rolis YCSB++ or
Mako TPC-C transaction, both of which perform multiple database operations and
may include replication.

`P1`, `P64`, and `P512` mean pipeline depths 1, 64, and 512. They are not Mako
partition counts. Every run in this protocol uses one unreplicated Mako shard.

## Controlled environment

- Host: ag2 (`zoo-002`), recorded in each manifest.
- Server CPUs: physical cores 0-31.
- Client CPUs: separate physical cores 32-63.
- Dataset: 1,000,000 preloaded keys.
- Default value: 8 bytes.
- Default workload: uniform-random 80% `GET`, 20% `SET`.
- Load model: closed loop, two clients per configured server worker.
- Startup and preload: after PING succeeds and all expected worker threads are
  visible, the harness allows five additional seconds for backend initialization.
  Starting a 64-byte preload immediately reproduced `-ERR backend`; after this
  settle interval, both 8- and 32-thread loaders completed the same one-million-
  key preload. Paper runs use at most eight loader threads. Preload is outside
  the timed window. Because each preload SET is idempotent, the loader retries
  transient backend or connection failures up to 100 times per key with a
  one-millisecond delay and reconnect; exhausted retries still fail the run.
  The retry count is written to the preload log. Settle time, retry limit, and
  effective preload concurrency are recorded in the manifest.
- Paper profile: 10-second warmup and five independent 30-second samples.
- Randomization: workload order is deterministically shuffled from the recorded
  experiment seed.
- Statistics: arithmetic mean, standard deviation, coefficient of variation,
  minimum, maximum, and a two-sided Student-t 95% confidence interval.
- Latency memory: deterministic reservoir sampling retains at most 65,536
  latency observations per client thread. CSV output records retained samples
  and total observations. This keeps long runs bounded while sampling across
  the entire measurement window.

Preload time is excluded. The server restarts before every worker-count point.
Every result directory records source and binary hashes, Git state, CPU
topology, configuration, repeat-level CSV data, CPU samples, server logs, and a
summary CSV.

## Experiment matrix

### E1: direct-Mako scale-up baseline

- Workers: 1, 8, 16, 32.
- Workload: uniform 80% `GET`, 20% `SET`.
- Target: direct Mako only.

### E2: Redis pipeline scale-up

- Workers: 1, 8, 16, 32.
- Pipeline depths: 1, 64, 512.
- Workload and dataset match E1.
- E1 is imported into every result directory for same-host normalization.

### E3: operation-mix sensitivity

- Workers: 32.
- Pipeline depth: 64.
- Workloads: `GET`, `SET`, and uniform 80% `GET` / 20% `SET`.
- The matching 32-worker direct-Mako rows are measured with the same protocol.

### E4: value-size sensitivity

- Workers: 32.
- Pipeline depth: 64.
- Values: 8, 64, and 1,024 bytes.
- Workload: uniform 80% `GET`, 20% `SET`.

### E5: correctness and stability

- Run the focused Python and Rust suites against the final binary.
- Run the acceptance harness, including the serializable-history checker and
  deterministic RESP fuzzing.
- Run a 30-minute bounded soak at the selected operating point while recording
  throughput, latency, RSS, file descriptors, threads, and failures.

The paper soak uses `run_paper_soak.sh`. It runs the same compiled RESP client
as the scalability experiments at 32 workers, two clients per worker, and
pipeline depth 64. Resource samples are written every five seconds to
`resource_timeseries.csv`, with endpoint and range statistics in
`resource_summary.json`. After the run, `summarize_paper_soak.py` extracts the
timed 30-minute window and reports its RSS slope, resource ranges, throughput,
and latency in `paper_soak_summary.json`, excluding preload, warmup, and the
post-client-disconnect sample. The measurement window ends at the final sample
with the fully connected load phase's maximum file-descriptor count; the rule
and selected elapsed-time bounds are recorded in the summary.

`run_latency_sampling_regression.sh` exercises the full 32-worker/P64 client
path with a small sample cap. It fails unless total latency observations exceed
the retained reservoir by at least 100x and the retained count exactly matches
the configured per-thread bound.

Multi-shard atomicity and replicated failover remain N/A until real fixtures
are supplied. They must not be presented as passing results.

## Primary figures and claims

The primary plots are throughput versus workers, throughput retained relative
to direct Mako, and P99 latency versus workers. Error bars show 95% confidence
intervals, not standard deviations. Pipeline depth is reported explicitly on
every plot and table.

Published Rolis and Mako numbers may be included only as contextual references.
The paper's quantitative comparison must use the same-host direct-Mako baseline
unless the competing implementation is run on the same hardware with the same
workload, client placement, CPU allocation, and replication guarantees.
