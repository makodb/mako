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
