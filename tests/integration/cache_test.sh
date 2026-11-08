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
fn main():
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
fn main():
    print(4 + 5)
EOF
[[ "$(run)" == "9" ]] || { echo "FAIL: edited run did not invalidate" >&2; exit 1; }
[ "$(bins)" = "2" ] || { echo "FAIL: edit did not write a new entry ($(bins))" >&2; exit 1; }

# A_NO_CACHE: bypasses (correct output, no new entries).
[[ "$(A_NO_CACHE=1 run)" == "9" ]] || { echo "FAIL: no-cache run output" >&2; exit 1; }
[ "$(bins)" = "2" ] || { echo "FAIL: A_NO_CACHE wrote to the cache ($(bins))" >&2; exit 1; }

echo "cache test passed"
