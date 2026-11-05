#!/usr/bin/env bash
# Golden tests for the A language compiler.
#
# Layout:
#   tests/integration/cases/**/*.a      one case per feature; may live in
#                                       subdirectories (e.g. errors/, stdlib/)
#   tests/integration/cases/**/lib/     imported fixtures; never run directly
#   <path>/<name>.out                   expected stdout for a `run` case
#   <path>/<name>.err                   expected stderr substrings (one per line)
#   <path>/<name>.exit                  expected numeric exit code
#   <path>/<name>.in                    stdin fed to the program
#   <path>/<name>.flags                 command/flags, e.g. "check" or "test"
#
# Usage: tests/integration/run_tests.sh [path-to-a-binary]
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CASES_DIR="$SCRIPT_DIR/cases"
A_BIN="${1:-$ROOT_DIR/build/a}"
if [[ "$A_BIN" != /* ]]; then
  A_BIN="$(cd "$(dirname "$A_BIN")" 2>/dev/null && pwd)/$(basename "$A_BIN")"
fi
if [[ ! -x "$A_BIN" ]]; then
  echo "error: A binary not found at '$A_BIN'" >&2
  echo "build the compiler first (tooling/build/build.sh or cmake --build build)" >&2
  exit 1
fi

# Ensure the repo stdlib is found regardless of cwd (v2 module loader can pick
# a nearer test fixture `stdlib/` directory when run from case subdirs).
if [[ -z "${A_STDLIB:-}" && -d "$ROOT_DIR/stdlib" ]]; then
  export A_STDLIB="$ROOT_DIR/stdlib"
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

passed=0
failed=0

run_case() {
  local source="$1"
  local case_dir base name command stdout_file stderr_file stdin_file
  local expected expected_err exit_file expected_exit actual_exit rc

  case_dir="$(dirname "$source")"
  base="$(basename "$source" .a)"
  name="${case_dir#$CASES_DIR/}/$base"
  name="${name#./}"
  stdout_file="$TMP_DIR/${name//\//__}.stdout"
  stderr_file="$TMP_DIR/${name//\//__}.stderr"
  expected="$case_dir/$base.out"
  expected_err="$case_dir/$base.err"
  exit_file="$case_dir/$base.exit"
  stdin_file="$case_dir/$base.in"

  command="run"
  local flags_file="$case_dir/$base.flags"
  if [[ -f "$flags_file" ]]; then
    command="$(tr -d '\r\n' < "$flags_file")"
  fi
  if [[ ! -f "$stdin_file" ]]; then
    stdin_file="/dev/null"
  fi

  rc=0
  # shellcheck disable=SC2086
  (cd "$case_dir" && "$A_BIN" $command "$source" <"$stdin_file") \
    >"$stdout_file" 2>"$stderr_file" || rc=$?

  expected_exit=0
  if [[ -f "$exit_file" ]]; then
    expected_exit="$(tr -d '[:space:]' < "$exit_file")"
  fi
  actual_exit="$rc"

  local ok=1
  if [[ "$actual_exit" -ne "$expected_exit" ]]; then
    echo "FAIL $name"
    echo "  - exit code: expected $expected_exit, got $actual_exit"
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

# Discover every case, excluding library fixtures in `lib/` directories.
while IFS= read -r source; do
  run_case "$source"
done < <(find "$CASES_DIR" -name '*.a' -not -path '*/lib/*' | sort)

echo
echo "passed: $passed, failed: $failed"
if [[ "$failed" -ne 0 ]]; then
  exit 1
fi
