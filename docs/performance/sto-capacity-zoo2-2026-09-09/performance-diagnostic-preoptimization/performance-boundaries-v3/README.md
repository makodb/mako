# Pre-optimization performance controls

Stopped after one accepted old/new block each at one and sixteen workers. These are diagnostic results for candidate hash `3538efdb...`, before the whole-segment budget reservation optimization.

| Workers | Variant | Engine | Transactions/s | Setup s | Guarded interval s | Post-measurement s | Process wall s |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | new | cpp | 40626.790580 | 2.389031 | 11.092767 | 0.180178 | 13.661976 |
| 1 | new | rust | 57844.808283 | 3.137249 | 11.097710 | 0.601309 | 14.836268 |
| 1 | old | cpp | 40716.461541 | 2.426294 | 11.095689 | 0.141612 | 13.663595 |
| 1 | old | rust | 58189.693601 | 3.099861 | 11.094951 | 0.603727 | 14.798539 |
| 16 | new | cpp | 635314.862975 | 20.177615 | 11.145473 | 1.040145 | 32.363233 |
| 16 | new | rust | 801104.589393 | 21.877655 | 11.112564 | 98.264318 | 131.254537 |
| 16 | old | cpp | 656205.758270 | 20.684953 | 11.141761 | 1.017282 | 32.843996 |
| 16 | old | rust | 848657.550237 | 22.235992 | 11.136100 | 103.975929 | 137.348021 |

All accepted measurements passed the existing journal-activity and competitor guards. Native allocator 2G, structural registry budget 8G, default transaction mix, CPU mask prefix of 10 through 25. The old binary ignores the new structural-budget setting and retains its historical quotas.

At one worker, Rust changed -0.5927% and C++ -0.2202%. At sixteen, Rust changed -5.6033% and C++ -3.1836%. C++-normalized changes were -0.3733% and -2.4993%. One block per cell cannot establish equivalence or isolate small patch costs. The sixteen-worker candidate pair used zero settling delay; all other accepted pairs used three seconds.

The earlier performance-interleaved and performance-interleaved-v2 directories are pilot or rejected evidence. Their results are not included here. The long Rust cleanup also occurred in the old binary, 103.976 seconds versus 98.264 seconds for the candidate at sixteen workers.
