# Provenance of scripts/raft_perf/jetpack

Copied from Jetpack, https://github.com/stonysystems/jetpack, commit
`c03e318ec355b11edd42aac56c68d0765f88d1d2` (MIT, see `LICENSE.jetpack`).

## Byte-identical copies (not edited)

| here | Jetpack path |
|---|---|
| `derive_fixed_conc.py` | `scripts/derive_fixed_conc.py` |
| `build_per_protocol_tables.py` | `scripts/build_per_protocol_tables.py` |
| `merge_latency_csv.py` | `scripts/merge_latency_csv.py` |
| `res_file_utils.py` | `scripts/res_file_utils.py` |
| `gen_tput_p90_figures.py` | `scripts/camera-ready/gen_tput_p90_figures.py` |
| `gen_latency_cdf.py` | `scripts/camera-ready/gen_latency_cdf.py` |
| `config/*.yml` | `config/` (same names) |

These are never edited. Their Jetpack-specific settings (protocol list,
results root, site name, host names) are module attributes, which
`jetpack_compare.py` overrides after importing them.

## Written here, reproducing Jetpack's code

- `../../../src/deptran/raft/raft_bench_jetpack.h`: Jetpack's `ZipfDist`, its
  `Distribution` statistics, the open-loop client loop, the latency window
  and the `.res`/CSV output, as a mode of `raft_bench` (`JETPACK_N`). Its
  header comment lists the source lines and what differs.
- `netdelay.c`: Jetpack's `WAN_DELAY_MS` (each outbound Raft request waits
  WAN_DELAY_MS; replies do not) as an `LD_PRELOAD` link delay, the same for
  every build.
- `run_jetpack_sweep.sh`: runs the points and writes Jetpack's result-dir
  layout plus a sidecar JSON per run recording the arm, commit and delay.
- `jetpack_compare.py`: drives the copied scripts over those results.
