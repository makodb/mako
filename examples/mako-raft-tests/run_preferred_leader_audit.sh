#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
BUILD_PATH="$PROJECT_ROOT/$BUILD_DIR"
TEST_EXEC="$BUILD_PATH/testPreferredLeaderAudit"
LOG_DIR="$SCRIPT_DIR/logs/preferred_leader_audit_${BUILD_DIR}"
PROCESSES=(localhost p1 p2)

mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/*.log

if [[ ! -x "$TEST_EXEC" ]]; then
  echo "Missing $TEST_EXEC"
  echo "Build it first, for example:"
  echo "  cmake --build $BUILD_PATH --target testPreferredLeaderAudit -j\$(nproc)"
  exit 2
fi

pkill -9 -f "$TEST_EXEC" 2>/dev/null || true
sleep 1

echo "Preferred leader audit"
echo "  build: $BUILD_DIR"
echo "  logs:  $LOG_DIR"

pids=()
for proc in "${PROCESSES[@]}"; do
  "$TEST_EXEC" "$proc" > "$LOG_DIR/$proc.log" 2>&1 &
  pids+=("$!")
  echo "  started $proc pid=${pids[-1]}"
done

exit_code=0
for i in "${!PROCESSES[@]}"; do
  proc="${PROCESSES[$i]}"
  pid="${pids[$i]}"
  if wait "$pid"; then
    echo "  $proc exited 0"
  else
    rc=$?
    echo "  $proc exited $rc"
    exit_code=1
  fi
done

echo
echo "Summary:"
for proc in "${PROCESSES[@]}"; do
  echo "--- $proc ---"
  grep -E "FINAL_ROLE_BEFORE_SUBMIT|final_leader=|became_leader_count=|lost_leader_count=|submitted=|applied=|role_ok=|submit_ok=|apply_ok=|OVERALL=" "$LOG_DIR/$proc.log" || true
done

if [[ "$exit_code" -eq 0 ]]; then
  echo "OVERALL AUDIT PASS"
else
  echo "OVERALL AUDIT FAIL"
  echo "Logs are in $LOG_DIR"
fi

pkill -9 -f "$TEST_EXEC" 2>/dev/null || true
exit "$exit_code"
