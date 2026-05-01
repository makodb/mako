#!/bin/bash
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR_NAME="${1:-build_fast}"
shift || true

cmake -S "$REPO" -B "$REPO/$BUILD_DIR_NAME" -G Ninja \
  -DENABLE_BORROW_CHECKING=ON \
  -DDBTEST_REQUIRE_DEPTRAN_BORROW_CHECK=OFF \
  "$@"

echo
echo "Configured fast build: $BUILD_DIR_NAME"
echo "Build with: cmake --build $BUILD_DIR_NAME -j"
