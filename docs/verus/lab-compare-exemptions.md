# Lab comparator exemptions

From the Phase 0 calibration ([modification-plan.md](modification-plan.md)
A.4 item 1): five full lab runs of one build (`build_rust_raftlab` at
`verus-p0`), compared with each other by `scripts/verus/lab_trace_compare.py
calibrate`. A field that differs between runs of the same binary is exempt for
that case; the machine-readable list is `$RESULTS/p0/lab/exemptions.json`.
Verdicts and cross-replica agreement (no two replicas commit different
entries at one index) are never exempt; in all five runs every case passed and
agreement held.

**Status: closed.** Stop point 0.7 point 8 was reached (16 of 27 cases exempt
the (term, leader) sequence). The user then cut the testing between phases
(plan 0.10, 2026-10-03): the comparator is no longer a gate, so the projection
proposed below was never adopted. Until Phase 3 the lab passing is the check;
`lab_trace_compare.py` stays as a diagnostic.

## What varies, and why

| Field | Cases exempt | Why it varies on one binary |
|---|---|---|
| `leaders` (the (term, leader) sequence of waitOneLeader results) | 16/27 | Elections are randomized: which replica's timeout fires first decides the leader, and terms depend on how many elections happened before. Case 1 alone elected replicas 3, 2, 2, 3 and 1 in the five runs. |
| `log` (committed log with terms) | 22/27 | Carries the terms above. |
| `log_values` (committed (index, payload)) | 20/27 | Cumulative: each case dumps the whole committed table, so from the first case whose outcome varies (case 5) every later case's cumulative log differs. |
| `log_length` | 20/27 | Same cumulative effect. |
| `new_payloads` (payloads this case committed, order-insensitive) | 2/27 | Stable except case 5 ("No agreement if too many followers disconnect": whether the minority leader's uncommitted entries are later overwritten or survive depends on timing) and case 10 ("Unreliable agreement": random message loss). |
| `new_count` | 2/27 | Same two cases. |

## Proposed comparison (needs a yes)

Compare, per case: the verdict and cross-replica agreement (never exempt),
plus `new_payloads` and `new_count` (exempt in cases 5 and 10 only). Keep
`leaders` and the cumulative log fields in the output as information, not as
pass/fail fields. From Phase 3 the byte-identical replay at `step()` (A.4
item 3) is the strong equivalence check and this comparator becomes a smoke
test.
