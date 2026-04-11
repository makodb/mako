# CI Hourly Check Plan

## Goal
Check the latest completed `ci.yml` workflow run and record whether it failed. If failed, add a fix task to TODO with the run/commit details.

## Scope
- Read the most recent completed CI run from GitHub Actions.
- Update the hourly check entry in `TODO.md` with the result and timestamp.

## Steps
1. Query `https://api.github.com/repos/makodb/mako/actions/workflows/ci.yml/runs?per_page=1&status=completed`.
2. Record the run URL, conclusion, and head SHA.
3. If conclusion is `failure`, add a fix task with the commit hash.
4. Update the TODO entry with timestamp and summary.
