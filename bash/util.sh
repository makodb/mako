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
