# Skipped Kvrocks Set Cases

Source: Apache Kvrocks `tests/gocase/unit/type/set/set_test.go`.

| Kvrocks case | Reason |
| --- | --- |
| `SMISMEMBER ...` | Mako Phase 6 does not implement `SMISMEMBER`. |
| `SINTERCARD ...` | Mako Phase 6 does not implement `SINTERCARD`. |
| `against non set should throw WRONGTYPE` | Mako has no complete Redis type table yet; cross-type `WRONGTYPE` is not claimed. |
| `intset` / integer encoding stress cases | Kvrocks/Redis internal encoding behavior is not visible in Mako's composite-key set design. |
| `SPOP` random algorithm cases | Phase 6 documents deterministic member choice; statistical randomness is not claimed yet. |
| wrong-arity and invalid-count parser matrices | Covered separately by Mako parser tests as commands are added. |

