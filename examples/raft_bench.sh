#!/bin/bash
#
# raft_bench.sh — stand up a three-process Raft cluster on the production
# build, run one raft_bench measurement point, and leave exactly one JSON
# record behind.
#
# This is the launcher for src/deptran/raft/raft_bench.cc. It is modelled on
# examples/test_1shard_replication_raft.sh — same randomized-port config
# generation, same preferred-leader grace window, same cleanup trap — but it
# launches ./build/raft_bench directly instead of going through bash/shard.sh
# and dbtest.
#
#   examples/raft_bench.sh --out /tmp/point.json --partitions 1 \
#       --payload-bytes 1024 --rate 5000 --duration-sec 10
#
# See docs/performance/raft-harness.md for what the numbers mean and what
# they deliberately do not mean.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

BUILD_DIR="${BUILD_DIR:-build}"

# ---------------------------------------------------------------------------
# Defaults. Every one of these is also a raft_bench flag; the launcher only
# forwards, it does not reinterpret.
# ---------------------------------------------------------------------------
PARTITIONS=1
REPLICAS=3
PAYLOAD_BYTES=1024
BATCH=1
RATE=0
DURATION_SEC=10
WARMUP_SEC=2
MAX_OUTSTANDING=4096
LEADER_WAIT_SEC=30
LOG_LEVEL=2
GROUP_MODE=single
LABEL=""
OUT_PATH=""
LOG_DIR=""
KEEP_LOGS=0
KILL_LEADER_AT_SEC=0
SNAPSHOT_BYTES=0
STALL_AT_SEC=0
STALL_FOR_SEC=0
BUILD_DIR_P1=""
BUILD_DIR_P2=""

usage() {
    cat <<'EOF'
Usage: examples/raft_bench.sh --out <record.json> [options]

  --out PATH              where to write the run's JSON record (required)
  --partitions N          Raft groups / worker threads (default 1).
                          A config/1leader_2followers/raftN_shardidx0.yml
                          must exist; N in {1,2,3,4,6,8,12,16} ships in-tree.
  --replicas N            recorded for provenance only (default 3)
  --payload-bytes B       entry size in bytes, >= 40 (default 1024)
  --batch K               batch hint forwarded to add_log_to_nc (default 1)
  --rate R                offered entries/sec across all partitions,
                          0 = unthrottled (default 0)
  --duration-sec S        measured window (default 10)
  --warmup-sec S          discarded prefix (default 2)
  --max-outstanding N     per-partition in-flight bound (default 4096)
  --leader-wait-sec S     leadership wait budget (default 30)
  --log-level N           0=FATAL 1=ERROR 2=WARN 3=INFO 4=DEBUG (default 2).
                          Raising this above 2 will change the number: the
                          apply path logs one INFO line per applied entry.
  --group-mode MODE       single|multi (default single)
  --label TEXT            free-form label copied into the record
  --log-dir DIR           per-process stdout/stderr (default: a temp dir)
  --keep-logs             keep --log-dir even on success
  --kill-leader-at-sec S  fault-injection mode: SIGKILL the leader S seconds
                          after it starts offering, then require that every
                          surviving replica still reports a clean log. This
                          produces NO measurement record — it is a correctness
                          test, not a performance point.
  --snapshot-bytes B      register raft_bench's snapshot state machine with
                          B-byte images (0 = none). Snapshots then run when
                          MAKO_RAFT_SNAPSHOTS=1; the run fails unless every
                          partition took at least 10.
  --stall-follower-at-sec S  SIGSTOP one follower S seconds into the load...
  --stall-for-sec D       ...for D seconds (at most 4: longer lets it campaign
                          on SIGCONT), then SIGCONT it, and require that it
                          received an InstallSnapshot RPC, caught up, and that
                          leadership never moved.
  --build-dir DIR         default: $BUILD_DIR or "build"
  --build-dir-p1 DIR      build tree for replica p1 (mixed-lane runs)
  --build-dir-p2 DIR      build tree for replica p2 (mixed-lane runs)
  --help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out)             OUT_PATH="$2"; shift 2 ;;
        --partitions)      PARTITIONS="$2"; shift 2 ;;
        --replicas)        REPLICAS="$2"; shift 2 ;;
        --payload-bytes)   PAYLOAD_BYTES="$2"; shift 2 ;;
        --batch)           BATCH="$2"; shift 2 ;;
        --rate)            RATE="$2"; shift 2 ;;
        --duration-sec)    DURATION_SEC="$2"; shift 2 ;;
        --warmup-sec)      WARMUP_SEC="$2"; shift 2 ;;
        --max-outstanding) MAX_OUTSTANDING="$2"; shift 2 ;;
        --leader-wait-sec) LEADER_WAIT_SEC="$2"; shift 2 ;;
        --log-level)       LOG_LEVEL="$2"; shift 2 ;;
        --group-mode)      GROUP_MODE="$2"; shift 2 ;;
        --label)           LABEL="$2"; shift 2 ;;
        --log-dir)         LOG_DIR="$2"; shift 2 ;;
        --keep-logs)       KEEP_LOGS=1; shift ;;
        --kill-leader-at-sec) KILL_LEADER_AT_SEC="$2"; shift 2 ;;
        --build-dir)       BUILD_DIR="$2"; shift 2 ;;
        --build-dir-p1)    BUILD_DIR_P1="$2"; shift 2 ;;
        --build-dir-p2)    BUILD_DIR_P2="$2"; shift 2 ;;
        --snapshot-bytes)  SNAPSHOT_BYTES="$2"; shift 2 ;;
        --stall-follower-at-sec) STALL_AT_SEC="$2"; shift 2 ;;
        --stall-for-sec)   STALL_FOR_SEC="$2"; shift 2 ;;
        --help|-h)         usage; exit 0 ;;
        *) echo "raft_bench.sh: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -z "$OUT_PATH" ]; then
    echo "raft_bench.sh: --out is required" >&2
    exit 2
fi

# Validate before any arithmetic. The driver validates too, but the launcher
# computes its wait budget from three of these first, and an empty or
# non-numeric value there would produce an empty budget and a wait loop that
# exits immediately.
check_number() {
    local name="$1" value="$2"
    if ! [[ "$value" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        echo "raft_bench.sh: $name must be a non-negative number (got '$value')" >&2
        exit 2
    fi
}
check_number --partitions "$PARTITIONS"
check_number --replicas "$REPLICAS"
check_number --payload-bytes "$PAYLOAD_BYTES"
check_number --batch "$BATCH"
check_number --rate "$RATE"
check_number --duration-sec "$DURATION_SEC"
check_number --warmup-sec "$WARMUP_SEC"
check_number --max-outstanding "$MAX_OUTSTANDING"
check_number --leader-wait-sec "$LEADER_WAIT_SEC"
check_number --log-level "$LOG_LEVEL"
check_number --kill-leader-at-sec "$KILL_LEADER_AT_SEC"
check_number --snapshot-bytes "$SNAPSHOT_BYTES"
check_number --stall-follower-at-sec "$STALL_AT_SEC"
check_number --stall-for-sec "$STALL_FOR_SEC"
KILL_MODE=0
if awk -v s="$KILL_LEADER_AT_SEC" 'BEGIN { exit !(s > 0) }'; then
    KILL_MODE=1
fi
STALL_MODE=0
if awk -v s="$STALL_AT_SEC" 'BEGIN { exit !(s > 0) }'; then
    STALL_MODE=1
    if ! awk -v d="$STALL_FOR_SEC" 'BEGIN { exit !(d > 0 && d <= 4) }'; then
        echo "raft_bench.sh: --stall-for-sec must be in (0, 4] with --stall-follower-at-sec" >&2
        exit 2
    fi
fi
SNAPSHOTS_ON=0
if [ "${MAKO_RAFT_SNAPSHOTS:-}" = "1" ] && awk -v b="$SNAPSHOT_BYTES" 'BEGIN { exit !(b > 0) }'; then
    SNAPSHOTS_ON=1
fi
case "$GROUP_MODE" in
    single|multi) ;;
    *) echo "raft_bench.sh: --group-mode must be single or multi (got '$GROUP_MODE')" >&2; exit 2 ;;
esac

BENCH_BIN="${REPO_ROOT}/${BUILD_DIR}/raft_bench"
# Per-replica binaries: the leader (localhost) always uses BUILD_DIR; p1 and
# p2 may come from other trees, for mixed-lane clusters.
bin_for() {
    local dir="$BUILD_DIR"
    case "$1" in
        p1) [ -n "$BUILD_DIR_P1" ] && dir="$BUILD_DIR_P1" ;;
        p2) [ -n "$BUILD_DIR_P2" ] && dir="$BUILD_DIR_P2" ;;
    esac
    echo "${REPO_ROOT}/${dir}/raft_bench"
}
for _proc in localhost p1 p2; do
    _bin="$(bin_for "$_proc")"
    if [ ! -x "$_bin" ]; then
        echo "raft_bench.sh: $_bin not found. Build it with:" >&2
        echo "    ninja -C $(dirname "$_bin") raft_bench" >&2
        exit 2
    fi
done

SRC_CONFIG="${REPO_ROOT}/config/1leader_2followers/raft${PARTITIONS}_shardidx0.yml"
if [ ! -f "$SRC_CONFIG" ]; then
    echo "raft_bench.sh: no topology config for ${PARTITIONS} partitions:" >&2
    echo "    $SRC_CONFIG" >&2
    echo "Generate one with config/1leader_2followers/raft_generator.py." >&2
    exit 2
fi

MODE_CONFIG="${REPO_ROOT}/config/raft.yml"
if [ ! -f "$MODE_CONFIG" ]; then
    echo "raft_bench.sh: missing $MODE_CONFIG (it sets 'ab: raft')" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Runtime library path. The build links against the dependency prefix the
# CMake cache recorded, and those .so files are not on the default loader
# path on every host. Derive it rather than hardcoding a home directory.
# ---------------------------------------------------------------------------
export LD_LIBRARY_PATH="${REPO_ROOT}/${BUILD_DIR}:${REPO_ROOT}/${BUILD_DIR}/third-party/yaml-cpp${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
for _d in "$BUILD_DIR_P1" "$BUILD_DIR_P2"; do
    [ -n "$_d" ] && export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${REPO_ROOT}/${_d}:${REPO_ROOT}/${_d}/third-party/yaml-cpp"
done
# CMAKE_PREFIX_PATH is a CMake LIST: it may hold several prefixes separated by
# ';'. Splitting matters — an unsplit "/opt/a;/opt/b" makes every candidate a
# nonexistent directory, nothing is added to the loader path, and the three
# processes then die at exec with "error while loading shared libraries", which
# this script would otherwise report as "nobody became leader".
DEP_PREFIX_LIST="$(sed -n 's/^CMAKE_PREFIX_PATH:[A-Z]*=//p' "${REPO_ROOT}/${BUILD_DIR}/CMakeCache.txt" 2>/dev/null | head -1)"
_dep_found=0
if [ -n "$DEP_PREFIX_LIST" ]; then
    IFS=';:' read -r -a _dep_prefixes <<< "$DEP_PREFIX_LIST"
    for prefix in "${_dep_prefixes[@]}"; do
        [ -z "$prefix" ] && continue
        for cand in "${prefix}/lib/x86_64-linux-gnu" "${prefix}/lib" "${prefix}/lib64"; do
            if [ -d "$cand" ]; then
                export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${cand}"
                _dep_found=1
            fi
        done
    done
fi
if [ "$_dep_found" -eq 0 ]; then
    echo "raft_bench.sh: note: no dependency library directory derived from" >&2
    echo "raft_bench.sh: ${REPO_ROOT}/${BUILD_DIR}/CMakeCache.txt (CMAKE_PREFIX_PATH='${DEP_PREFIX_LIST}')." >&2
    echo "raft_bench.sh: If the replicas fail to start, set LD_LIBRARY_PATH yourself:" >&2
    echo "raft_bench.sh:     ldd ${BENCH_BIN} | grep 'not found'" >&2
fi

# ---------------------------------------------------------------------------
# Preferred-leader bias. Same values examples/test_1shard_replication_raft.sh
# uses: a long grace window in which localhost (locale_id 0) wins any election
# and the two followers hold off, so the process that offers load is the one
# the harness expects. Without this a follower can win the first election and
# the run produces no record.
# ---------------------------------------------------------------------------
export MAKO_RAFT_PREFERRED_GRACE_US="${MAKO_RAFT_PREFERRED_GRACE_US:-30000000}"
export MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MIN_US="${MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MIN_US:-5000000}"
export MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MAX_US="${MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MAX_US:-10000000}"

# Provenance the driver cannot obtain on its own without shelling out.
if [ -z "${MAKO_BENCH_COMMIT:-}" ]; then
    _commit="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
    if [ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]; then
        _commit="${_commit}-dirty"
    fi
    export MAKO_BENCH_COMMIT="$_commit"
fi

# ---------------------------------------------------------------------------
# Randomized ports. Two runs back to back must not collide, and a leftover
# listener from an earlier suite must not silently break this one.
# ---------------------------------------------------------------------------
TEMP_CONFIG_DIR="$(cd "$REPO_ROOT" && make_paxos_replication_configs 1 "$PARTITIONS" raft)"
if [ -z "$TEMP_CONFIG_DIR" ] || [ ! -d "$TEMP_CONFIG_DIR" ]; then
    echo "raft_bench.sh: failed to materialize a randomized-port raft config" >&2
    exit 1
fi
TOPOLOGY_CONFIG="${TEMP_CONFIG_DIR}/raft${PARTITIONS}_shardidx0.yml"

OWN_LOG_DIR=0
if [ -z "$LOG_DIR" ]; then
    LOG_DIR="$(mktemp -d /tmp/raft_bench_logs_XXXX)"
    OWN_LOG_DIR=1
fi
mkdir -p "$LOG_DIR"

RECORD_DIR="$(mktemp -d /tmp/raft_bench_rec_XXXX)"
PIDS=()
# Parallel to PIDS: PROCS[i] is the --proc name of PIDS[i]. The flap watcher
# needs both, and pids alone cannot be mapped back to a log file.
PROCS=()

cleanup() {
    local pid
    # The flap watcher sleeps; leaving it alive would let it SIGKILL a pid this
    # run no longer owns after the pid has been recycled.
    [ -n "${WATCHER_PID:-}" ] && kill -9 "$WATCHER_PID" 2>/dev/null
    # The stall watcher too: an orphaned one would SIGSTOP whatever process
    # later reuses the pid it recorded.
    [ -n "${STALL_WATCHER_PID:-}" ] && kill -9 "$STALL_WATCHER_PID" 2>/dev/null
    for pid in "${PIDS[@]:-}"; do
        [ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null
    done
    sleep 1
    for pid in "${PIDS[@]:-}"; do
        [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null
    done
    # Deliberately NOT 'pkill -f "$BENCH_BIN"': that matches every raft_bench
    # started from this checkout, including a concurrent sweep's replicas, and
    # the victim only sees "no process wrote a record". Our own three children
    # are tracked in PIDS. The one broad match that IS safe is this run's own
    # mktemp record directory, which appears in each child's --out and cannot
    # collide with another run.
    [ -n "${RECORD_DIR:-}" ] && pkill -9 -f "$RECORD_DIR" 2>/dev/null
    rm -rf "$TEMP_CONFIG_DIR" "$RECORD_DIR"
    if [ "$OWN_LOG_DIR" -eq 1 ] && [ "$KEEP_LOGS" -eq 0 ] && [ "${RUN_OK:-0}" -eq 1 ]; then
        rm -rf "$LOG_DIR"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Raft disk mode (docs/verus/disk-persistence-plan.md P0): a -DMAKO_RAFT_DISK=ON
# tree gets a fresh, locked store run directory on the local disk, each
# replica creates its store (MAKO_RAFT_CREATE=1), and the directory goes when
# this script exits; a run killed outright is swept by the next launcher.
# MAKO_RAFT_FLUSH_DELAY_US and MAKO_RAFT_DATA_ROOT pass through.
# ---------------------------------------------------------------------------
if grep -qs '^MAKO_RAFT_DISK:BOOL=ON' "${REPO_ROOT}/${BUILD_DIR}/CMakeCache.txt"; then
    # shellcheck source=../scripts/raft_disk/store_dir.sh
    source "${REPO_ROOT}/scripts/raft_disk/store_dir.sh"
    raft_store_make_run bench || { echo "raft_bench.sh: no store run directory" >&2; exit 1; }
    export MAKO_RAFT_CREATE=1
    trap 'cleanup; raft_store_cleanup' EXIT
fi


run_one() {
    local proc="$1"
    # exec, so the backgrounded subshell is REPLACED by raft_bench and the $!
    # the caller records is the driver's own pid. Without it $! names a shell
    # that merely waits, and the timeout path's kill would leave the real
    # process running while the script believed it had cleaned up.
    local bin extra=()
    bin="$(bin_for "$proc")"
    # The phase-N0 flags go only to a binary that knows them, so a pre-N0
    # build still runs under this launcher (the N0 disabled-path comparison).
    local help_text
    help_text="$("$bin" --help 2>&1 || true)"   # --help may exit non-zero
    if grep -q -- '--side-out' <<< "$help_text"; then
        extra+=(--side-out "${RECORD_DIR}/${proc}.side.json")
        if awk -v b="$SNAPSHOT_BYTES" 'BEGIN { exit !(b > 0) }'; then
            extra+=(--snapshot-bytes "$SNAPSHOT_BYTES")
        fi
        if [ "$STALL_MODE" -eq 1 ]; then
            extra+=(--catchup-dir "$RECORD_DIR")
        fi
    elif awk -v b="$SNAPSHOT_BYTES" 'BEGIN { exit !(b > 0) }' || [ "$STALL_MODE" -eq 1 ]; then
        echo "raft_bench.sh: $bin predates --snapshot-bytes/--stall; cannot run this mode" >&2
        exit 2
    fi
    # [M0] G7 failover timing: a survivor that becomes leader times it.
    if [ "$KILL_MODE" -eq 1 ] && grep -q -- '--failover-out' <<< "$help_text"; then
        extra+=(--failover-out "${RECORD_DIR}/${proc}.failover.json")
    fi
    exec "$bin" \
        --proc "$proc" \
        "${extra[@]}" \
        --config "$TOPOLOGY_CONFIG" \
        --config "$MODE_CONFIG" \
        --partitions "$PARTITIONS" \
        --replicas "$REPLICAS" \
        --payload-bytes "$PAYLOAD_BYTES" \
        --batch "$BATCH" \
        --rate "$RATE" \
        --duration-sec "$DURATION_SEC" \
        --warmup-sec "$WARMUP_SEC" \
        --max-outstanding "$MAX_OUTSTANDING" \
        --leader-wait-sec "$LEADER_WAIT_SEC" \
        --log-level "$LOG_LEVEL" \
        --group-mode "$GROUP_MODE" \
        --label "$LABEL" \
        --out "${RECORD_DIR}/${proc}.json" \
        > "${LOG_DIR}/${proc}.log" 2>&1
}

# One budget covering leadership, warmup, the measured window, the drain and
# the follower linger, plus slack for process startup. Computed before launch
# because the flap watcher bounds its own wait by it.
# awk -v, not string interpolation: the arguments are validated above, but a
# program built by pasting user text is a hazard that should not exist at all.
BUDGET=$(awk -v lw="$LEADER_WAIT_SEC" -v w="$WARMUP_SEC" -v d="$DURATION_SEC" \
    'BEGIN { print int(lw + w + d + 120) }')

echo "raft_bench: partitions=$PARTITIONS payload=${PAYLOAD_BYTES}B batch=$BATCH rate=$RATE"
echo "raft_bench: warmup=${WARMUP_SEC}s duration=${DURATION_SEC}s group=$GROUP_MODE label='${LABEL}'"
echo "raft_bench: config=$TOPOLOGY_CONFIG"
echo "raft_bench: logs=$LOG_DIR"

# Followers first, leader last: localhost must find a quorum waiting so it can
# win the first election inside the preferred-leader grace window.
run_one p1 & PIDS+=($!); PROCS+=(p1)
sleep 2
run_one p2 & PIDS+=($!); PROCS+=(p2)
sleep 2
run_one localhost & PIDS+=($!); PROCS+=(localhost)

# ---------------------------------------------------------------------------
# Leadership flap (--kill-leader-at-sec).
#
# Waits for a process to announce that it is offering load, lets it offer for
# S seconds, then SIGKILLs it. SIGKILL, not SIGTERM: a clean shutdown proves
# nothing about what a replica does when the leader vanishes mid-flight.
#
# The leader is discovered from the logs rather than assumed to be localhost.
# The preferred-leader bias above makes localhost win in practice, but a test
# that kills the wrong process and then asserts the survivors are clean would
# pass without ever having removed a leader.
#
# What the survivors must show afterwards is NO gap and NO duplicate in the
# prefix they applied. They cannot show completeness: entries the dead leader
# had accepted but not yet committed are legitimately lost, and entries it
# committed may still be replicating. Truncation at the end of the stream is
# invisible to the sequence check by construction, which is exactly why that
# check is the right assertion here.
# ---------------------------------------------------------------------------
KILLED_MARKER="${RECORD_DIR}/killed_leader"
WATCHER_PID=""
if awk -v s="$KILL_LEADER_AT_SEC" 'BEGIN { exit !(s > 0) }'; then
    (
        watch_deadline=$((SECONDS + BUDGET))
        while [ "$SECONDS" -lt "$watch_deadline" ]; do
            for idx in "${!PROCS[@]}"; do
                if grep -q 'leader; offering load' "${LOG_DIR}/${PROCS[$idx]}.log" 2>/dev/null; then
                    sleep "$KILL_LEADER_AT_SEC"
                    # [M0] G7: CLOCK_REALTIME microseconds, the clock
                    # raft_bench's --failover-out stamps read. %N then /1000,
                    # not %6N: this host's date ignores the %N width.
                    loss_us=$(( $(date +%s%N) / 1000 ))
                    echo "${PROCS[$idx]} ${PIDS[$idx]} ${loss_us}" > "$KILLED_MARKER"
                    echo "raft_bench: killing leader ${PROCS[$idx]} (pid ${PIDS[$idx]}) after ${KILL_LEADER_AT_SEC}s of load"
                    kill -9 "${PIDS[$idx]}" 2>/dev/null
                    exit 0
                fi
            done
            sleep 0.2
        done
        echo "raft_bench: kill-leader watcher timed out; nobody announced leadership" >&2
        exit 1
    ) &
    WATCHER_PID=$!
fi

# ---------------------------------------------------------------------------
# Follower stall (--stall-follower-at-sec S --stall-for-sec D), phase N0.
#
# Once the leader announces it is offering load, wait S seconds, SIGSTOP one
# follower for D seconds, then SIGCONT it. With snapshots on and the interval
# sized below the entries applied during D, the leader compacts past the
# stalled follower, which can then only catch up through InstallSnapshot.
# After SIGCONT the watcher writes "<proc> <ms>" to stall.txt and sends the
# leader SIGUSR1; the leader writes its last_seq values and the follower
# reports catchup_ms (raft_bench.cc, follower_catchup_watcher).
# ---------------------------------------------------------------------------
STALLED_MARKER="${RECORD_DIR}/stalled_follower"
STALL_WATCHER_PID=""
if [ "$STALL_MODE" -eq 1 ]; then
    (
        watch_deadline=$((SECONDS + BUDGET))
        while [ "$SECONDS" -lt "$watch_deadline" ]; do
            for idx in "${!PROCS[@]}"; do
                if grep -q 'leader; offering load' "${LOG_DIR}/${PROCS[$idx]}.log" 2>/dev/null; then
                    leader_pid="${PIDS[$idx]}"
                    victim_idx=""
                    for j in "${!PROCS[@]}"; do
                        [ "$j" = "$idx" ] && continue
                        victim_idx="$j"
                        break
                    done
                    sleep "$STALL_AT_SEC"
                    victim="${PROCS[$victim_idx]}"
                    echo "$victim ${PIDS[$victim_idx]}" > "$STALLED_MARKER"
                    echo "raft_bench: stalling follower $victim (pid ${PIDS[$victim_idx]}) for ${STALL_FOR_SEC}s"
                    kill -STOP "${PIDS[$victim_idx]}" 2>/dev/null
                    sleep "$STALL_FOR_SEC"
                    kill -CONT "${PIDS[$victim_idx]}" 2>/dev/null
                    echo "$victim $(( $(date +%s%N) / 1000000 ))" > "${RECORD_DIR}/stall.txt"
                    kill -USR1 "$leader_pid" 2>/dev/null
                    exit 0
                fi
            done
            sleep 0.2
        done
        echo "raft_bench: stall watcher timed out; nobody announced leadership" >&2
        exit 1
    ) &
    STALL_WATCHER_PID=$!
fi

echo "raft_bench: waiting up to ${BUDGET}s"

waited=0
while [ "$waited" -lt "$BUDGET" ]; do
    alive=0
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            alive=1
            break
        fi
    done
    [ "$alive" -eq 0 ] && break
    sleep 1
    waited=$((waited + 1))
done

TIMED_OUT=0
if [ "$waited" -ge "$BUDGET" ]; then
    echo "raft_bench: TIMEOUT after ${BUDGET}s; killing the replicas" >&2
    KEEP_LOGS=1
    TIMED_OUT=1
    # Kill BEFORE waiting. Without this the wait below blocks forever on the
    # process that caused the timeout, the EXIT trap never runs, and an
    # unattended sweep wedges with three replicas still holding ports.
    for pid in "${PIDS[@]}"; do
        kill -TERM "$pid" 2>/dev/null
    done
    sleep 3
    for pid in "${PIDS[@]}"; do
        kill -9 "$pid" 2>/dev/null
    done
fi

# Collect exit status per process. Every child is now either finished or
# killed, so this cannot block. The driver's own exit codes are meaningful —
# 3 leadership lost, 4 partial leadership, 6 nothing applied in the window,
# 7 no leader anywhere — and a run where any of them fired is not a
# measurement, even if a record was written.
KILLED_PID=""
KILLED_PROC=""
if [ -s "$KILLED_MARKER" ]; then
    read -r KILLED_PROC KILLED_PID KILLED_AT_US < "$KILLED_MARKER"
fi

WORST_CHILD_STATUS=0
for pid in "${PIDS[@]}"; do
    wait "$pid" 2>/dev/null
    child_status=$?
    # The process this run deliberately SIGKILLed exits 137 by construction.
    # Folding that into the worst status would make the flap test fail on the
    # one thing it set out to do.
    if [ -n "$KILLED_PID" ] && [ "$pid" = "$KILLED_PID" ]; then
        continue
    fi
    if [ "$child_status" -gt "$WORST_CHILD_STATUS" ]; then
        WORST_CHILD_STATUS="$child_status"
    fi
done

# ---------------------------------------------------------------------------
# Flap mode verdict. There is no record to check — the process that would have
# written one is the process we killed — so the assertion is entirely on the
# survivors: each must have finished its own integrity check and found the
# prefix it applied to be gap-free and duplicate-free.
# ---------------------------------------------------------------------------
if awk -v s="$KILL_LEADER_AT_SEC" 'BEGIN { exit !(s > 0) }'; then
    [ -n "$WATCHER_PID" ] && wait "$WATCHER_PID" 2>/dev/null
    if [ -z "$KILLED_PID" ]; then
        echo "raft_bench: FAILED — flap test killed nobody: no process ever announced" >&2
        echo "raft_bench: that it was offering load. Logs kept in $LOG_DIR" >&2
        KEEP_LOGS=1
        exit 1
    fi
    echo "raft_bench: flap test killed leader '$KILLED_PROC'; checking survivors"
    survivors=0
    for idx in "${!PROCS[@]}"; do
        proc="${PROCS[$idx]}"
        [ "${PIDS[$idx]}" = "$KILLED_PID" ] && continue
        survivors=$((survivors + 1))
        if ! grep -q 'log integrity: OK' "${LOG_DIR}/${proc}.log" 2>/dev/null; then
            echo "raft_bench: FAILED — survivor '$proc' did not report a clean log" >&2
            echo "--- tail ${LOG_DIR}/${proc}.log ---" >&2
            tail -n 20 "${LOG_DIR}/${proc}.log" >&2 2>/dev/null
            KEEP_LOGS=1
            exit 8
        fi
        grep -h 'log integrity: OK' "${LOG_DIR}/${proc}.log"
    done
    if [ "$WORST_CHILD_STATUS" -ne 0 ]; then
        echo "raft_bench: FAILED — a survivor exited with status $WORST_CHILD_STATUS." >&2
        echo "raft_bench: Logs kept in $LOG_DIR" >&2
        KEEP_LOGS=1
        exit "$WORST_CHILD_STATUS"
    fi
    echo "raft_bench: flap test PASSED — $survivors survivors applied a gap-free," \
         "duplicate-free prefix after the leader was SIGKILLed"
    # [M0] G7: the run's record. new_leader_us / first_commit_us are the
    # earliest over the survivors that became leader; a record without them
    # is a kill whose failover was not timed (scripts/verus/election_times.py
    # counts it as failed).
    mkdir -p "$(dirname "$OUT_PATH")"
    python3 - "$OUT_PATH" "$RECORD_DIR" "$KILLED_PROC" "${KILLED_AT_US:-}" <<'PYKILL' || { echo "raft_bench: failover record failed" >&2; exit 1; }
import glob, json, os, sys
out, rdir, killed, loss = sys.argv[1:5]
rec = {"mode": "kill_leader", "killed_proc": killed}
if loss:
    rec["leader_loss_us"] = int(loss)
recs = []
for p in glob.glob(os.path.join(rdir, "*.failover.json")):
    try:
        recs.append(json.load(open(p)))
    except ValueError:
        pass
nl = [r for r in recs if r.get("new_leader_us")]
fc = [r for r in recs if r.get("first_commit_us")]
if nl:
    first = min(nl, key=lambda r: r["new_leader_us"])
    rec["new_leader_us"] = first["new_leader_us"]
    rec["new_leader_proc"] = first["proc"]
if fc:
    rec["first_commit_us"] = min(r["first_commit_us"] for r in fc)
json.dump(rec, open(out, "w"), indent=2)
PYKILL
    echo "raft_bench: record -> $OUT_PATH"
    cat "$OUT_PATH"
    RUN_OK=1
    exit 0
fi

if [ "$TIMED_OUT" -eq 1 ]; then
    # A run that had to be killed is not a measurement, even if the leader
    # managed to write a record before the straggler wedged.
    echo "raft_bench: FAILED — run exceeded its ${BUDGET}s budget. Logs kept in $LOG_DIR" >&2
    exit 5
fi

# Exactly one process — the one that actually led — writes a record. More
# than one means two processes each thought they were the leader and each ran
# a measurement, which invalidates both.
RECORD=""
RECORD_COUNT=0
for proc in localhost p1 p2; do
    if [ -s "${RECORD_DIR}/${proc}.json" ]; then
        RECORD_COUNT=$((RECORD_COUNT + 1))
        [ -z "$RECORD" ] && RECORD="${RECORD_DIR}/${proc}.json"
    fi
done

if [ "$RECORD_COUNT" -gt 1 ]; then
    echo "raft_bench: FAILED — $RECORD_COUNT processes wrote a record; leadership" >&2
    echo "raft_bench: was not exclusive, so neither measurement is trustworthy." >&2
    echo "raft_bench: logs kept in $LOG_DIR" >&2
    KEEP_LOGS=1
    exit 4
fi

if [ -z "$RECORD" ]; then
    echo "raft_bench: FAILED — no process wrote a record." >&2
    echo "raft_bench: nobody became leader, or the leader aborted. Logs kept in $LOG_DIR" >&2
    for proc in localhost p1 p2; do
        echo "--- tail ${LOG_DIR}/${proc}.log ---" >&2
        tail -n 15 "${LOG_DIR}/${proc}.log" >&2 2>/dev/null
    done
    KEEP_LOGS=1
    exit 1
fi

mkdir -p "$(dirname "$OUT_PATH")"
cp "$RECORD" "$OUT_PATH"
# Merge every other replica's side record into the leader's, as flat
# "<proc>_<field>" keys ("Flat, no nesting", see raft_bench.cc's Record).
LEADER_PROC="$(basename "$RECORD" .json)"
python3 - "$OUT_PATH" "$RECORD_DIR" "$LEADER_PROC" "$STALL_MODE" <<'PYMERGE' || { echo "raft_bench: side-record merge failed" >&2; exit 1; }
import json, os, sys
out, rdir, leader, stall = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] == "1"
rec = json.load(open(out))
for proc in ("localhost", "p1", "p2"):
    if proc == leader:
        continue
    side = os.path.join(rdir, proc + ".side.json")
    if os.path.exists(side):
        for k, v in json.load(open(side)).items():
            if k != "proc":
                rec[f"{proc}_{k}"] = v
stalled = os.path.join(rdir, "stalled_follower")
if stall and os.path.exists(stalled):
    rec["stalled_follower"] = open(stalled).read().split()[0]
json.dump(rec, open(out, "w"), indent=2)
PYMERGE
echo "raft_bench: record -> $OUT_PATH"
grep -E '"(applied_per_sec|latency_p50_us|latency_p99_us|offered_in_window|applied_in_window|offer_rejected|peak_outstanding|out_of_order|gaps|duplicates|foreign_applied)"' "$OUT_PATH"

# Log integrity first. The driver already exits 8 for this and WORST_CHILD_STATUS
# would carry it, but checking the record too names the failure here rather than
# leaving the sweep to report a bare status number, and it catches the case where
# the leader wrote a violated record and then died for some other reason.
for field in out_of_order gaps duplicates foreign_applied; do
    value="$(sed -n "s/.*\"${field}\": \([0-9]*\).*/\1/p" "$OUT_PATH")"
    if [ -n "$value" ] && [ "$value" -gt 0 ]; then
        echo "raft_bench: FAILED — log integrity violation: ${field}=${value}" >&2
        echo "raft_bench: the replicated log lost, duplicated or reordered an entry;" >&2
        echo "raft_bench: this is a correctness failure, not a slow measurement." >&2
        KEEP_LOGS=1
        exit 8
    fi
done

# A run whose leader lost leadership mid-window is not a measurement.
rejected="$(sed -n 's/.*"offer_rejected": \([0-9]*\).*/\1/p' "$OUT_PATH")"
if [ -n "$rejected" ] && [ "$rejected" -gt 0 ]; then
    echo "raft_bench: FAILED — $rejected offers rejected (leadership lost mid-run)" >&2
    KEEP_LOGS=1
    exit 3
fi

# Nor is one where nothing was applied inside the window — the record would
# still carry a full set of zeroed percentiles and a zeroed CDF.
applied="$(sed -n 's/.*"applied_in_window": \([0-9]*\).*/\1/p' "$OUT_PATH")"
if [ -z "$applied" ] || [ "$applied" -eq 0 ]; then
    echo "raft_bench: FAILED — nothing was applied inside the measured window" >&2
    KEEP_LOGS=1
    exit 6
fi

# ---------------------------------------------------------------------------
# Phase N0 validity checks.
# ---------------------------------------------------------------------------
json_field() {
    python3 -c 'import json,sys; v=json.load(open(sys.argv[1])).get(sys.argv[2]); print("" if v is None else v)' "$OUT_PATH" "$1"
}
if [ "$SNAPSHOTS_ON" -eq 1 ]; then
    created="$(json_field snapshots_created)"
    if [ -z "$created" ] || [ "$created" -lt 10 ]; then
        echo "raft_bench: FAILED — snapshots are on but only '${created}' were taken per" >&2
        echo "raft_bench: partition (need >= 10). Lengthen the run or lower MAKO_RAFT_SNAPSHOT_INTERVAL." >&2
        KEEP_LOGS=1
        exit 9
    fi
fi
if [ "$STALL_MODE" -eq 1 ]; then
    [ -n "$STALL_WATCHER_PID" ] && wait "$STALL_WATCHER_PID" 2>/dev/null
    victim="$(json_field stalled_follower)"
    if [ -z "$victim" ]; then
        echo "raft_bench: FAILED — the stall watcher stalled nobody" >&2
        KEEP_LOGS=1
        exit 10
    fi
    # The initial election is one notification; anything more is a flap.
    changes="$(json_field leadership_changes)"
    if [ -n "$changes" ] && [ "$changes" -gt 1 ]; then
        echo "raft_bench: FAILED — leadership moved during the stall run ($changes notifications)" >&2
        KEEP_LOGS=1
        exit 10
    fi
    if [ "$SNAPSHOTS_ON" -eq 1 ]; then
        received="$(json_field "${victim}_install_rpcs_received")"
        if [ -z "$received" ] || [ "$received" -lt 1 ]; then
            echo "raft_bench: FAILED — stalled follower $victim received no InstallSnapshot RPC" >&2
            echo "raft_bench: (size MAKO_RAFT_SNAPSHOT_INTERVAL below the entries applied during the stall)" >&2
            KEEP_LOGS=1
            exit 10
        fi
    fi
    # The survivor check the flap mode runs, for the two followers.
    for proc in localhost p1 p2; do
        [ "$proc" = "$LEADER_PROC" ] && continue
        if ! grep -q 'log integrity: OK' "${LOG_DIR}/${proc}.log" 2>/dev/null; then
            echo "raft_bench: FAILED — follower '$proc' did not report a clean log after the stall" >&2
            tail -n 20 "${LOG_DIR}/${proc}.log" >&2 2>/dev/null
            KEEP_LOGS=1
            exit 8
        fi
    done
    echo "raft_bench: stall of $victim: install_rpcs_received=$(json_field "${victim}_install_rpcs_received") catchup_ms=$(json_field "${victim}_catchup_ms")"
fi

# Finally, honour the driver's own verdict. Without this a driver that exited
# non-zero for a reason the record does not name would still be counted as a
# clean run by the sweep driver.
if [ "$WORST_CHILD_STATUS" -ne 0 ]; then
    echo "raft_bench: FAILED — a replica exited with status $WORST_CHILD_STATUS." >&2
    echo "raft_bench: Logs kept in $LOG_DIR" >&2
    KEEP_LOGS=1
    exit "$WORST_CHILD_STATUS"
fi

RUN_OK=1
exit 0
