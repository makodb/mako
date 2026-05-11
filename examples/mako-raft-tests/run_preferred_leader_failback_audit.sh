#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
BUILD_PATH="$PROJECT_ROOT/$BUILD_DIR"
TEST_EXEC="$BUILD_PATH/testPreferredLeaderFailbackAudit"
LOG_DIR="$SCRIPT_DIR/logs/preferred_leader_failback_${BUILD_DIR}"

mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/*.log

if [[ ! -x "$TEST_EXEC" ]]; then
  echo "Missing $TEST_EXEC"
  echo "Build it first, for example:"
  echo "  cmake --build $BUILD_PATH --target testPreferredLeaderFailbackAudit -j\$(nproc)"
  exit 2
fi

pkill -9 -f "$TEST_EXEC" 2>/dev/null || true
sleep 1

echo "Preferred leader failback audit"
echo "  build: $BUILD_DIR"
echo "  logs:  $LOG_DIR"

pids=()

PREFERRED_FAILBACK_PHASE=initial "$TEST_EXEC" localhost > "$LOG_DIR/localhost.initial.log" 2>&1 &
initial_preferred_pid="$!"
echo "  started initial preferred localhost pid=$initial_preferred_pid"

for proc in p1 p2; do
  PREFERRED_FAILBACK_PHASE=backup "$TEST_EXEC" "$proc" > "$LOG_DIR/$proc.log" 2>&1 &
  pids+=("$!")
  echo "  started backup $proc pid=${pids[-1]}"
done

echo "  waiting before stopping preferred replica"
sleep "${PREFERRED_FAILBACK_STOP_DELAY_SEC:-5}"

echo "  stopping initial preferred replica pid=$initial_preferred_pid"
kill "$initial_preferred_pid" 2>/dev/null || true
wait "$initial_preferred_pid" 2>/dev/null || true

echo "  waiting before restarting preferred replica"
sleep "${PREFERRED_FAILBACK_RESTART_DELAY_SEC:-6}"

PREFERRED_FAILBACK_PHASE=rejoin "$TEST_EXEC" localhost > "$LOG_DIR/localhost.log" 2>&1 &
pids+=("$!")
echo "  restarted preferred localhost pid=${pids[-1]}"

exit_code=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    exit_code=1
  fi
done

cluster_submitted=0
for proc in localhost p1 p2; do
  submitted="$(grep -E "^\[$proc\] submitted=" "$LOG_DIR/$proc.log" | tail -1 | sed -E 's/.*submitted=([0-9]+).*/\1/' || true)"
  if [[ -n "$submitted" ]]; then
    cluster_submitted=$((cluster_submitted + submitted))
  fi
done

if [[ "$cluster_submitted" -lt 6 ]]; then
  echo "Expected at least one backup phase and one preferred-return phase, but only $cluster_submitted records were submitted"
  exit_code=1
fi

for proc in localhost p1 p2; do
  applied="$(grep -E "^\[$proc\] applied=" "$LOG_DIR/$proc.log" | tail -1 | sed -E 's/.*applied=([0-9]+).*/\1/' || true)"
  if [[ -z "$applied" || "$applied" -ne "$cluster_submitted" ]]; then
    echo "Applied-work mismatch for $proc: applied=${applied:-missing}, cluster_submitted=$cluster_submitted"
    exit_code=1
  fi
done

echo
echo "Summary:"
for proc in localhost p1 p2; do
  echo "--- $proc ---"
  grep -E "ROLE_DURING_PREFERRED_ABSENCE|ROLE_AFTER_FAILBACK_WAIT|final_leader=|became_leader_count=|lost_leader_count=|submitted=|applied=|minimum_expected_applied=|role_ok=|backup_failover_seen=|preferred_submit_ok=|apply_ok=|OVERALL=" "$LOG_DIR/$proc.log" || true
done
echo "--- localhost.initial ---"
grep -E "BECAME|LOST|initial preferred|final_leader=|OVERALL=" "$LOG_DIR/localhost.initial.log" || true

if [[ "$exit_code" -eq 0 ]]; then
  echo "OVERALL FAILBACK AUDIT PASS"
else
  echo "OVERALL FAILBACK AUDIT FAIL"
  echo "Logs are in $LOG_DIR"
fi

pkill -9 -f "$TEST_EXEC" 2>/dev/null || true
exit "$exit_code"
