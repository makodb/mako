# Hourly CI Check

## Rationale
Regularly checking the latest completed GitHub CI run helps catch regressions early and ensures failures are recorded with commit context for follow-up tasks.

## Procedure
1. Query GitHub Actions for `ci.yml` workflow runs.
2. Find the most recent run with `status=completed`.
3. Record its conclusion and commit SHA in `TODO.md` under the hourly repeated task.
