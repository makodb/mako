# Raft performance sweep

- Commit:  `538df5f8c`
- Started: 20260925_195540
- Output:  `/home/users/zyang2/mako-srpc-adopt/raft_perf_output/sweep_20260925_195540`
- Quick:   false
- Trials:  3
- Runs:    590 ok, 34 failed, 624 planned

## Failed runs

- `rate/538df5f8c-p1-single-pb286208-b1-r450-t1`
- `rate/538df5f8c-p1-single-pb286208-b1-r450-t2`
- `rate/538df5f8c-p1-single-pb286208-b1-r675-t1`
- `rate/538df5f8c-p1-single-pb286208-b1-r675-t2`
- `rate/538df5f8c-p1-single-pb286208-b1-r675-t3`
- `rate/538df5f8c-p1-single-pb286208-b1-r0-t1`
- `rate/538df5f8c-p1-single-pb286208-b1-r0-t2`
- `rate/538df5f8c-p1-single-pb286208-b1-r0-t3`
- `rate/538df5f8c-p1-single-pb1048576-b1-r98-t1`
- `rate/538df5f8c-p1-single-pb1048576-b1-r130-t3`
- `rate/538df5f8c-p1-single-pb1048576-b1-r195-t1`
- `rate/538df5f8c-p1-single-pb1048576-b1-r195-t2`
- `rate/538df5f8c-p1-single-pb1048576-b1-r0-t1`
- `rate/538df5f8c-p6-single-pb286208-b1-r450-t2`
- `rate/538df5f8c-p6-single-pb286208-b1-r0-t1`
- `rate/538df5f8c-p6-single-pb286208-b1-r0-t2`
- `rate/538df5f8c-p6-single-pb286208-b1-r0-t3`
- `rate/538df5f8c-p6-single-pb1048576-b1-r62-t2`
- `rate/538df5f8c-p6-single-pb1048576-b1-r72-t1`
- `rate/538df5f8c-p6-single-pb1048576-b1-r72-t3`
- `rate/538df5f8c-p6-single-pb1048576-b1-r81-t1`
- `rate/538df5f8c-p6-single-pb1048576-b1-r81-t2`
- `rate/538df5f8c-p6-single-pb1048576-b1-r98-t1`
- `rate/538df5f8c-p6-single-pb1048576-b1-r98-t3`
- `rate/538df5f8c-p6-single-pb1048576-b1-r130-t2`
- `rate/538df5f8c-p6-single-pb1048576-b1-r195-t2`
- `rate/538df5f8c-p6-single-pb1048576-b1-r195-t3`
- `rate/538df5f8c-p6-single-pb1048576-b1-r0-t1`
- `rate/538df5f8c-p6-single-pb1048576-b1-r0-t2`
- `rate/538df5f8c-p6-single-pb1048576-b1-r0-t3`
- `rate/538df5f8c-p6-multi-pb286208-b1-r0-t2`
- `groups/538df5f8c-p6-single-pb286208-b1-r0-t1`
- `groups/538df5f8c-p6-single-pb286208-b1-r0-t2`
- `groups/538df5f8c-p6-single-pb286208-b1-r0-t3`

A failed run is a result, not a gap: check the matching .log
and the .logs/ directory beside it for the three replicas.

## Records

| phase | records |
|---|---|
| rate | 567 |
| payload | 27 |
| batch | 18 |
| groups | 12 |

## Next

```
python3 scripts/raft_perf/processing.py /home/users/zyang2/mako-srpc-adopt/raft_perf_output/sweep_20260925_195540/rate
python3 scripts/raft_perf/lattput.py /home/users/zyang2/mako-srpc-adopt/raft_perf_output/sweep_20260925_195540/rate -o /home/users/zyang2/mako-srpc-adopt/raft_perf_output/sweep_20260925_195540/lattput.png
python3 scripts/raft_perf/plot_latency_cdf.py /home/users/zyang2/mako-srpc-adopt/raft_perf_output/sweep_20260925_195540/rate -o /home/users/zyang2/mako-srpc-adopt/raft_perf_output/sweep_20260925_195540/cdf.png
```
