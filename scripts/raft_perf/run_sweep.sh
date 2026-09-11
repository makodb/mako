#!/usr/bin/env bash
# scripts/raft_perf/run_sweep.sh — sweep driver for the standalone Raft
# performance harness.
#
# Runs examples/raft_bench.sh over declared axes and drops one JSON record per
# run into a timestamped output directory, ready for
# scripts/raft_perf/processing.py and the two plot scripts.
#
# Usage:
#   ./run_sweep.sh                     # full sweep (hours — the estimate is printed)
#   ./run_sweep.sh --quick             # one point per axis, a few minutes
#   ./run_sweep.sh --dry-run           # print the whole matrix, run nothing
#   ./run_sweep.sh --phase rate        # saturation sweep only (the main deliverable)
#   ./run_sweep.sh --phase payload     # payload sweep at the comparison rate
#   ./run_sweep.sh --phase batch       # batch sweep at the comparison rate
#   ./run_sweep.sh --phase groups      # single vs multi group mode at 6 partitions
#   ./run_sweep.sh --phase summary     # (re)write SUMMARY.md for an existing dir
#   ./run_sweep.sh --output <dir>      # override the output root
#   ./run_sweep.sh --trials N          # repetitions per point (default 3)
#   ./run_sweep.sh --help
#
# Output: <root>/sweep_<timestamp>/{rate,payload,batch,groups}/*.json + SUMMARY.md
#
# Ported from ~/jetpack/ae/local/run.sh. What was kept is the SHAPE: phase
# selection, --quick, --dry-run, a timestamped output directory, the logging
# helpers, and above all the discipline of holding every axis in a declared
# array at the top of the file instead of scattering values through the logic.
# What was replaced is every axis: jetpack swept nine consensus protocols by
# client concurrency, Zipf skew and key range; this sweeps one protocol by
# offered rate, entry size, batch hint, partition count and Raft group mode.
#
# See docs/performance/raft-harness.md for what the numbers mean.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BENCH_SH="${REPO_ROOT}/examples/raft_bench.sh"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
COMMIT_HASH="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
DEFAULT_OUTPUT="${REPO_ROOT}/raft_perf_output/sweep_${TIMESTAMP}"

OUTPUT_DIR="${OUTPUT_DIR:-$DEFAULT_OUTPUT}"
PHASE="all"
QUICK=false
DRY_RUN=false
TRIALS=3
TRIALS_EXPLICIT=false

# This must come BEFORE the first `declare -A` below: on bash 3.2 (still
# /bin/bash on macOS) the script would otherwise die at the declaration with
# "declare: -A: invalid option", and a friendly check further down would never
# be reached.
if [ "${BASH_VERSINFO[0]}" -lt 4 ]; then
    echo "[ERROR] Bash 4+ required (associative arrays); this is ${BASH_VERSINFO[0]}.${BASH_VERSINFO[1]}" >&2
    echo "[ERROR] On macOS: brew install bash, then run with that bash." >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# The axes. Every number below is data, not logic — change it here, nowhere
# else. The derivations are in docs/plans/raft-perf-profile.txt (what Mako
# actually submits) and in the knee measurements quoted beside each array.
# ---------------------------------------------------------------------------

# The three cluster shapes, and only these three (harness plan decision D5).
# "1 partition" is the low-variance regression gate. "6 partitions" is what
# production runs. The two group modes at six partitions bracket the cost of
# the shared recursive mutex that single-group mode puts every partition
# behind. Sweeping every partition count is deliberately out of scope.
#
# NOTE ON multi ABOVE ONE PARTITION: as of 2026-09-11 per-partition group mode
# does not elect a leader above one partition on this tree — three processes
# enter an unbounded election storm. That is a pre-existing defect in the Raft
# implementation, not in this harness (multi at 1 partition works; single at 6
# works). See docs/performance/raft-harness.md.
#
# It is therefore NOT in CONFIGS, which every phase iterates. Carrying it
# through the rate sweep would add 189 identical two-minute failures and
# double the wall clock of the whole sweep for no information. It lives in the
# groups phase instead (GROUPS_MODES below), which exists precisely to compare
# the two modes: one failure there is the finding, 210 of them are noise.
CONFIGS=(
    "1:single"
    "6:single"
)

# Entry sizes, bracketing the profile. The middle value is the measured p50 of
# a real TPC-C Raft entry, 286208 bytes, NOT the arithmetic mean (~310 KB):
# the mean is dragged upward by the 13 MB and 25 MB single entries that
# docs/plans/raft-perf-profile.txt records as rare events rather than workload,
# so the p50 is the honest "typical entry". 4096 is "much smaller" and 1048576
# is "much larger" while still inside the observed tail.
PAYLOAD_SMALL=4096
PAYLOAD_PROFILE=286208
PAYLOAD_LARGE=1048576
PAYLOAD_BYTES=($PAYLOAD_SMALL $PAYLOAD_PROFILE $PAYLOAD_LARGE)

# In-flight bound per partition, per payload size. This is a memory bound, not
# a tuning knob: RaftWorker::submit_queue_ is an unbounded deque holding a COPY
# of every payload, so 4096 in flight at 286 KB would be 1.2 GB per partition.
# Each value below keeps the in-flight bytes near 16-64 MB per partition.
declare -A MAX_OUTSTANDING=(
    [$PAYLOAD_SMALL]=4096
    [$PAYLOAD_PROFILE]=256
    [$PAYLOAD_LARGE]=64
)

# Offered rate, in entries/sec across all partitions. One array per payload,
# because the achievable rate is bandwidth-bound and a single array cannot
# bracket the knee for a 4 KB entry and a 1 MB entry at once.
#
# Shape ported from jetpack's Raft concurrency array (24 points from 1 to 2000,
# dense around the knee at 150, sparse above it), re-expressed as fractions of
# the measured unthrottled rate K: 0.02 0.05 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.75
# 0.8 0.85 0.9 0.95 1.0 1.1 1.25 1.5 2.0 3.0, then 0 (unthrottled) last.
#
# K measured on zoo-003 (Linux 7.0.0-29, clang Release MODE=perf), unthrottled,
# single group mode, 8 s window. One partition and six partitions agree closely,
# which says the ceiling is a shared loopback/bandwidth limit rather than a
# per-partition one:
#   4096 B    -> 11872 entries/s (1 par), 11805 (6 par)  ~ 49 MB/s
#   286208 B  ->   225 entries/s (1 par),   262 (6 par)  ~ 64-75 MB/s
#   1048576 B ->    65 entries/s (1 par),    64 (6 par)  ~ 67 MB/s
# For calibration, the real TPC-C workload profiled in
# docs/plans/raft-perf-profile.txt submits ~303 entries/s of ~290 KB, i.e. Mako
# in production sits right at this ceiling.
# Re-measure on a new machine with:  ./run_sweep.sh --phase knee
declare -A RATES=(
    [$PAYLOAD_SMALL]="240 600 1200 2400 3600 4800 6000 7100 8300 8900 9500 10100 10700 11300 11900 13100 14900 17900 23800 35700 0"
    [$PAYLOAD_PROFILE]="5 11 23 45 68 90 113 135 158 169 180 191 203 214 225 248 281 338 450 675 0"
    [$PAYLOAD_LARGE]="1 3 7 13 20 26 33 39 46 49 52 55 59 62 65 72 81 98 130 195 0"
)

# The rate every non-rate phase is held at, PER PAYLOAD. Jetpack called the
# equivalent FIXED_CONC and set it to the knee; here the knee is a different
# number for each entry size, so one global constant cannot serve. A single
# FIXED_RATE=200 would sit just under the 286 KB knee (225) and three times
# ABOVE the 1 MB knee (65) — the payload phase would then show a latency cliff
# at 1 MB that a reader would attribute to entry size when it was only "this
# point was driven past saturation and the others were not".
#
# Each value is ~0.85 of that payload's measured unthrottled rate: high enough
# to be interesting, low enough that every configuration can actually deliver
# it, so a shortfall means something real.
declare -A FIXED_RATE_FOR=(
    [$PAYLOAD_SMALL]=10000
    [$PAYLOAD_PROFILE]=190
    [$PAYLOAD_LARGE]=55
)

# Raft's batch hint. 1 is "no drain coalescing"; 400 is what
# src/mako/sto/Transaction.cc passes. The profile says the observed batch is
# 1.04 either way, so a flat curve here is the expected result and worth
# recording rather than assuming.
BATCH_SIZES=(1 400)

# Per-run timing. 10 s is long enough that a 5 ms heartbeat and a 1 ms apply
# poll average out; 3 s of warmup covers leadership settling and the first
# AppendEntries catch-up.
DURATION_SEC=10
WARMUP_SEC=3
LEADER_WAIT_SEC=30

# Fixed per-run overhead in seconds: process start, election, drain, teardown.
# Measured at ~15 s on zoo-003; used only for the runtime estimate.
RUN_OVERHEAD_SEC=15

# The groups phase's own axes, declared rather than hardcoded in the function.
GROUPS_PARTITIONS=6
GROUPS_MODES=(single multi)

# --quick values: one point per axis, enough to prove the pipeline end to end.
# It stays on 1 partition and single-group mode throughout, so a smoke test
# does not spend four minutes reproducing a failure the file already documents.
QUICK_CONFIGS=("1:single")
QUICK_PAYLOADS=($PAYLOAD_SMALL)
QUICK_RATES="2000 0"
QUICK_BATCHES=(1)
QUICK_TRIALS=1
QUICK_DURATION_SEC=4
QUICK_WARMUP_SEC=1
QUICK_LEADER_WAIT_SEC=15
QUICK_GROUPS_PARTITIONS=1
QUICK_GROUPS_MODES=(single)

# ---------------------------------------------------------------------------
# Helpers (ported from jetpack ae/local/run.sh)
# ---------------------------------------------------------------------------
log_info()  { echo "[INFO]  $(date '+%H:%M:%S') $*"; }
log_warn()  { echo "[WARN]  $(date '+%H:%M:%S') $*" >&2; }
log_error() { echo "[ERROR] $(date '+%H:%M:%S') $*" >&2; }
log_step()  { echo ""; echo "====== $* ======"; echo ""; }

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
    exit "${1:-0}"
}

RUNS_PLANNED=0
RUNS_OK=0
RUNS_FAILED=0
FAILED_LABELS=()

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --phase)    PHASE="$2"; shift 2 ;;
        --output)   OUTPUT_DIR="$2"; shift 2 ;;
        --trials)   TRIALS="$2"; TRIALS_EXPLICIT=true; shift 2 ;;
        --quick)    QUICK=true; shift ;;
        --dry-run)  DRY_RUN=true; shift ;;
        --help|-h)  usage 0 ;;
        *)          log_error "Unknown arg: $1"; usage 2 ;;
    esac
done

if $QUICK; then
    CONFIGS=("${QUICK_CONFIGS[@]}")
    PAYLOAD_BYTES=("${QUICK_PAYLOADS[@]}")
    BATCH_SIZES=("${QUICK_BATCHES[@]}")
    DURATION_SEC=$QUICK_DURATION_SEC
    WARMUP_SEC=$QUICK_WARMUP_SEC
    # A quick run is a smoke test, so it should not also sit through the full
    # leadership budget; the preferred leader normally wins in under 3 s.
    LEADER_WAIT_SEC=$QUICK_LEADER_WAIT_SEC
    # The phases that hold a payload or a partition count fixed must follow
    # --quick too, or a "kick the tires" run still performs a 6-partition
    # 286 KB per-partition-group run — the known-failing configuration, at a
    # full leader-wait timeout each.
    PAYLOAD_PROFILE=${QUICK_PAYLOADS[0]}
    GROUPS_PARTITIONS=$QUICK_GROUPS_PARTITIONS
    GROUPS_MODES=("${QUICK_GROUPS_MODES[@]}")
    for pb in "${QUICK_PAYLOADS[@]}"; do
        RATES[$pb]="$QUICK_RATES"
    done
    if $TRIALS_EXPLICIT; then
        log_info "--quick with an explicit --trials $TRIALS; honouring the flag"
    else
        TRIALS=$QUICK_TRIALS
    fi
fi

# ---------------------------------------------------------------------------
# Pre-flight. Fail with a fixable message rather than 200 runs into a sweep.
# ---------------------------------------------------------------------------
preflight() {
    local fatal=0
    if [[ ${BASH_VERSINFO[0]} -lt 4 ]]; then
        log_error "Bash 4+ required (associative arrays); this is ${BASH_VERSINFO[0]}.${BASH_VERSINFO[1]}"
        fatal=1
    fi
    if [ ! -x "$BENCH_SH" ]; then
        log_error "Launcher not found or not executable: $BENCH_SH"
        fatal=1
    fi
    # A dry run prints the matrix; it must not require a build, so the
    # acceptance check can be performed on a machine that has not built yet.
    if ! $DRY_RUN && [ ! -x "${REPO_ROOT}/${BUILD_DIR:-build}/raft_bench" ]; then
        log_error "raft_bench not built. Run: ninja -C ${BUILD_DIR:-build} raft_bench"
        fatal=1
    fi
    for entry in "${CONFIGS[@]}"; do
        local parts="${entry%%:*}"
        if [ ! -f "${REPO_ROOT}/config/1leader_2followers/raft${parts}_shardidx0.yml" ]; then
            log_error "No topology config for ${parts} partitions; generate it with config/1leader_2followers/raft_generator.py"
            fatal=1
        fi
    done
    if ! command -v python3 >/dev/null 2>&1; then
        log_warn "python3 not found — the port-randomizing config helper needs it"
    fi
    if [ "$fatal" -ne 0 ]; then
        log_error "Pre-flight failed."
        exit 2
    fi
}

# ---------------------------------------------------------------------------
# One measurement point, repeated TRIALS times.
# ---------------------------------------------------------------------------
# Every payload must appear in all three payload-keyed maps. Defaulting one of
# them silently is worse than failing: a missing MAX_OUTSTANDING entry would
# run 4096 in-flight copies of a 1 MB payload, which is 4 GB per partition and
# exactly what those bounds exist to prevent.
require_payload_maps() {
    local payload="$1" missing=""
    [ -z "${RATES[$payload]+set}" ] && missing="$missing RATES"
    [ -z "${MAX_OUTSTANDING[$payload]+set}" ] && missing="$missing MAX_OUTSTANDING"
    [ -z "${FIXED_RATE_FOR[$payload]+set}" ] && missing="$missing FIXED_RATE_FOR"
    if [ -n "$missing" ]; then
        log_error "payload $payload is missing from:$missing"
        log_error "Add it to every payload-keyed map at the top of $0."
        exit 2
    fi
}

run_point() {
    local phase="$1" parts="$2" group="$3" payload="$4" batch="$5" rate="$6"
    require_payload_maps "$payload"
    local maxout="${MAX_OUTSTANDING[$payload]}"
    local out_dir="${OUTPUT_DIR}/${phase}"
    local trial

    for ((trial = 1; trial <= TRIALS; trial++)); do
        # The commit is part of the filename so that re-running a sweep into
        # the same --output directory after a code change adds points rather
        # than overwriting some and leaving others stale — which would blend
        # two commits into one curve.
        local name="${COMMIT_HASH}-p${parts}-${group}-pb${payload}-b${batch}-r${rate}-t${trial}"
        RUNS_PLANNED=$((RUNS_PLANNED + 1))
        if $DRY_RUN; then
            echo "  [DRY-RUN] $phase $name"
            continue
        fi
        mkdir -p "$out_dir"
        log_info "$phase: $name"
        if "$BENCH_SH" \
            --out "${out_dir}/${name}.json" \
            --partitions "$parts" \
            --group-mode "$group" \
            --payload-bytes "$payload" \
            --batch "$batch" \
            --rate "$rate" \
            --max-outstanding "$maxout" \
            --duration-sec "$DURATION_SEC" \
            --warmup-sec "$WARMUP_SEC" \
            --leader-wait-sec "$LEADER_WAIT_SEC" \
            --label "$phase" \
            --log-dir "${out_dir}/${name}.logs" \
            --keep-logs \
            > "${out_dir}/${name}.log" 2>&1; then
            RUNS_OK=$((RUNS_OK + 1))
        else
            RUNS_FAILED=$((RUNS_FAILED + 1))
            FAILED_LABELS+=("$phase/$name")
            log_warn "$phase: $name FAILED (see ${out_dir}/${name}.log)"
        fi
    done
}

# ---------------------------------------------------------------------------
# Phase: rate — the saturation sweep. This is the main deliverable; every other
# phase is a slice through a single point of it.
# ---------------------------------------------------------------------------
phase_rate() {
    log_step "Phase: rate (saturation sweep)"
    local entry parts group payload rate
    for entry in "${CONFIGS[@]}"; do
        parts="${entry%%:*}"; group="${entry##*:}"
        for payload in "${PAYLOAD_BYTES[@]}"; do
            for rate in ${RATES[$payload]}; do
                run_point rate "$parts" "$group" "$payload" 1 "$rate"
            done
        done
    done
}

# ---------------------------------------------------------------------------
# Phase: payload — entry size at the fixed comparison rate.
# ---------------------------------------------------------------------------
phase_payload() {
    log_step "Phase: payload (entry size, each at ~0.85 of its own saturation rate)"
    local entry parts group payload
    for entry in "${CONFIGS[@]}"; do
        parts="${entry%%:*}"; group="${entry##*:}"
        for payload in "${PAYLOAD_BYTES[@]}"; do
            require_payload_maps "$payload"
            run_point payload "$parts" "$group" "$payload" 1 "${FIXED_RATE_FOR[$payload]}"
        done
    done
}

# ---------------------------------------------------------------------------
# Phase: batch — the add_log_to_nc batch hint at the fixed comparison rate.
# ---------------------------------------------------------------------------
phase_batch() {
    log_step "Phase: batch (drain hint at ${FIXED_RATE_FOR[$PAYLOAD_PROFILE]} entries/s)"
    local entry parts group batch
    for entry in "${CONFIGS[@]}"; do
        parts="${entry%%:*}"; group="${entry##*:}"
        for batch in "${BATCH_SIZES[@]}"; do
            run_point batch "$parts" "$group" "$PAYLOAD_PROFILE" "$batch" "${FIXED_RATE_FOR[$PAYLOAD_PROFILE]}"
        done
    done
}

# ---------------------------------------------------------------------------
# Phase: groups — single vs per-partition Raft instances at 6 partitions, the
# comparison decision D5 exists for. Unthrottled as well as at the fixed rate,
# because the shared recursive mutex only bites under contention.
# ---------------------------------------------------------------------------
phase_groups() {
    log_step "Phase: groups (${GROUPS_MODES[*]} at ${GROUPS_PARTITIONS} partitions)"
    local group rate
    local rates=("${FIXED_RATE_FOR[$PAYLOAD_PROFILE]}" 0)
    $QUICK && rates=("${FIXED_RATE_FOR[$PAYLOAD_PROFILE]}")
    for group in "${GROUPS_MODES[@]}"; do
        for rate in "${rates[@]}"; do
            run_point groups "$GROUPS_PARTITIONS" "$group" "$PAYLOAD_PROFILE" 1 "$rate"
        done
    done
}

# ---------------------------------------------------------------------------
# Phase: knee — one unthrottled run per payload, to re-derive the RATES arrays
# on a new machine. Not part of --phase all; run it, read the numbers, edit the
# arrays at the top of this file.
# ---------------------------------------------------------------------------
phase_knee() {
    log_step "Phase: knee (unthrottled rate per payload, for re-deriving RATES)"
    local payload
    for payload in "${PAYLOAD_BYTES[@]}"; do
        run_point knee 1 single "$payload" 1 0
    done
    if ! $DRY_RUN; then
        log_info "Unthrottled rates measured — use these to rescale RATES in $0:"
        grep -H '"applied_per_sec"' "${OUTPUT_DIR}"/knee/*.json 2>/dev/null || true
    fi
}

# ---------------------------------------------------------------------------
# Runtime estimate. Printed before anything runs, so the operator knows what
# they started.
# ---------------------------------------------------------------------------
# A run costs one of two very different amounts of time, so a flat per-run
# figure would understate the sweep by about a factor of two. A healthy run is
# warmup + window + fixed overhead. A run in a configuration that cannot elect
# a leader — per-partition group mode above one partition, which CONFIGS
# declares deliberately — pays the whole leadership budget plus teardown of a
# cluster in an election storm. The +75 was calibrated against the observed
# 118 s for a 6-partition multi run at leader_wait 30, warmup 2, window 8.
run_cost_sec() {
    local parts="$1" group="$2"
    if [ "$group" = "multi" ] && [ "$parts" -gt 1 ]; then
        echo $(( LEADER_WAIT_SEC + WARMUP_SEC + DURATION_SEC + 75 ))
    else
        echo $(( DURATION_SEC + WARMUP_SEC + RUN_OVERHEAD_SEC ))
    fi
}

# Emits "<runs> <seconds>" for one phase, using exactly the loops the phase
# functions use. Keep these in step: a runtime estimate that disagrees with
# what actually runs is worse than no estimate.
plan_phase() {
    local phase="$1"
    local n=0 secs=0 entry parts group payload rate batch cost
    case "$phase" in
        rate)
            for entry in "${CONFIGS[@]}"; do
                parts="${entry%%:*}"; group="${entry##*:}"
                cost="$(run_cost_sec "$parts" "$group")"
                for payload in "${PAYLOAD_BYTES[@]}"; do
                    require_payload_maps "$payload"
                    for rate in ${RATES[$payload]}; do
                        n=$((n + 1)); secs=$((secs + cost))
                    done
                done
            done ;;
        payload)
            for entry in "${CONFIGS[@]}"; do
                parts="${entry%%:*}"; group="${entry##*:}"
                cost="$(run_cost_sec "$parts" "$group")"
                for payload in "${PAYLOAD_BYTES[@]}"; do
                    n=$((n + 1)); secs=$((secs + cost))
                done
            done ;;
        batch)
            for entry in "${CONFIGS[@]}"; do
                parts="${entry%%:*}"; group="${entry##*:}"
                cost="$(run_cost_sec "$parts" "$group")"
                for batch in "${BATCH_SIZES[@]}"; do
                    n=$((n + 1)); secs=$((secs + cost))
                done
            done ;;
        groups)
            local nrates=2
            $QUICK && nrates=1
            for group in "${GROUPS_MODES[@]}"; do
                cost="$(run_cost_sec "$GROUPS_PARTITIONS" "$group")"
                for ((rate = 0; rate < nrates; rate++)); do
                    n=$((n + 1)); secs=$((secs + cost))
                done
            done ;;
        knee)
            cost="$(run_cost_sec 1 single)"
            for payload in "${PAYLOAD_BYTES[@]}"; do
                n=$((n + 1)); secs=$((secs + cost))
            done ;;
    esac
    echo "$((n * TRIALS)) $((secs * TRIALS))"
}

print_estimate() {
    local phases=("$@")
    local total=0 total_secs=0 p n secs plan
    echo ""
    echo "Planned sweep:"
    for p in "${phases[@]}"; do
        plan="$(plan_phase "$p")"
        n="${plan%% *}"; secs="${plan##* }"
        total=$((total + n))
        total_secs=$((total_secs + secs))
        printf "  %-8s %4d runs  ~%dh%02dm\n" "$p" "$n" \
            $((secs / 3600)) $(((secs % 3600) / 60))
    done
    printf "  %-8s %4d runs  ~%dh%02dm\n" "TOTAL" "$total" \
        $((total_secs / 3600)) $(((total_secs % 3600) / 60))
    echo "  trials per point: $TRIALS   output: $OUTPUT_DIR"
    # Name the cost rather than burying it in the total.
    local slow=0 entry parts group
    for entry in "${CONFIGS[@]}"; do
        parts="${entry%%:*}"; group="${entry##*:}"
        if [ "$group" = "multi" ] && [ "$parts" -gt 1 ]; then slow=1; fi
    done
    for group in "${GROUPS_MODES[@]}"; do
        if [ "$group" = "multi" ] && [ "$GROUPS_PARTITIONS" -gt 1 ]; then slow=1; fi
    done
    if [ "$slow" -eq 1 ]; then
        echo "  NOTE: this plan includes per-partition group mode above one partition,"
        echo "        which does not elect a leader on this tree. Those runs will fail"
        echo "        after their full leadership budget; the estimate already charges"
        echo "        them at that rate. See docs/performance/raft-harness.md."
    fi
    echo ""
}

# ---------------------------------------------------------------------------
# Phase: summary
# ---------------------------------------------------------------------------
phase_summary() {
    log_step "Phase: summary"
    local sum="${OUTPUT_DIR}/SUMMARY.md"
    mkdir -p "$OUTPUT_DIR"

    # --phase summary regenerates the file for a directory this invocation did
    # not produce. In that case the in-memory counters are all zero and both
    # COMMIT_HASH and TIMESTAMP describe NOW, not the sweep — writing them
    # would silently falsify an archived run's provenance. So: reuse the
    # header lines of an existing SUMMARY.md when there is one, and derive the
    # run counts from what is on disk rather than from the counters.
    local header_commit="$COMMIT_HASH" header_started="$TIMESTAMP"
    local header_quick="$QUICK" header_trials="$TRIALS"
    local regenerating=false
    if [ "${#PHASES[@]}" -eq 0 ] && [ -f "$sum" ]; then
        regenerating=true
        header_commit="$(sed -n 's/^- Commit:  *`\(.*\)`$/\1/p' "$sum" | head -1)"
        header_started="$(sed -n 's/^- Started: *//p' "$sum" | head -1)"
        header_quick="$(sed -n 's/^- Quick: *//p' "$sum" | head -1)"
        header_trials="$(sed -n 's/^- Trials: *//p' "$sum" | head -1)"
        [ -z "$header_commit" ] && header_commit="unknown (regenerated)"
        [ -z "$header_started" ] && header_started="unknown (regenerated)"
        [ -z "$header_quick" ] && header_quick="unknown"
        [ -z "$header_trials" ] && header_trials="unknown"
    fi

    # Counts from disk: a record is a successful run, a .log with no matching
    # .json is a failed one. This is the same answer as the counters for a
    # sweep this invocation ran, and the only available answer when
    # regenerating.
    local disk_ok disk_logs disk_failed
    disk_ok=$(find "$OUTPUT_DIR" -name '*.json' -not -path '*/.*' 2>/dev/null | wc -l)
    disk_logs=$(find "$OUTPUT_DIR" -maxdepth 2 -name '*.log' 2>/dev/null | wc -l)
    disk_failed=$((disk_logs - disk_ok))
    [ "$disk_failed" -lt 0 ] && disk_failed=0

    {
        echo "# Raft performance sweep"
        echo ""
        echo "- Commit:  \`$header_commit\`"
        echo "- Started: $header_started"
        echo "- Output:  \`$OUTPUT_DIR\`"
        echo "- Quick:   $header_quick"
        echo "- Trials:  $header_trials"
        if $regenerating; then
            echo "- Runs:    $disk_ok ok, $disk_failed failed (counted from disk;"
            echo "           regenerated $(date '+%Y-%m-%d %H:%M:%S') by --phase summary)"
        else
            echo "- Runs:    $RUNS_OK ok, $RUNS_FAILED failed, $RUNS_PLANNED planned"
        fi
        echo ""
        if [ "$RUNS_FAILED" -gt 0 ]; then
            echo "## Failed runs"
            echo ""
            local f
            for f in "${FAILED_LABELS[@]}"; do
                echo "- \`$f\`"
            done
            echo ""
            echo "A failed run is a result, not a gap: check the matching .log."
            echo "The known one is group_mode=multi above one partition, which"
            echo "does not elect a leader on this tree."
            echo ""
        elif $regenerating && [ "$disk_failed" -gt 0 ]; then
            echo "## Failed runs"
            echo ""
            echo "$disk_failed run(s) produced a log but no record. This summary was"
            echo "regenerated after the fact, so the individual labels are no longer"
            echo "known; find them with:"
            echo ""
            echo '```'
            echo "for f in $OUTPUT_DIR/*/*.log; do [ -f \"\${f%.log}.json\" ] || echo \"\$f\"; done"
            echo '```'
            echo ""
        fi
        echo "## Records"
        echo ""
        echo "| phase | records |"
        echo "|---|---|"
        local p count
        for p in rate payload batch groups knee; do
            count=$(find "${OUTPUT_DIR}/$p" -name '*.json' 2>/dev/null | wc -l)
            [ "$count" -gt 0 ] && echo "| $p | $count |"
        done
        echo ""
        echo "## Next"
        echo ""
        echo '```'
        echo "python3 scripts/raft_perf/processing.py $OUTPUT_DIR/rate"
        echo "python3 scripts/raft_perf/lattput.py $OUTPUT_DIR/rate -o $OUTPUT_DIR/lattput.png"
        echo "python3 scripts/raft_perf/plot_latency_cdf.py $OUTPUT_DIR/rate -o $OUTPUT_DIR/cdf.png"
        echo '```'
    } > "$sum"
    log_info "Summary: $sum"
    cat "$sum"
}

# ---------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------
preflight

case "$PHASE" in
    all)     PHASES=(rate payload batch groups) ;;
    rate)    PHASES=(rate) ;;
    payload) PHASES=(payload) ;;
    batch)   PHASES=(batch) ;;
    groups)  PHASES=(groups) ;;
    knee)    PHASES=(knee) ;;
    summary) PHASES=() ;;
    *)       log_error "Unknown phase: $PHASE"; usage 2 ;;
esac

if [ "${#PHASES[@]}" -gt 0 ]; then
    print_estimate "${PHASES[@]}"
fi

log_info "Commit: $COMMIT_HASH"
log_info "Output: $OUTPUT_DIR"
if $DRY_RUN; then
    log_info "DRY RUN — nothing will be executed and no directory is created"
else
    mkdir -p "$OUTPUT_DIR"
fi

for p in "${PHASES[@]:-}"; do
    [ -z "$p" ] && continue
    case "$p" in
        rate)    phase_rate ;;
        payload) phase_payload ;;
        batch)   phase_batch ;;
        groups)  phase_groups ;;
        knee)    phase_knee ;;
    esac
done

if ! $DRY_RUN; then
    phase_summary
fi

if [ "$RUNS_FAILED" -gt 0 ]; then
    log_warn "$RUNS_FAILED of $RUNS_PLANNED runs failed; see SUMMARY.md"
    exit 1
fi
log_info "Done."
