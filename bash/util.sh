# Read config value from ~/.makorc
# Usage: read_makorc_value "key" "default"
# Lines starting with # are ignored as comments
read_makorc_value() {
    local key="$1"
    local default="$2"
    if [ -f ~/.makorc ]; then
        local found=$(grep -v "^\s*#" ~/.makorc | grep -E "^${key}\s*:" | sed 's/.*:\s*//' | tr -d ' ')
        if [ -n "$found" ]; then
            echo "$found"
            return
        fi
    fi
    echo "$default"
}

# GDB_ENABLED: Set from ~/.makorc (use_gdb: 1 to enable)
# Can be overridden by MAKO_NO_GDB=1 environment variable (useful for CI)
# Scripts can use this variable directly to conditionally run under GDB
if [ "$MAKO_NO_GDB" == "1" ]; then
    GDB_ENABLED="0"
else
    GDB_ENABLED=$(read_makorc_value "use_gdb" "0")
fi

# GDB_PREFIX: If GDB is enabled, this prefix wraps commands with gdb batch mode
# Usage: $GDB_PREFIX ./executable args > logfile 2>&1
GDB_PREFIX=""
GDB_LOG_DIR="./crash_logs"
if [ "$GDB_ENABLED" == "1" ]; then
    mkdir -p "$GDB_LOG_DIR"
    GDB_CMD_FILE="${GDB_LOG_DIR}/gdb_cmd.txt"
    cat > "${GDB_CMD_FILE}" <<EOF
set pagination off
run
echo \n========== thread apply all bt full ==========\n
thread apply all bt full
quit
EOF
    GDB_PREFIX="gdb -batch -x ${GDB_CMD_FILE} --args"
fi

# Build optional dbtest CPU throttling CLI flags from environment.
# - MAKO_CPU_LIMIT: integer in [0,100]
# - MAKO_THROTTLE_CYCLE_MS: positive integer (milliseconds)
# Prints a leading-space-prefixed argument string, or nothing if disabled.
mako_dbtest_throttle_args() {
    local args=""

    if [ -n "${MAKO_CPU_LIMIT:-}" ]; then
        if ! [[ "${MAKO_CPU_LIMIT}" =~ ^[0-9]+$ ]] || [ "${MAKO_CPU_LIMIT}" -lt 0 ] || [ "${MAKO_CPU_LIMIT}" -gt 100 ]; then
            echo "Error: MAKO_CPU_LIMIT must be an integer in [0,100], got '${MAKO_CPU_LIMIT}'" >&2
            return 1
        fi
        args="${args} --cpu-limit ${MAKO_CPU_LIMIT}"
    fi

    if [ -n "${MAKO_THROTTLE_CYCLE_MS:-}" ]; then
        if ! [[ "${MAKO_THROTTLE_CYCLE_MS}" =~ ^[0-9]+$ ]] || [ "${MAKO_THROTTLE_CYCLE_MS}" -le 0 ]; then
            echo "Error: MAKO_THROTTLE_CYCLE_MS must be a positive integer, got '${MAKO_THROTTLE_CYCLE_MS}'" >&2
            return 1
        fi
        args="${args} --throttle-cycle ${MAKO_THROTTLE_CYCLE_MS}"
    fi

    printf '%s' "${args}"
}

# wait for nohup jobs DONE
wait_for_jobs() {
  echo "Wait for jobs..."
  FAIL=0
  for job in `jobs -p`
  do
      wait $job || let "FAIL+=1"
  done

  if [ "$FAIL" == "0" ];
  then
      echo "YAY!"
  else
      echo "FAIL! ($FAIL)"
  fi
}

# build masstree
build_masstree() {
    cd masstree; ./configure  CC="cc" CXX="g++" --enable-max-key-len=1024 --disable-assertions --disable-invariants --disable-preconditions --with-malloc=jemalloc
}
