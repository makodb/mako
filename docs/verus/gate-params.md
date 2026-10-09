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
| G1 | 10 | latency_p50_us=0.02,latency_p99_us=0.05 | default bounds hold; CV_paired latency_p50_us=0.97%, latency_p99_us=1.44% (A/A n=25) |
| G2 | 10 | two_follower_round_us=0.02 | traced (`two_follower_rounds.py`): per run, the median time of rounds that sent to both followers; user decision after the Phase 3 checkpoint (Phase 3 report §3.1, since removed). Reported beside it, not gated: 25 untraced rounds' applied_per_sec and runs ending with a follower behind. Was: applied_per_sec=-0.054 at 25 rounds (CV_paired 9.56%, the skip-rate bimodality) |
| G3 | 25 | latency_p50_us=0.024,latency_p99_us=0.06 | latency_p50_us widened +0.02 -> +0.024 (25-round MDE); latency_p99_us widened +0.05 -> +0.06 (25-round MDE); CV_paired latency_p50_us=4.27%, latency_p99_us=10.70% (A/A n=25) |
| G4 | 11 | applied_per_sec=-0.02 | default bounds hold; CV_paired applied_per_sec=2.31% (A/A n=25) |
| G5 | 20 | latency_p50_us=0.02,latency_p99_us=0.05 | default bounds hold; CV_paired latency_p50_us=3.13%, latency_p99_us=4.70% (A/A n=25) |
| G6 | 12 | applied_per_sec=-0.02 | default bounds hold; CV_paired applied_per_sec=2.37% (A/A n=25) |

## Defaults (plan §6 table; `paired_cv.py` starts from these, never from a widened row)

| Point | Rounds | Bounds | Note |
|---|---|---|---|
| default G1 | 25 | latency_p50_us=0.02,latency_p99_us=0.05 | low-load latency, wake path |
| default G2 | 25 | applied_per_sec=-0.02 | per-message CPU. Superseded for gating by the traced row above: `paired_cv.py` does not derive it (re-running it for G2 would restore the untraced row) |
| default G3 | 25 | latency_p50_us=0.02,latency_p99_us=0.05 | production shape |
| default G4 | 25 | applied_per_sec=-0.02 | production capacity |
| default G5 | 25 | latency_p50_us=0.02,latency_p99_us=0.05 | per-byte path |
| default G6 | 25 | applied_per_sec=-0.02 | large-batch round |

## G7 (election and failover time)

Set once from the Phase 0 A/A G7 run by `scripts/verus/election_times.py --aa`:
per duration and statistic, `max(10%, |A − B| / A)` at n = 20.

| Point | Kills per arm | Bounds | Note |
|---|---|---|---|
| G7 | 20 | new_leader.median=0.1,new_leader.p90=0.102,first_commit.median=0.1,first_commit.p90=0.102 | from A/A run, n=20 per arm; max(10%, abs(A-B)/A) |
