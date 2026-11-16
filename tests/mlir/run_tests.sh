#!/usr/bin/env bash
# Golden tests for `a mlir-opt` (tests/mlir/cases/).
#
# Layout (all files flat in cases/):
#   <name>.mlir | <name>.ambc   one case per file; .mlir is MLIR text input,
#                             .ambc is AMBC bytecode input
#   <name>.args                extra CLI args for `a mlir-opt` (whitespace
#                             separated; e.g. "cse" or "--emit-llvmir")
#   <name>.out                 expected stdout (exact diff)
#   <name>.err                 expected stderr substrings (one per line,
#                             grep -F style like tests/integration)
#   <name>.exit                expected numeric exit code (default 0)
#   <name>.skip                skip this case; first line is the reason
#   <name>.normalize.sed       sed -E script applied to stdout before diff
#                             (for stripping loc(...)/; ModuleID noise)
#
# Usage: tests/mlir/run_tests.sh [path-to-a-binary]
#   Binary resolution: $1, else $A_MLIR_OPT, else <repo>/build/a.
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CASES_DIR="$SCRIPT_DIR/cases"
A_BIN="${1:-${A_MLIR_OPT:-$ROOT_DIR/build/a}}"
if [[ "$A_BIN" != /* ]]; then
  A_BIN="$(cd "$(dirname "$A_BIN")" 2>/dev/null && pwd)/$(basename "$A_BIN")"
fi
if [[ ! -x "$A_BIN" ]]; then
  echo "error: a binary not found at '$A_BIN'" >&2
  echo "build the compiler first (tooling/build/build.sh or cmake --build build)" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

passed=0
failed=0
skipped=0

run_case() {
  local source="$1"
  local base name stdout_file stderr_file expected expected_err exit_file
  local expected_exit actual_exit rc args sed_script

  base="$(basename "$source")"
  base="${base%.*}"
  name="$base"
  stdout_file="$TMP_DIR/$base.stdout"
  stderr_file="$TMP_DIR/$base.stderr"
  expected="$CASES_DIR/$base.out"
  expected_err="$CASES_DIR/$base.err"
  exit_file="$CASES_DIR/$base.exit"

  local skip_file="$CASES_DIR/$base.skip"
  if [[ -f "$skip_file" ]]; then
    skipped=$((skipped + 1))
    echo "SKIP $name ($(head -n1 "$skip_file"))"
    return
  fi

  args=""
  local args_file="$CASES_DIR/$base.args"
  if [[ -f "$args_file" ]]; then
    args="$(tr -d '\r\n' < "$args_file")"
  fi

  sed_script=""
  local sed_file="$CASES_DIR/$base.normalize.sed"
  if [[ -f "$sed_file" ]]; then
    sed_script="$(cat "$sed_file")"
  fi

  expected_exit=0
  if [[ -f "$exit_file" ]]; then
    expected_exit="$(tr -d '[:space:]' < "$exit_file")"
  fi

  rc=0
  # shellcheck disable=SC2086
  "$A_BIN" mlir-opt "$source" $args \
    >"$stdout_file" 2>"$stderr_file" || rc=$?

  if [[ -n "$sed_script" ]]; then
    sed -E -i "$sed_script" "$stdout_file"
  fi

  local ok=1
  if [[ "$rc" -ne "$expected_exit" ]]; then
    echo "FAIL $name"
    echo "  - exit code: expected $expected_exit, got $rc"
    ok=0
  fi
  if [[ -f "$expected" ]]; then
    if ! diff -u "$expected" "$stdout_file" >/dev/null; then
      [[ "$ok" -eq 1 ]] && echo "FAIL $name"
      echo "  - stdout mismatch"
      diff -u "$expected" "$stdout_file" | sed 's/^/  /' || true
      ok=0
    fi
  fi
  if [[ -f "$expected_err" ]]; then
    while IFS= read -r line || [[ -n "$line" ]]; do
      [[ -z "$line" ]] && continue
      if ! grep -Fq -- "$line" "$stderr_file"; then
        [[ "$ok" -eq 1 ]] && echo "FAIL $name"
        echo "  - missing expected stderr: $line"
        ok=0
      fi
    done < "$expected_err"
  fi

  if [[ "$ok" -eq 1 ]]; then
    passed=$((passed + 1))
  else
    if [[ -s "$stderr_file" ]]; then
      echo "  stderr|"
      sed 's/^/  /' "$stderr_file"
    fi
    failed=$((failed + 1))
  fi
}

shopt -s nullglob
for source in "$CASES_DIR"/*.mlir "$CASES_DIR"/*.ambc; do
  run_case "$source"
done

echo
echo "passed: $passed, failed: $failed, skipped: $skipped"
if [[ "$failed" -ne 0 ]]; then
  exit 1
fi
