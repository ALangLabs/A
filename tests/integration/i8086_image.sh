#!/usr/bin/env bash
# Build an A program for the bare-metal i8086 target and run the boot image
# under QEMU. Serial (COM1) output is captured on stdout; the guest exits via
# isa-debug-exit port 0xf4, which QEMU reports as exit status (value<<1)|1
# (the i8086 _entry writes 42 -> status 85).
#
# Usage: tests/integration/i8086_image.sh <path-to-a> <source.a> [expected.out]
set -u

A_BIN="$1"; SRC="$2"; EXPECT="${3:-}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ "$A_BIN" != /* ]]; then A_BIN="$ROOT/$A_BIN"; fi
[[ -x "$A_BIN" ]] || { echo "error: A binary not found: $A_BIN" >&2; exit 2; }
command -v qemu-system-i386 >/dev/null 2>&1 || { echo "error: qemu-system-i386 not found" >&2; exit 2; }
[[ -n "${A_STDLIB:-}" ]] || export A_STDLIB="$ROOT/stdlib"

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
IMG="$TMP/program.img"
OUT="$TMP/serial.out"; ERR="$TMP/qemu.err"

if ! "$A_BIN" build --target i8086 "$SRC" -o "$IMG" >"$TMP/build.out" 2>"$TMP/build.err"; then
  cat "$TMP/build.err" >&2
  exit 1
fi

rc=0
timeout 15 qemu-system-i386 \
  -drive format=raw,file="$IMG" \
  -serial stdio \
  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
  -display none -no-reboot \
  < /dev/null >"$OUT" 2>"$ERR" || rc=$?

cat "$OUT"
if [[ -s "$ERR" ]]; then cat "$ERR" >&2; fi

ok=0
[[ "$rc" -eq 85 ]] && ok=1
if [[ -n "$EXPECT" ]]; then
  exp="$(cat "$EXPECT")"
  act="$(cat "$OUT")"
  [[ "$act" == "$exp" ]] || ok=0
fi
if [[ "$ok" -eq 1 ]]; then exit 0; fi
echo "i8086 harness failed: qemu rc=$rc (expected 85)" >&2
exit 1
