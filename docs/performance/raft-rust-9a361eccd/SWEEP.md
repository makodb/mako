# Raft performance sweep

- Commit:  `9a361eccd`
- Started: 20260926_143933
- Output:  `/home/users/zyang2/mako-srpc-adopt/raft_perf_output/sweep_rust_9a361eccd`
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
python3 scripts/raft_perf/processing.py /home/users/zyang2/mako-srpc-adopt/raft_perf_output/sweep_rust_9a361eccd/rate
python3 scripts/raft_perf/lattput.py /home/users/zyang2/mako-srpc-adopt/raft_perf_output/sweep_rust_9a361eccd/rate -o /home/users/zyang2/mako-srpc-adopt/raft_perf_output/sweep_rust_9a361eccd/lattput.png
python3 scripts/raft_perf/plot_latency_cdf.py /home/users/zyang2/mako-srpc-adopt/raft_perf_output/sweep_rust_9a361eccd/rate -o /home/users/zyang2/mako-srpc-adopt/raft_perf_output/sweep_rust_9a361eccd/cdf.png
```
