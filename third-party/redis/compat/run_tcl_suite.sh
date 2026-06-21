#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TEST_HELPER="${ROOT_DIR}/third-party/redis/redis-tests/tests/test_helper.tcl"
TEST_ROOT="${ROOT_DIR}/third-party/redis/redis-tests"
SKIP_FILE="${TCL_COMPAT_SKIPFILE:-${ROOT_DIR}/third-party/redis/compat/tcl_known_skips.txt}"
HOST="${MAKO_HOST:-127.0.0.1}"
PORT="${MAKO_PORT:-6380}"
TIMEOUT_SECONDS="${TCL_COMPAT_FILE_TIMEOUT:-60}"
TAGS="${TCL_COMPAT_TAGS:--slow -needs:debug -needs:repl}"

if ! command -v tclsh >/dev/null 2>&1; then
    echo "missing tclsh"
    exit 78
fi

if [[ ! -f "${TEST_HELPER}" && "${REDIS_COMPAT_BOOTSTRAP_TCL:-0}" == "1" ]]; then
    bash "${ROOT_DIR}/third-party/redis/compat/bootstrap_redis_tests.sh" || exit $?
fi

if [[ ! -f "${TEST_HELPER}" ]]; then
    echo "missing third-party/redis/redis-tests/tests/test_helper.tcl"
    exit 78
fi

FILES="${TCL_COMPAT_FILES:-unit/type/string unit/type/hash unit/type/list unit/type/set unit/type/zset unit/expire unit/scan unit/multi unit/keyspace unit/networking unit/pubsub}"
passed=0
failed=0

for file in ${FILES}; do
    args=(
        tests/test_helper.tcl
        --host "${HOST}"
        --port "${PORT}"
        --single "${file}"
        --singledb
        --ignore-encoding
        --ignore-digest
        --timeout "${TIMEOUT_SECONDS}"
    )
    if [[ -n "${TAGS}" ]]; then
        args+=(--tags "${TAGS}")
    fi
    if [[ -f "${SKIP_FILE}" ]]; then
        args+=(--skipfile "${SKIP_FILE}")
    fi
    if (cd "${TEST_ROOT}" && timeout "${TIMEOUT_SECONDS}s" tclsh "${args[@]}"); then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
    fi
done

if [[ "${failed}" -ne 0 ]]; then
    echo "tcl semantic guard failed files=${failed} passed=${passed}"
    exit 1
fi

echo "tcl semantic guard passed files=${passed}"
