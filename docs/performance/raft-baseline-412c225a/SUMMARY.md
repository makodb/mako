# Raft performance sweep

- Commit:  `412c225a`
- Started: 20260912_035757
- Output:  `raft_perf_output/baseline-412c225a`
- Quick:   false
- Trials:  3
- Runs:    624 ok, 0 failed, 624 planned

## Records

| phase | records |
|---|---|
| rate | 567 |
| payload | 27 |
| batch | 18 |
| groups | 12 |

## Next

```
python3 scripts/raft_perf/processing.py raft_perf_output/baseline-412c225a/rate
python3 scripts/raft_perf/lattput.py raft_perf_output/baseline-412c225a/rate -o raft_perf_output/baseline-412c225a/lattput.png
python3 scripts/raft_perf/plot_latency_cdf.py raft_perf_output/baseline-412c225a/rate -o raft_perf_output/baseline-412c225a/cdf.png
```
