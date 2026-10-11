#!/usr/bin/env bash
# tier1.sh PHASE [rust|rustlab|rustdisk]...
#
# Tier 1 of docs/verus/modification-plan.md §6: the Raft suites, serially,
# logging to $RESULTS/PHASE/tier1/, then the first-attempt failure count.
# rust (default): the lab plus the four replication suites; rustlab: the lab
# alone; rustdisk: the lab and the four suites on build_rust_disk, the
# -DMAKO_RAFT_DISK=ON tree (docs/verus/disk-persistence.md §8), which
# refuses to start with under 8 GB free where the stores live. Raft has no
# other lane on this branch (the plan, Q9).
#
# ci.sh's cleanup_processes kill -9s EVERY process of this user named dbtest,
# simpleTransactionRep, simplePaxos, simpleTransaction or simpleRaft, and
# deletes /tmp/$USER_mako_rocksdb_shard*. Other worktrees on this host may be
# running those. So before each suite this waits until no such process runs
# from a directory outside this worktree, and refuses to start after an hour.
set -uo pipefail
P=${1:?usage: tier1.sh PHASE [rust|rustlab|rustdisk]...}; shift
LANES=("$@"); [ ${#LANES[@]} -eq 0 ] && LANES=(rust)
: "${RESULTS:?source ~/mako-verus-env.sh first}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
OUT=$RESULTS/$P/tier1
mkdir -p "$OUT"

foreign_tests() {
  local pid cwd name
  for name in simpleTransactionRep dbtest simplePaxos simpleTransaction simpleRaft deptran_server; do
    for pid in $(pgrep -u "$(id -u)" -x "$name" 2>/dev/null); do
      cwd=$(readlink "/proc/$pid/cwd" 2>/dev/null) || continue
      case "$cwd" in "$REPO_ROOT"|"$REPO_ROOT"/*) ;; *) echo "$pid $name $cwd";; esac
    done
  done
}

wait_quiet() {
  local waited=0 f
  while f=$(foreign_tests) && [ -n "$f" ]; do
    [ $waited -eq 0 ] && echo "tier1: waiting for another worktree's test processes: $f"
    sleep 30; waited=$((waited + 30))
    if [ $waited -ge 3600 ]; then echo "tier1: gave up waiting after 1 h"; return 1; fi
  done
  return 0
}

rc=0
run() {  # run NAME BUILD_DIR SUITE
  wait_quiet || { echo "FAIL $1 (not started: foreign test processes)"; rc=1; return; }
  local t0=$SECONDS
  if BUILD_DIR=$2 ./ci/ci.sh "$3" > "$OUT/$1.log" 2>&1; then
    echo "pass $1 ($((SECONDS - t0)) s)"
  else
    echo "FAIL $1 ($((SECONDS - t0)) s)"; rc=1
  fi
}
for lane in "${LANES[@]}"; do
  case $lane in
    rust)
      run raftLabTest build_rust raftLabTest
      for t in shard1ReplicationRaft shard2ReplicationRaft shard1ReplicationSimpleRaft shard2ReplicationSimpleRaft; do
        run "$t" build_rust "$t"
      done;;
    rustlab) run raftLabTest build_rust raftLabTest;;
    rustdisk)
      root=${MAKO_RAFT_DATA_ROOT:-/var/tmp/raft-wal-$USER}; mkdir -p "$root"
      free=$(df --output=avail -B1G "$root" | tail -1 | tr -d ' ')
      if [ "${free:-0}" -lt 8 ]; then
        echo "FAIL rustdisk (only ${free} GB free under $root)"; rc=1; continue
      fi
      if ! grep -qs '^MAKO_RAFT_DISK:BOOL=ON' build_rust_disk/CMakeCache.txt; then
        echo "FAIL rustdisk (build_rust_disk is not a -DMAKO_RAFT_DISK=ON tree)"; rc=1; continue
      fi
      run raftLabTest.disk build_rust_disk raftLabTest
      for t in shard1ReplicationRaft shard2ReplicationRaft shard1ReplicationSimpleRaft shard2ReplicationSimpleRaft; do
        run "$t.disk" build_rust_disk "$t"
      done;;
    *) echo "unknown lane $lane"; rc=2;;
  esac
done
echo "first-attempt failures (Retrying lines) per log:"
grep -c '^Retrying' "$OUT"/*.log
echo "tier1 rc=$rc"
exit $rc
