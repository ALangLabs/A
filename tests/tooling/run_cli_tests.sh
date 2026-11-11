#!/usr/bin/env bash
# CLI coverage for `a fmt` and `a doc`.
#
# Usage: tests/tooling/run_cli_tests.sh [path-to-a-binary]
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
A_BIN="${1:-$ROOT_DIR/build/a}"
if [[ "$A_BIN" != /* ]]; then
  A_BIN="$(cd "$(dirname "$A_BIN")" 2>/dev/null && pwd)/$(basename "$A_BIN")"
fi
if [[ ! -x "$A_BIN" ]]; then
  echo "error: A binary not found at '$A_BIN'" >&2
  exit 1
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

SOURCE="$WORK_DIR/sample.a"
cat >"$SOURCE" <<'EOF'
"""Adds two numbers."""
fn add( a:i32,b:i32 )->i32:
  return a+b

# A note.
value=0xFF # trailing
EOF

EXPECTED="$WORK_DIR/expected.a"
cat >"$EXPECTED" <<'EOF'
"""Adds two numbers."""
fn add(a: i32, b: i32) -> i32:
    return a + b

# A note.
value = 0xFF  # trailing
EOF

"$A_BIN" fmt "$SOURCE" >"$WORK_DIR/formatted.a"
diff -u "$EXPECTED" "$WORK_DIR/formatted.a"

if "$A_BIN" fmt --check "$SOURCE" >/dev/null 2>"$WORK_DIR/check.err"; then
  echo "error: --check accepted an unformatted file" >&2
  exit 1
fi
if ! grep -q "needs formatting" "$WORK_DIR/check.err"; then
  echo "error: --check did not explain the failure" >&2
  exit 1
fi

"$A_BIN" fmt -w "$SOURCE"
"$A_BIN" fmt --check "$SOURCE"
diff -u "$EXPECTED" "$SOURCE"

BROKEN="$WORK_DIR/broken.a"
printf 'fn broken(:\n' >"$BROKEN"
cp "$BROKEN" "$WORK_DIR/broken.original"
if "$A_BIN" fmt -w "$BROKEN" >/dev/null 2>&1; then
  echo "error: fmt accepted a file with a syntax error" >&2
  exit 1
fi
cmp "$BROKEN" "$WORK_DIR/broken.original"

"$A_BIN" doc "$SOURCE" >"$WORK_DIR/sample.md"
grep -q "Adds two numbers." "$WORK_DIR/sample.md"
grep -q 'fn add(a: i32, b: i32) -> i32' "$WORK_DIR/sample.md"

"$A_BIN" doc --json -o "$WORK_DIR/sample.json" "$SOURCE"
python3 - "$WORK_DIR/sample.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
assert data["module"] == "sample", data
assert any(item["name"] == "add" for item in data["items"]), data
PY

WARNABLE="$WORK_DIR/warn.a"
cat >"$WARNABLE" <<'EOF'
fn f() -> i32:
    unused = 1
    return 0

print(f())
EOF
"$A_BIN" check --json "$WARNABLE" >"$WORK_DIR/check.json"
python3 - "$WORK_DIR/check.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
codes = [item["code"] for item in data["diagnostics"]]
assert "W0001" in codes, data
assert all(item["severity"] == "warning" for item in data["diagnostics"]), data
PY

TESTS="$WORK_DIR/tests.a"
cat >"$TESTS" <<'EOF'
fn test_ok() -> void:
    assert_eq(1 + 1, 2)

fn test_bad() -> void:
    assert_eq(1, 2, "one is not two")
EOF
if "$A_BIN" test "$TESTS" >"$WORK_DIR/test.out" 2>"$WORK_DIR/test.err"; then
  echo "error: 'a test' succeeded despite a failing test" >&2
  exit 1
fi
grep -q "FAILED test_bad:" "$WORK_DIR/test.out"
grep -q "ok test_ok" "$WORK_DIR/test.out"
grep -q "1 passed, 1 failed" "$WORK_DIR/test.out"

# Async/parallel checking: --parallel must agree with serial on diagnostics,
# exit code, and program output.
PARALLEL="$WORK_DIR/parallel.a"
cat >"$PARALLEL" <<'EOF'
fn add(a: i32, b: i32) -> i32:
    return a + b

print(add(2, 3))
EOF
"$A_BIN" check "$PARALLEL"
"$A_BIN" check --parallel "$PARALLEL"
"$A_BIN" check --jobs=4 "$PARALLEL"
"$A_BIN" check -j4 "$PARALLEL"
"$A_BIN" run --parallel "$PARALLEL" >"$WORK_DIR/parallel.out"
grep -q "^5$" "$WORK_DIR/parallel.out"

BROKEN2="$WORK_DIR/broken2.a"
printf 'x = undefined_name\n' >"$BROKEN2"
if "$A_BIN" check --parallel "$BROKEN2" >/dev/null 2>"$WORK_DIR/serial.err"; then
  echo "error: parallel check accepted undefined name" >&2
  exit 1
fi
if "$A_BIN" check "$BROKEN2" >/dev/null 2>&1; then
  echo "error: serial check accepted undefined name" >&2
  exit 1
fi
grep -q "E0001" "$WORK_DIR/serial.err"

# Multi-module programs exercise analyzeParallel's worker pool; serial and
# parallel runs must agree.
mkdir -p "$WORK_DIR/multi/lib"
cat >"$WORK_DIR/multi/lib/mathx.a" <<'EOF'
fn add(a, b):
    return a + b
EOF
cat >"$WORK_DIR/multi/main.a" <<'EOF'
import "lib/mathx.a"
print(mathx.add(2, 3))
EOF
"$A_BIN" check "$WORK_DIR/multi/main.a"
"$A_BIN" check --parallel "$WORK_DIR/multi/main.a"
"$A_BIN" check -j4 "$WORK_DIR/multi/main.a"
"$A_BIN" run --parallel "$WORK_DIR/multi/main.a" >"$WORK_DIR/multi.out"
grep -q "^5$" "$WORK_DIR/multi.out"

# Directory / codebase scan: `a test <dir>` and `a test` (no args) discover
# `*.a` files containing `fn test_*()` and aggregate results.
SCAN="$WORK_DIR/scan"
mkdir -p "$SCAN/sub"
cat >"$SCAN/one_test.a" <<'EOF'
fn test_one() -> void:
    assert_eq(1, 1)
EOF
cat >"$SCAN/sub/two_test.a" <<'EOF'
fn test_two() -> void:
    assert_eq(2, 2)

fn test_two_b() -> void:
    assert_eq(3, 3)
EOF
cat >"$SCAN/not_a_test.a" <<'EOF'
fn helper() -> i32:
    return 1
EOF
"$A_BIN" test "$SCAN" >"$WORK_DIR/scan.out" 2>"$WORK_DIR/scan.err"
grep -q "== one_test.a ==" "$WORK_DIR/scan.out"
grep -q "== sub/two_test.a ==" "$WORK_DIR/scan.out"
grep -q "ok test_one" "$WORK_DIR/scan.out"
grep -q "ok test_two_b" "$WORK_DIR/scan.out"
grep -q "3 passed, 0 failed in 2 file(s)" "$WORK_DIR/scan.out"
if grep -q "not_a_test" "$WORK_DIR/scan.out" "$WORK_DIR/scan.err"; then
  echo "error: scan ran a file without fn test_*()" >&2
  exit 1
fi

# no-arg scan from cwd
(cd "$SCAN" && "$A_BIN" test >"$WORK_DIR/scan_cwd.out" 2>"$WORK_DIR/scan_cwd.err")
grep -q "3 passed, 0 failed in 2 file(s)" "$WORK_DIR/scan_cwd.out"

# --filter across scanned files (files with no matching tests are skipped)
"$A_BIN" test "$SCAN" --filter two_b >"$WORK_DIR/scan_filter.out" 2>"$WORK_DIR/scan_filter.err"
grep -q "1 passed, 0 failed in 1 file(s)" "$WORK_DIR/scan_filter.out"
grep -q "skipped by filter" "$WORK_DIR/scan_filter.out"

if "$A_BIN" test "$SCAN" --filter nonexistent >"$WORK_DIR/scan_nomatch.out" 2>"$WORK_DIR/scan_nomatch.err"; then
  echo "error: filtered scan with no matches should fail" >&2
  exit 1
fi
grep -q "no tests matched --filter 'nonexistent'" "$WORK_DIR/scan_nomatch.err"

EMPTYSCAN="$WORK_DIR/emptyscan"
mkdir -p "$EMPTYSCAN"
if "$A_BIN" test "$EMPTYSCAN" >"$WORK_DIR/scan_empty.out" 2>"$WORK_DIR/scan_empty.err"; then
  echo "error: scan with no test files should fail" >&2
  exit 1
fi
grep -q "no test files found" "$WORK_DIR/scan_empty.err"

echo "OK: fmt, doc, check (incl. parallel/async), and test CLI (incl. scan)"
