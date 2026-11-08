#!/usr/bin/env bash
# Configure, build, and test the A toolchain.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-$ROOT_DIR/build}"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DA_WARNINGS_AS_ERRORS=ON \
  -DA_ENABLE_LTO=ON
cmake --build "$BUILD_DIR" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
ctest --test-dir "$BUILD_DIR" --output-on-failure
echo "A toolchain built at $BUILD_DIR/a"
