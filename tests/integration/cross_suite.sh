#!/usr/bin/env bash
# Cross-target golden tests: build every runnable integration case for a
# foreign arch and diff program stdout against the .out fixtures.
#
# Usage: tests/integration/cross_suite.sh <target> <sysroot-env> <sysroot> [runner]
#   <target>      arch passed to `a build --target` (i386, arm, aarch64, riscv64)
#   <sysroot-env> env var name the pipeline reads, e.g. A_SYSROOT_ARM64
#   <sysroot>     path to the target sysroot root
#   [runner]      optional wrapper that executes the foreign binary, e.g.
#                 /tmp/run-a64 (qemu-user-static with QEMU_LD_PREFIX set);
#                 omit for arches the host can execute natively (i386).
#
# Requires `a` built and a target sysroot (Debian *-cross packages work;
# their libc.so/libm.so linker scripts need GROUP paths made relative).
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
A="$ROOT/build/a"
CASES="$SCRIPT_DIR/cases"

ARCH="$1"; SYSENV="$2"; SYSROOT="$3"; RUNNER="${4:-}"
export A_STDLIB="$ROOT/stdlib"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0; failed=""

for src in $(find "$CASES" -name '*.a' | sort); do
  base="${src%.a}"; name="${base#$CASES/}"
  # lib/ fixtures are imports, not cases; .flags/.err cases aren't plain runs
  [[ "$name" == */lib/* ]] && continue
  [[ -f "$base.flags" || -f "$base.err" ]] && continue
  [[ ! -f "$base.out" ]] && continue

  bin="$TMP/$(echo "$name" | tr '/' '_')"
  if ! env "$SYSENV=$SYSROOT" "$A" build "$src" --target "$ARCH" -o "$bin" >/dev/null 2>&1; then
    fail=$((fail+1)); failed="$failed $name(build)"; continue
  fi
  IN=/dev/null; [[ -f "$base.in" ]] && IN="$base.in"
  if [[ -n "$RUNNER" ]]; then out=$("$RUNNER" "$bin" < "$IN" 2>/dev/null)
  else out=$("$bin" < "$IN" 2>/dev/null); fi
  exp="$(cat "$base.out")"
  if [[ "$out" == "$exp" ]]; then pass=$((pass+1))
  else fail=$((fail+1)); failed="$failed $name"; fi
done

echo "ARCH=$ARCH pass=$pass fail=$fail"
if [[ -n "$failed" ]]; then
  echo "FAILED:$failed" | tr ' ' '\n' | grep -v '^$' | head -40
fi
