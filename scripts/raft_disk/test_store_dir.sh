#!/usr/bin/env bash
# Tests scripts/raft_disk/store_dir.sh: a live run survives another
# launcher's sweep, a SIGKILLed launcher's run is swept by the next, a child
# that outlives its launcher keeps the run alive, and cleanup deletes the run.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(mktemp -d /var/tmp/raft-store-dir-test.XXXXXX)
trap 'rm -rf "$root"' EXIT
export MAKO_RAFT_DATA_ROOT="$root"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

# A launcher that makes a run, reports it, and waits to be killed.
launcher() {
    bash -c "source '$here/store_dir.sh'; raft_store_make_run t 0 || exit 1; $2; echo \$RAFT_RUN_DIR > '$1'; exec sleep 300" >/dev/null 2>&1 &
    echo $!
}

pa=$(launcher "$root/a" true)
for _ in $(seq 50); do [ -s "$root/a" ] && break; sleep 0.1; done
run_a=$(cat "$root/a")
check "run A created" '[ -d "$run_a" ] && [ -e "$run_a/RUN.lock" ]'

# B sweeps while A lives: A's run stays.
( source "$here/store_dir.sh"; raft_store_make_run t 0 >/dev/null 2>&1; raft_store_cleanup )
check "a live run survives a sweep" '[ -d "$run_a" ]'

# Kill A: the next sweep deletes its run.
kill -9 "$pa"; wait "$pa" 2>/dev/null
( source "$here/store_dir.sh"; raft_store_sweep 2>/dev/null )
check "a killed launcher's run is swept" '[ ! -d "$run_a" ]'

# A child (a server) that outlives its launcher keeps the run.
pc=$(launcher "$root/c" "sleep 300 </dev/null >/dev/null 2>&1 & echo \$! > '$root/c.child'")
for _ in $(seq 50); do [ -s "$root/c" ] && break; sleep 0.1; done
run_c=$(cat "$root/c")
kill -9 "$pc"; wait "$pc" 2>/dev/null
( source "$here/store_dir.sh"; raft_store_sweep 2>/dev/null )
check "a run whose server still lives is kept" '[ -d "$run_c" ]'
kill -9 "$(cat "$root/c.child")"
sleep 0.2
( source "$here/store_dir.sh"; raft_store_sweep 2>/dev/null )
check "and is swept once the server dies" '[ ! -d "$run_c" ]'

# Cleanup deletes the run and releases the lock.
( source "$here/store_dir.sh"; raft_store_make_run t 0 2>/dev/null; d=$RAFT_RUN_DIR; raft_store_cleanup; [ ! -d "$d" ] )
check "cleanup deletes the run" '[ $? -eq 0 ]'

# Free-space refusal.
( source "$here/store_dir.sh"; ! raft_store_make_run t 999999 2>/dev/null )
check "refuses without enough free space" '[ $? -eq 0 ]'

exit $fail
