#!/usr/bin/env bash
# Build a release package: binary + stdlib + docs.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-release}"
DIST_DIR="${DIST_DIR:-$ROOT_DIR/dist}"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DA_WARNINGS_AS_ERRORS=ON \
  -DA_ENABLE_LTO=ON
cmake --build "$BUILD_DIR" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
ctest --test-dir "$BUILD_DIR" --output-on-failure

platform="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"

"$ROOT_DIR/tooling/release/package.sh" "$BUILD_DIR" "$DIST_DIR" "$platform"
echo "release package: $DIST_DIR/a-$platform.tar.gz"
