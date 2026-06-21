#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TARGET_DIR="${ROOT_DIR}/third-party/redis/redis-tests"
TEST_HELPER="${TARGET_DIR}/tests/test_helper.tcl"
REDIS_TAG="${REDIS_TESTS_TAG:-7.4.0}"

if [[ -f "${TEST_HELPER}" ]]; then
    echo "redis TCL tests already present: ${TEST_HELPER}"
    exit 0
fi

if [[ -n "${REDIS_TESTS_SOURCE:-}" ]]; then
    SOURCE_TESTS="${REDIS_TESTS_SOURCE%/}"
    if [[ -f "${SOURCE_TESTS}/test_helper.tcl" ]]; then
        mkdir -p "${TARGET_DIR}"
        rm -f "${TARGET_DIR}/tests"
        ln -s "${SOURCE_TESTS}" "${TARGET_DIR}/tests"
        echo "linked Redis TCL tests from ${SOURCE_TESTS}"
        exit 0
    fi
    echo "REDIS_TESTS_SOURCE must point at a Redis tests/ directory"
    exit 78
fi

if [[ "${REDIS_COMPAT_FETCH_REDIS_TESTS:-0}" != "1" ]]; then
    echo "missing third-party/redis/redis-tests/tests/test_helper.tcl"
    echo "set REDIS_TESTS_SOURCE=/path/to/redis/tests or REDIS_COMPAT_FETCH_REDIS_TESTS=1"
    exit 78
fi

if ! command -v git >/dev/null 2>&1; then
    echo "missing git"
    exit 78
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

git clone --depth 1 --filter=blob:none --sparse --branch "${REDIS_TAG}" \
    https://github.com/redis/redis.git "${TMP_DIR}/redis"
git -C "${TMP_DIR}/redis" sparse-checkout set tests

mkdir -p "${TARGET_DIR}"
rm -rf "${TARGET_DIR}/tests"
cp -R "${TMP_DIR}/redis/tests" "${TARGET_DIR}/tests"
echo "bootstrapped Redis TCL tests tag=${REDIS_TAG} into ${TARGET_DIR}/tests"
