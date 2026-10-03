# Tier 2 gate parameters

Read by `scripts/verus/gate_point.sh` (the `| Gk |` rows) and written by
`scripts/verus/paired_cv.py` from the Phase 0 A/A run
([modification-plan.md](modification-plan.md) §6, "Deriving rounds and
bounds"). A bound is a signed fraction of the median paired ratio B/A − 1:
positive where higher is worse (latency), negative where lower is worse
(throughput).

## Gated rows

Seeded with 25 rounds and the default bounds; overwritten by the A/A run.

| Point | Rounds | Bounds | Note |
|---|---|---|---|
| G1 | 25 | latency_p50_us=0.02,latency_p99_us=0.05 | seed (plan §6 defaults) |
| G2 | 25 | applied_per_sec=-0.02 | seed (plan §6 defaults) |
| G3 | 25 | latency_p50_us=0.02,latency_p99_us=0.05 | seed (plan §6 defaults) |
| G4 | 25 | applied_per_sec=-0.02 | seed (plan §6 defaults) |
| G5 | 25 | latency_p50_us=0.02,latency_p99_us=0.05 | seed (plan §6 defaults) |
| G6 | 25 | applied_per_sec=-0.02 | seed (plan §6 defaults) |

## Defaults (plan §6 table; `paired_cv.py` starts from these, never from a widened row)

| Point | Rounds | Bounds | Note |
|---|---|---|---|
| default G1 | 25 | latency_p50_us=0.02,latency_p99_us=0.05 | low-load latency, wake path |
| default G2 | 25 | applied_per_sec=-0.02 | per-message CPU |
| default G3 | 25 | latency_p50_us=0.02,latency_p99_us=0.05 | production shape |
| default G4 | 25 | applied_per_sec=-0.02 | production capacity |
| default G5 | 25 | latency_p50_us=0.02,latency_p99_us=0.05 | per-byte path |
| default G6 | 25 | applied_per_sec=-0.02 | large-batch round |

## G7 (election and failover time)

Set once from the Phase 0 A/A G7 run by `scripts/verus/election_times.py --aa`:
per duration and statistic, `max(10%, |A − B| / A)` at n = 20.

| Point | Kills per arm | Bounds | Note |
|---|---|---|---|
| G7 | 20 | new_leader.median=0.10,new_leader.p90=0.10,first_commit.median=0.10,first_commit.p90=0.10 | seed (plan §6 floor of 10%) |
