#!/usr/bin/env bash
# Run-cache self-test for `a run`: a repeat run of unchanged source must
# reuse the cached binary (same output, no new cache entry) and any source
# edit must invalidate it. Compile-time diagnostics are replayed on hits.
# A_NO_CACHE=1 must bypass the cache entirely.
#
# Usage: tests/integration/cache_test.sh [path-to-a-binary]
set -euo pipefail

A_BIN="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/build/a}"
if [[ ! -x "$A_BIN" ]]; then
  echo "error: A binary not found at '$A_BIN'" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
CACHE_DIR="$TMP_DIR/cache"
export A_CACHE_DIR="$CACHE_DIR"

SRC1="$TMP_DIR/prog.a"
cat > "$SRC1" <<'EOF'
entry main():
    print(1 + 2)
EOF

run() { "$A_BIN" run "$SRC1"; }
bins() { find "$CACHE_DIR" -type f ! -name '*.diag' | wc -l | tr -d ' '; }

# Cold run: correct output, exactly one cache entry (plus its .diag sidecar).
[[ "$(run)" == "3" ]] || { echo "FAIL: cold run output" >&2; exit 1; }
[ -d "$CACHE_DIR" ] || { echo "FAIL: cache dir not created" >&2; exit 1; }
[ "$(bins)" = "1" ] || { echo "FAIL: expected 1 cache entry, got $(bins)" >&2; exit 1; }

# Hot run: same output, no new entry (binary byte-reused).
[[ "$(run)" == "3" ]] || { echo "FAIL: hot run output" >&2; exit 1; }
[ "$(bins)" = "1" ] || { echo "FAIL: hot run wrote a new entry ($(bins))" >&2; exit 1; }

# Edit invalidates: new output, new entry.
cat > "$SRC1" <<'EOF'
entry main():
    print(4 + 5)
EOF
[[ "$(run)" == "9" ]] || { echo "FAIL: edited run did not invalidate" >&2; exit 1; }
[ "$(bins)" = "2" ] || { echo "FAIL: edit did not write a new entry ($(bins))" >&2; exit 1; }

# A_NO_CACHE: bypasses (correct output, no new entries).
[[ "$(A_NO_CACHE=1 run)" == "9" ]] || { echo "FAIL: no-cache run output" >&2; exit 1; }
[ "$(bins)" = "2" ] || { echo "FAIL: A_NO_CACHE wrote to the cache ($(bins))" >&2; exit 1; }

# ---- per-module object cache (Phase 3) ----
# Runs only when per-module units are enabled (default); with A_SPLIT_UNITS=0
# the single-unit path is the fallback and emit-count assertions do not apply.
if [[ "${A_SPLIT_UNITS:-1}" != "0" && "${A_SPLIT_UNITS:-1}" != "off" && "${A_SPLIT_UNITS:-1}" != "no" ]]; then
  OBJ_DIR="$(dirname "$CACHE_DIR")/obj"
  PROJ_DIR="$TMP_DIR/proj"
  mkdir -p "$PROJ_DIR/lib"
  ENTRY="$PROJ_DIR/main.a"
  HELPER="$PROJ_DIR/lib/helper.a"
  cat > "$HELPER" <<'EOF'
fn add(a, b):
    return a + b
EOF
  cat > "$ENTRY" <<'EOF'
import "lib/helper.a"
entry main():
    print(helper.add(1, 2))
EOF
  # Force a cold object cache so the "3 emitted" count is exact (the prelude
  # unit from the earlier single-module checks would otherwise be a hit).
  rm -rf "$OBJ_DIR"
  # (a) cold: every unit emitted (prelude + helper + entry).
  out="$(A_DEBUG_UNITS=1 "$A_BIN" run "$ENTRY" 2>"$TMP_DIR/dbg1" || true)"
  [[ "$out" == "3" ]] || { echo "FAIL: unit cold output ($out)" >&2; cat "$TMP_DIR/dbg1" >&2; exit 1; }
  grep -q "3 emitted" "$TMP_DIR/dbg1" || { echo "FAIL: cold should emit 3 units" >&2; cat "$TMP_DIR/dbg1" >&2; exit 1; }
  [ -d "$OBJ_DIR" ] || { echo "FAIL: obj cache dir not created ($OBJ_DIR)" >&2; exit 1; }
  [ "$(find "$OBJ_DIR" -name '*.o' | wc -l | tr -d ' ')" = "3" ] || { echo "FAIL: expected 3 objects" >&2; ls "$OBJ_DIR" >&2; exit 1; }
  # Body-only edit in the leaf: exactly one unit re-emitted.
  cat > "$HELPER" <<'EOF'
fn add(a, b):
    return a + b + 10
EOF
  out="$(A_DEBUG_UNITS=1 "$A_BIN" run "$ENTRY" 2>"$TMP_DIR/dbg2" || true)"
  [[ "$out" == "13" ]] || { echo "FAIL: body-edit output ($out)" >&2; cat "$TMP_DIR/dbg2" >&2; exit 1; }
  grep -q "1 emitted" "$TMP_DIR/dbg2" || { echo "FAIL: body edit should emit 1 unit" >&2; cat "$TMP_DIR/dbg2" >&2; exit 1; }
  # (b) Interface edit: adding a parameter changes helper's sighash; the
  # entry (dependent) must re-emit as well.
  cat > "$HELPER" <<'EOF'
fn add(a, b, c):
    return a + b + c
EOF
  cat > "$ENTRY" <<'EOF'
import "lib/helper.a"
entry main():
    print(helper.add(1, 2, 3))
EOF
  out="$(A_DEBUG_UNITS=1 "$A_BIN" run "$ENTRY" 2>"$TMP_DIR/dbg3" || true)"
  [[ "$out" == "6" ]] || { echo "FAIL: interface-edit output ($out)" >&2; cat "$TMP_DIR/dbg3" >&2; exit 1; }
  grep -q "2 emitted" "$TMP_DIR/dbg3" || { echo "FAIL: interface edit should emit 2 units" >&2; cat "$TMP_DIR/dbg3" >&2; exit 1; }
  # (c) Shadowing file invalidation: a bare import resolved via -I; creating
  # a file in an earlier search dir that shadows the resolved module must
  # change the result (precheck absent markers + per-unit path in the key).
  SHADOW_A="$TMP_DIR/shadow_a"
  SHADOW_B="$TMP_DIR/shadow_b"
  mkdir -p "$SHADOW_A" "$SHADOW_B"
  cat > "$SHADOW_B/helper.a" <<'EOF'
fn who():
    return 1
EOF
  BARE_ENTRY="$TMP_DIR/bare_main.a"
  cat > "$BARE_ENTRY" <<'EOF'
import helper
entry main():
    print(helper.who())
EOF
  out="$("$A_BIN" run -I "$SHADOW_B" "$BARE_ENTRY" 2>/dev/null || true)"
  [[ "$out" == "1" ]] || { echo "FAIL: bare cold output ($out)" >&2; exit 1; }
  # Second run (binary cached) still 1.
  out="$("$A_BIN" run -I "$SHADOW_B" "$BARE_ENTRY" 2>/dev/null || true)"
  [[ "$out" == "1" ]] || { echo "FAIL: bare hot output ($out)" >&2; exit 1; }
  cat > "$SHADOW_A/helper.a" <<'EOF'
fn who():
    return 2
EOF
  # Shadow dir listed first on the command line is searched first.
  out="$("$A_BIN" run -I "$SHADOW_A" -I "$SHADOW_B" "$BARE_ENTRY" 2>/dev/null || true)"
  [[ "$out" == "2" ]] || { echo "FAIL: shadow did not invalidate ($out)" >&2; exit 1; }
  # (d) Cache-off parity: A_NO_CACHE=1 stdout byte-equal to cached runs.
  cached="$(A_DEBUG_UNITS=1 "$A_BIN" run "$ENTRY" 2>"$TMP_DIR/dbg4" || true)"
  nocache="$(A_NO_CACHE=1 "$A_BIN" run "$ENTRY" 2>/dev/null || true)"
  [[ "$cached" == "$nocache" && -n "$cached" ]] || { echo "FAIL: cache-off parity ($cached vs $nocache)" >&2; cat "$TMP_DIR/dbg4" >&2; exit 1; }
  split0="$(A_SPLIT_UNITS=0 "$A_BIN" run "$ENTRY" 2>/dev/null || true)"
  [[ "$cached" == "$split0" ]] || { echo "FAIL: split-off parity ($cached vs $split0)" >&2; exit 1; }
fi

echo "cache test passed"
