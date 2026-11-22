# Near-final one-thread read profile

These are diagnostic profiles, not the final acceptance matrix. The Rust
`opt69` binary predates two small independently gated improvements included in
implementation commit `6936abb30000`. The C++ and Rust processes used one
thread, 100,000 preloaded keys, ten reads per transaction, and a ten-second
timed run. `perf stat` attached for approximately five seconds; `perf record`
used a separate same-shape run.

Counts normalized by enabled time and measured transaction throughput are:

| Counter | C++ per txn | Rust per txn | Rust/C++ |
| --- | ---: | ---: | ---: |
| cycles | 5,521 | 7,024 | 1.272x |
| L1 data loads | 2,904 | 4,052 | 1.395x |
| L1 data-load misses | 145.1 | 145.9 | 1.006x |
| data-TLB loads | 9.72 | 40.80 | 4.199x |
| data-TLB load misses | 0.052 | 1.700 | 32.55x |

`reach_leaf` accounts for 45.56% of C++ samples and 35.70% of Rust samples.
Applying those shares to cycles per transaction gives approximately 2,516 and
2,508 cycles respectively. Sampling percentages are approximate, but the close
normalized values support the conclusion that the residual is around the
Masstree lookup rather than in the traversal itself.

Normalization divides the approximately five-second task-clock counter window
by whole-run transaction throughput from the surrounding ten-second benchmark;
the stat attachment did not record a separate transaction count. The values
are therefore a controlled approximation, not direct per-transaction hardware
counts from exactly the same window.

Raw counters, benchmark JSON, and self/call-graph reports are retained in the
[`cpp`](cpp/) and [`rust-opt69`](rust-opt69/) directories. Multiplexed hardware
counter values reported by `perf` were used as printed.
