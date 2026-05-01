#!/bin/bash
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR_NAME="${1:-build_checked}"
shift || true

cmake -S "$REPO" -B "$REPO/$BUILD_DIR_NAME" -G Ninja \
  -DENABLE_BORROW_CHECKING=ON \
  -DDBTEST_REQUIRE_DEPTRAN_BORROW_CHECK=ON \
  "$@"

echo
echo "Configured checked build: $BUILD_DIR_NAME"
echo "Fast target:    cmake --build $BUILD_DIR_NAME --target dbtest -j"
echo "Checked target: cmake --build $BUILD_DIR_NAME --target dbtest_checked -j"
