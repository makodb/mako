#!/usr/bin/env bash
#
# Reactor promotion battery driver — plan section 2.2, items 10 and 11, plus
# the orchestration of items 1-9 under their sanitizer configurations.
#
# This script does NOT decide promotion.  It runs the battery and reports; the
# verdict is written by a reviewer who is not the producer (plan section 4).
#
# It is authored ahead of a compiling provider on purpose ("author now, run
# later"): items 1-9 cannot build until compiler tuple V11 clears H2/H3/H4/H5
# and C6.  Run with --check to verify the wiring and inputs without needing
# any of that.
#
# Usage:
#   run_reactor_promotion_battery.sh --check
#   run_reactor_promotion_battery.sh --build-dir DIR [--stage all|runtime|asan|tsan|abi|incumbent]
#
set -uo pipefail

RED=$'\033[31m'; GRN=$'\033[32m'; YEL=$'\033[33m'; RST=$'\033[0m'
fail_count=0
pass_count=0
skip_count=0

note()  { printf '%s\n' "$*"; }
ok()    { printf '%s[PASS]%s %s\n' "$GRN" "$RST" "$*"; pass_count=$((pass_count+1)); }
bad()   { printf '%s[FAIL]%s %s\n' "$RED" "$RST" "$*"; fail_count=$((fail_count+1)); }
skip()  { printf '%s[SKIP]%s %s\n' "$YEL" "$RST" "$*"; skip_count=$((skip_count+1)); }

# --- frozen oracle inputs (read-only; never regenerate these) ---------------
INCUMBENT_MANIFEST=/var/tmp/reactor-incumbent-owned.unique.demangled
INCUMBENT_MANIFEST_SHA=e566039257c993ce43e9d96132ffc55d24300edbbd4bfd65c8b9104bc8d5be86
LAYOUT_PROBE=/var/tmp/srpc-reactor-audit.eLLZeD/repo/.reactor-audit/layout_probe.cpp


# Every battery test runs under the harness watchdog, but bound the whole
# process too: a wedged binary must never become another 15-hour silence.
PER_TEST_TIMEOUT=${PER_TEST_TIMEOUT:-120}

# --- item 9 + 1-8: the native battery ---------------------------------------
BATTERY_TESTS=(
    stackless_foreign_wake_completes_on_owner_tid              # 1
    stackless_wake_during_initial_poll                         # 2
    stackless_duplicate_and_concurrent_wake_coalescing         # 3
    stackless_completion_races_forced_slot_reuse               # 4
    stackless_reactor_destruction_races_retained_waker         # 5
    stackless_pollthread_shutdown_races_waker                  # 6
    stackless_direct_and_nonpollthread_reactors                # 7
    stackless_void_spawn_completes_on_owner_tid                # 7b (frozen ABI lane)
    stackless_wake_latency_bound                               # 8
    stackless_client_hang_regression                           # 9
)

# --- item 10: incumbent behavior regression ---------------------------------
# The checkpoint promotion list.  These are EXISTING behaviors: a green battery
# with any of these red is not a promotion, it is a regression with good race
# coverage.
INCUMBENT_TESTS=(
    test_reactor
    test_reactor_extended
    test_reactor_minimal
    test_timeout_race
    test_and_event
    fiber_test
    fiber_runtime
    rpc_pollthread_proxy_storage_test
)

usage() {
    sed -n '2,20p' "$0"
    exit 2
}

MODE=run
BUILD_DIR=
STAGE=all
while [ $# -gt 0 ]; do
    case "$1" in
        --check)      MODE=check ;;
        --build-dir)  BUILD_DIR=${2:-}; shift ;;
        --stage)      STAGE=${2:-all}; shift ;;
        -h|--help)    usage ;;
        *) note "unknown argument: $1"; usage ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# --check: validate inputs and wiring without a built provider.
# ---------------------------------------------------------------------------
if [ "$MODE" = check ]; then
    note "== battery wiring check =="

    here=$(cd "$(dirname "$0")/.." && pwd)

    if [ -f "$here/tests/reactor_stackless_battery.cc" ]; then
        for t in "${BATTERY_TESTS[@]}"; do
            if grep -q "$t" "$here/tests/reactor_stackless_battery.cc"; then
                ok "battery item present: $t"
            else
                bad "battery item MISSING from reactor_stackless_battery.cc: $t"
            fi
        done
    else
        bad "tests/reactor_stackless_battery.cc not found"
    fi

    if [ -f "$here/tests/reactor_watchdog.h" ]; then
        ok "watchdog harness present"
        n=$(grep -c 'SRPC_TEST_WATCHDOG("' "$here/tests/reactor_stackless_battery.cc" 2>/dev/null || echo 0)
        if [ "$n" -ge "${#BATTERY_TESTS[@]}" ]; then
            ok "every battery test opens a watchdog ($n sites)"
        else
            bad "only $n of ${#BATTERY_TESTS[@]} battery tests open a watchdog"
        fi
    else
        bad "tests/reactor_watchdog.h not found"
    fi

    if [ -r "$INCUMBENT_MANIFEST" ]; then
        actual=$(sha256sum "$INCUMBENT_MANIFEST" | cut -d' ' -f1)
        if [ "$actual" = "$INCUMBENT_MANIFEST_SHA" ]; then
            ok "incumbent owned manifest intact ($(wc -l < "$INCUMBENT_MANIFEST") entries)"
        else
            bad "incumbent manifest SHA MISMATCH — tampered or wrong file: $actual"
        fi
        janus=$(grep -c janus "$INCUMBENT_MANIFEST")
        if [ "$janus" -eq 49 ]; then
            ok "manifest carries the expected 49 janus lines (46 strong + 3 RTTI/vtable)"
        else
            bad "manifest janus line count is $janus, expected 49"
        fi
    else
        bad "incumbent manifest unreadable: $INCUMBENT_MANIFEST"
    fi

    if [ -r "$LAYOUT_PROBE" ]; then
        ok "29-row layout probe reachable"
    else
        bad "layout probe unreachable: $LAYOUT_PROBE"
    fi

    note ""
    note "checked: $pass_count pass, $fail_count fail, $skip_count skip"
    [ "$fail_count" -eq 0 ] || exit 1
    exit 0
fi

# ---------------------------------------------------------------------------
# run mode
# ---------------------------------------------------------------------------
if [ -z "$BUILD_DIR" ]; then
    note "--build-dir is required in run mode"
    usage
fi
if [ ! -d "$BUILD_DIR" ]; then
    note "build dir does not exist: $BUILD_DIR"
    exit 2
fi

run_gtest_binary() {
    # $1 binary, $2 optional gtest filter, $3 label
    local bin=$1 filter=$2 label=$3
    if [ ! -x "$bin" ]; then
        skip "$label (binary not built: $bin)"
        return
    fi
    local args=()
    [ -n "$filter" ] && args+=("--gtest_filter=$filter")
    if timeout --signal=ABRT "$PER_TEST_TIMEOUT" "$bin" "${args[@]}"; then
        ok "$label"
    else
        bad "$label (exit $?)"
    fi
}

# G6 — runtime, items 1-9
if [ "$STAGE" = all ] || [ "$STAGE" = runtime ]; then
    note "== G6 runtime: battery items 1-9 =="
    for t in "${BATTERY_TESTS[@]}"; do
        run_gtest_binary "$BUILD_DIR/reactor_stackless_battery" "*.$t" "G6 $t"
    done
fi

# G7 — ASan, items 5-7 plus the teardown-straggler assertion
if [ "$STAGE" = all ] || [ "$STAGE" = asan ]; then
    note "== G7 ASan: battery items 5-7 =="
    for t in stackless_reactor_destruction_races_retained_waker \
             stackless_pollthread_shutdown_races_waker \
             stackless_direct_and_nonpollthread_reactors; do
        run_gtest_binary "$BUILD_DIR/reactor_stackless_battery_asan" "*.$t" "G7 $t"
    done
fi

# G8 — TSan, items 1-9
if [ "$STAGE" = all ] || [ "$STAGE" = tsan ]; then
    note "== G8 TSan: battery items 1-9 =="
    for t in "${BATTERY_TESTS[@]}"; do
        run_gtest_binary "$BUILD_DIR/reactor_stackless_battery_tsan" "*.$t" "G8 $t"
    done
fi

# item 10 — incumbent behavior regression
if [ "$STAGE" = all ] || [ "$STAGE" = incumbent ]; then
    note "== item 10: incumbent behavior regression =="
    for t in "${INCUMBENT_TESTS[@]}"; do
        run_gtest_binary "$BUILD_DIR/$t" "" "incumbent $t"
    done
fi

# item 11 — ABI oracles (G3 symbols, G4 layout)
if [ "$STAGE" = all ] || [ "$STAGE" = abi ]; then
    note "== item 11: ABI oracles =="

    obj=$(find "$BUILD_DIR" -name 'srpc.reactor*.o' -print -quit 2>/dev/null)
    if [ -z "$obj" ]; then
        skip "G3 symbol oracle (no generated reactor object in $BUILD_DIR)"
        skip "G4 layout oracle (needs the same object)"
    else
        # G3: exact owned-strong-symbol compare.  Producing the normalized
        # owned manifest from the object is the gate runner's job; this driver
        # reports the comparison so the count and the diff are both visible.
        #
        # The compare is against the incumbent manifest PLUS 65 named,
        # reviewed additions.  It is still exact and bidirectional: every
        # addition is spelled out here and echoed on every run, so it is
        # owner-visible rather than absorbed, and ANY undeclared new symbol or
        # ANY missing symbol is still a failure.  The same declaration, with
        # the full reason, is REACTOR_INCUMBENT_ORACLE_ADDITIONS in
        # scripts/check_srpc_crate_mode.py.  Short version:
        #   * 1  EventState::new_ -- EventState has no field defaults, so it is
        #     built by the mandated `fn new()` factory, and the incumbent
        #     oracle has no EventState constructor symbol at all.
        #   * 64 the REVIEWED ADDITIVE ABI DELTA from removing the eleven
        #     `#[cfg_attr(any(), cpp_internal)]` markers in reactor/reactor.rs:
        #     54 EventPollable UFCS overloads (9 methods x 6 implementors) that
        #     lost their `inline`/vague linkage, 9 free helpers that lost
        #     internal linkage, and 1 module-attached const.  These
        #     port-internal helpers now export normally by owner decision; the
        #     incumbent object owned none of them.
        AUTHORIZED_ADDITIONS=(
            'srpc::EventState@srpc.reactor::new_()'
            'srpc::current_thread_gettid@srpc.reactor()'
            'srpc::EventPollable_::is_ready@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)'
            'srpc::EventPollable_::is_ready@srpc.reactor(srpc::IntEvent@srpc.reactor const&)'
            'srpc::EventPollable_::is_ready@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)'
            'srpc::EventPollable_::is_ready@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)'
            'srpc::EventPollable_::is_ready@srpc.reactor(srpc::WaitAll@srpc.reactor const&)'
            'srpc::EventPollable_::is_ready@srpc.reactor(srpc::WaitAny@srpc.reactor const&)'
            'srpc::EventPollable_::log@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)'
            'srpc::EventPollable_::log@srpc.reactor(srpc::IntEvent@srpc.reactor const&)'
            'srpc::EventPollable_::log@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)'
            'srpc::EventPollable_::log@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)'
            'srpc::EventPollable_::log@srpc.reactor(srpc::WaitAll@srpc.reactor const&)'
            'srpc::EventPollable_::log@srpc.reactor(srpc::WaitAny@srpc.reactor const&)'
            'srpc::EventPollable_::prunable@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)'
            'srpc::EventPollable_::prunable@srpc.reactor(srpc::IntEvent@srpc.reactor const&)'
            'srpc::EventPollable_::prunable@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)'
            'srpc::EventPollable_::prunable@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)'
            'srpc::EventPollable_::prunable@srpc.reactor(srpc::WaitAll@srpc.reactor const&)'
            'srpc::EventPollable_::prunable@srpc.reactor(srpc::WaitAny@srpc.reactor const&)'
            'srpc::EventPollable_::set_prunable@srpc.reactor(janus::QuorumEvent@srpc.reactor const&, bool)'
            'srpc::EventPollable_::set_prunable@srpc.reactor(srpc::IntEvent@srpc.reactor const&, bool)'
            'srpc::EventPollable_::set_prunable@srpc.reactor(srpc::NeverEvent@srpc.reactor const&, bool)'
            'srpc::EventPollable_::set_prunable@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&, bool)'
            'srpc::EventPollable_::set_prunable@srpc.reactor(srpc::WaitAll@srpc.reactor const&, bool)'
            'srpc::EventPollable_::set_prunable@srpc.reactor(srpc::WaitAny@srpc.reactor const&, bool)'
            'srpc::EventPollable_::set_status@srpc.reactor(janus::QuorumEvent@srpc.reactor const&, srpc::EventStatus@srpc.reactor)'
            'srpc::EventPollable_::set_status@srpc.reactor(srpc::IntEvent@srpc.reactor const&, srpc::EventStatus@srpc.reactor)'
            'srpc::EventPollable_::set_status@srpc.reactor(srpc::NeverEvent@srpc.reactor const&, srpc::EventStatus@srpc.reactor)'
            'srpc::EventPollable_::set_status@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&, srpc::EventStatus@srpc.reactor)'
            'srpc::EventPollable_::set_status@srpc.reactor(srpc::WaitAll@srpc.reactor const&, srpc::EventStatus@srpc.reactor)'
            'srpc::EventPollable_::set_status@srpc.reactor(srpc::WaitAny@srpc.reactor const&, srpc::EventStatus@srpc.reactor)'
            'srpc::EventPollable_::status@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)'
            'srpc::EventPollable_::status@srpc.reactor(srpc::IntEvent@srpc.reactor const&)'
            'srpc::EventPollable_::status@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)'
            'srpc::EventPollable_::status@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)'
            'srpc::EventPollable_::status@srpc.reactor(srpc::WaitAll@srpc.reactor const&)'
            'srpc::EventPollable_::status@srpc.reactor(srpc::WaitAny@srpc.reactor const&)'
            'srpc::EventPollable_::test@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)'
            'srpc::EventPollable_::test@srpc.reactor(srpc::IntEvent@srpc.reactor const&)'
            'srpc::EventPollable_::test@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)'
            'srpc::EventPollable_::test@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)'
            'srpc::EventPollable_::test@srpc.reactor(srpc::WaitAll@srpc.reactor const&)'
            'srpc::EventPollable_::test@srpc.reactor(srpc::WaitAny@srpc.reactor const&)'
            'srpc::EventPollable_::upgrade_fiber@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)'
            'srpc::EventPollable_::upgrade_fiber@srpc.reactor(srpc::IntEvent@srpc.reactor const&)'
            'srpc::EventPollable_::upgrade_fiber@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)'
            'srpc::EventPollable_::upgrade_fiber@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)'
            'srpc::EventPollable_::upgrade_fiber@srpc.reactor(srpc::WaitAll@srpc.reactor const&)'
            'srpc::EventPollable_::upgrade_fiber@srpc.reactor(srpc::WaitAny@srpc.reactor const&)'
            'srpc::EventPollable_::wakeup_time@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)'
            'srpc::EventPollable_::wakeup_time@srpc.reactor(srpc::IntEvent@srpc.reactor const&)'
            'srpc::EventPollable_::wakeup_time@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)'
            'srpc::EventPollable_::wakeup_time@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)'
            'srpc::EventPollable_::wakeup_time@srpc.reactor(srpc::WaitAll@srpc.reactor const&)'
            'srpc::EventPollable_::wakeup_time@srpc.reactor(srpc::WaitAny@srpc.reactor const&)'
            'srpc::reactor_log_line@srpc.reactor(int, int, signed char const*, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> >)'
            'srpc::reactor_verify@srpc.reactor(bool)'
            'srpc::reusing_fiber@srpc.reactor()'
            'srpc::stackless_profile_enabled@srpc.reactor()'
            'srpc::stackless_profile_env@srpc.reactor()'
            'srpc::stackless_profile_report_periodic@srpc.reactor()'
            'srpc::stackless_profile_update_max_slots@srpc.reactor(unsigned long)'
            'srpc::STACKLESS_UNREGISTERED_SLOT@srpc.reactor'
            'srpc::thread_id_to_u64@srpc.reactor(rusty::thread::ThreadId)'
        )
        actual_manifest=$(mktemp /var/tmp/reactor-battery-nm.XXXXXX)
        expected_manifest=$(mktemp /var/tmp/reactor-battery-expected.XXXXXX)
        # Symbol-class filter is [TDR], not [TDBRV].  The frozen manifest is
        # STRONG defined symbols only: it contains zero `guard variable for ...`,
        # zero DW.ref.* and zero `g_stackless_*` lines.  B (BSS) and V (weak
        # object) drag exactly those in -- measured on the real object, [TDBRV]
        # yields 372 entries against a 300-entry manifest, an 87-extra
        # "drift" that has nothing to do with the provider and that this driver
        # could therefore never report as clean.  Keep `nm -C`: the manifest was
        # demangled GNU-style (it has 15 `> >` lines), so llvm-nm --demangle,
        # which prints `>>`, is NOT a substitute here.
        nm -C --defined-only "$obj" 2>/dev/null \
            | awk '$2 ~ /^[TDR]$/ {sub(/^[^ ]+ [^ ]+ /,""); print}' \
            | sort -u > "$actual_manifest"
        { cat "$INCUMBENT_MANIFEST"; printf '%s\n' "${AUTHORIZED_ADDITIONS[@]}"; } \
            | sort -u > "$expected_manifest"
        for add in "${AUTHORIZED_ADDITIONS[@]}"; do
            note "     authorized addition over the incumbent oracle: $add"
        done
        if diff -u "$expected_manifest" "$actual_manifest" > /var/tmp/reactor-symbol-diff.txt 2>&1; then
            ok "G3 symbol oracle: exact match, $(wc -l < "$INCUMBENT_MANIFEST") incumbent + ${#AUTHORIZED_ADDITIONS[@]} authorized addition(s)"
        else
            bad "G3 symbol oracle: drift — see /var/tmp/reactor-symbol-diff.txt"
            note "     new symbols:  $(grep -c '^+[^+]' /var/tmp/reactor-symbol-diff.txt)"
            note "     missing:      $(grep -c '^-[^-]' /var/tmp/reactor-symbol-diff.txt)"
            note "     (beyond the 65 declared additions, C7 wake helpers and"
            note "      C10 UFCS surfaces must contribute ZERO)"
        fi
        rm -f "$expected_manifest"
        note "     janus entries seen: $(grep -c janus "$actual_manifest") (expect 58)"
        rm -f "$actual_manifest"

        skip "G4 layout oracle: rebuild $LAYOUT_PROBE against the generated provider"
    fi
fi

note ""
note "battery: $pass_count pass, $fail_count fail, $skip_count skip"
note "NOTE: this driver reports only. The verdict is written by a reviewer who"
note "      did not produce the tuple or run the gate (plan section 4)."
[ "$fail_count" -eq 0 ] || exit 1
exit 0
