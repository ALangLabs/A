#!/usr/bin/env bash
# Superfast build: Ninja + ccache + mold + Unity builds, LTO OFF by default.
# Usage: tooling/build/build.sh [build_dir] [--fast] [--lto] [--no-unity]
#   default: fastest build time (Ninja, ccache, mold, Unity ON, LTO OFF, -j=nproc)
#   --lto: enable LTO (faster binaries, slower builds)
#   --no-unity: disable Unity build
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
MODE="fast"; UNITY=0
for arg in "$@"; do
  case "$arg" in
    --fast) MODE="fast" ;;
    --lto) MODE="lto" ;;
    --unity) UNITY=1 ;;
    *) if [ ! "${arg}" = "--"* ]; then BUILD_DIR="$arg"; fi ;;
  esac
done

JOBS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
GEN_ARGS=(); command -v ninja >/dev/null 2>&1 && GEN_ARGS=(-G Ninja)
LAUNCHER_ARGS=(); command -v ccache >/dev/null 2>&1 && LAUNCHER_ARGS=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache)
LINK_ARGS=(); command -v mold >/dev/null 2>&1 && LINK_ARGS=(-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=mold -DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=mold)
LTO=OFF; [ "$MODE" = "lto" ] && LTO=ON
UNITY_ARGS=(); [ "$UNITY" = "1" ] && UNITY_ARGS=(-DCMAKE_UNITY_BUILD=ON -DCMAKE_UNITY_BUILD_MODE=GROUP)

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" "${GEN_ARGS[@]:-}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DA_WARNINGS_AS_ERRORS=ON \
  -DA_ENABLE_LTO="$LTO" \
  "${LAUNCHER_ARGS[@]:-}" "${LINK_ARGS[@]:-}" "${UNITY_ARGS[@]:-}"
cmake --build "$BUILD_DIR" -j "$JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure
echo "A toolchain built at $BUILD_DIR/a"
