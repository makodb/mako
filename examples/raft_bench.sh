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
  --build-dir DIR         default: $BUILD_DIR or "build"
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
        --build-dir)       BUILD_DIR="$2"; shift 2 ;;
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
case "$GROUP_MODE" in
    single|multi) ;;
    *) echo "raft_bench.sh: --group-mode must be single or multi (got '$GROUP_MODE')" >&2; exit 2 ;;
esac

BENCH_BIN="${REPO_ROOT}/${BUILD_DIR}/raft_bench"
if [ ! -x "$BENCH_BIN" ]; then
    echo "raft_bench.sh: $BENCH_BIN not found. Build it with:" >&2
    echo "    ninja -C ${BUILD_DIR} raft_bench" >&2
    exit 2
fi

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

cleanup() {
    local pid
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


run_one() {
    local proc="$1"
    # exec, so the backgrounded subshell is REPLACED by raft_bench and the $!
    # the caller records is the driver's own pid. Without it $! names a shell
    # that merely waits, and the timeout path's kill would leave the real
    # process running while the script believed it had cleaned up.
    exec "$BENCH_BIN" \
        --proc "$proc" \
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

echo "raft_bench: partitions=$PARTITIONS payload=${PAYLOAD_BYTES}B batch=$BATCH rate=$RATE"
echo "raft_bench: warmup=${WARMUP_SEC}s duration=${DURATION_SEC}s group=$GROUP_MODE label='${LABEL}'"
echo "raft_bench: config=$TOPOLOGY_CONFIG"
echo "raft_bench: logs=$LOG_DIR"

# Followers first, leader last: localhost must find a quorum waiting so it can
# win the first election inside the preferred-leader grace window.
run_one p1 & PIDS+=($!)
sleep 2
run_one p2 & PIDS+=($!)
sleep 2
run_one localhost & PIDS+=($!)

# One budget covering leadership, warmup, the measured window, the drain and
# the follower linger, plus slack for process startup.
# awk -v, not string interpolation: the arguments are validated above, but a
# program built by pasting user text is a hazard that should not exist at all.
BUDGET=$(awk -v lw="$LEADER_WAIT_SEC" -v w="$WARMUP_SEC" -v d="$DURATION_SEC" \
    'BEGIN { print int(lw + w + d + 120) }')
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
WORST_CHILD_STATUS=0
for pid in "${PIDS[@]}"; do
    wait "$pid" 2>/dev/null
    child_status=$?
    if [ "$child_status" -gt "$WORST_CHILD_STATUS" ]; then
        WORST_CHILD_STATUS="$child_status"
    fi
done

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
echo "raft_bench: record -> $OUT_PATH"
grep -E '"(applied_per_sec|latency_p50_us|latency_p99_us|offered_in_window|applied_in_window|offer_rejected|peak_outstanding)"' "$OUT_PATH"

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
