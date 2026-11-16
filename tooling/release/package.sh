#!/usr/bin/env bash
# Assemble a redistributable A toolchain payload from an existing build tree.
#
# The payload mirrors what the `a` driver discovers at run time:
#   <root>/bin/a, a-lsp                            (frontend binaries)
#   <root>/astralib/                                (staged runtime sources + libaruntime.a)
#   <root>/stdlib/                                (standard library sources)
#
# usage: package.sh <build-dir> <dist-dir> [platform]
#   platform defaults to $(uname -s | tr A-Z a-z)-$(uname -m)
# Outputs: <dist-dir>/a-<platform>.tar.gz (and .zip on Windows)
#          <dist-dir>/a-language.vsix (the checked-in extension package)
set -euo pipefail

BUILD_DIR="$(cd "$1" && pwd)"
DIST_DIR="$2"
PLATFORM="${3:-$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

version="$(sed -n 's/^project(Alang VERSION \([^ ]*\).*/\1/p' "$ROOT_DIR/CMakeLists.txt")"
name="a-$version-$PLATFORM"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
root="$stage/$name"
mkdir -p "$root/bin" "$DIST_DIR"

# Frontend binaries: `a`/`a-lsp` are `a`/`a.exe` etc. per platform.
for b in a a-lsp; do
  for ext in "" ".exe"; do
    if [ -f "$BUILD_DIR/$b$ext" ]; then
      cp "$BUILD_DIR/$b$ext" "$root/bin/"
    fi
  done
done
[ -f "$root/bin/a" ] || [ -f "$root/bin/a.exe" ] || {
  echo "package: no 'a' binary found in $BUILD_DIR" >&2; exit 1; }

# Staged runtime directory (runtime C sources + libaruntime.a + headers) —
# `a run`/`a build` need it next to the prefix. cmake/aruntime.cmake produces
# build/astralib; only the host-arch stage exists (the LLVM backend is
# host-only).
for lib in "$BUILD_DIR"/astralib; do
  [ -d "$lib" ] && cp -R "$lib" "$root/"
done

# LLVM toolchain: the `a` binary discovers opt/llc/clang/mlir-translate under
# <exe>/llvm-tools/ before PATH (see findLLVMTool/findClangDriver), so release
# bundles ship the tools that were used to build — no system LLVM required on
# the target machine. A tool is only bundled when it actually executes:
# distro clangs are often symlinks into versioned installs whose shared libs
# (libclang-cpp, libLLVM) do not travel with a bare binary copy.
llvm_dir="$root/bin/llvm-tools"
mkdir -p "$llvm_dir"
# tool|env override|candidate names (unversioned first, then -18..-15)
pick_tool() {
  local resolved=""
  for cand in "$@"; do
    resolved="$(command -v "$cand" 2>/dev/null || true)"
    [ -n "$resolved" ] || continue
    if "$resolved" --version >/dev/null 2>&1; then
      printf '%s' "$resolved"
      return 0
    fi
  done
  return 1
}
# A bare binary copy is not always relocatable (distro clangs need
# libclang-cpp/libLLVM .so's, Homebrew clangs need ../lib dylibs). The driver
# probes llvm-tools/ before PATH, so a bundled tool that cannot execute would
# shadow a working system compiler — verify the copy and drop it if broken.
bundle_tool() {
  local tool="$1" src="$2" dest="$llvm_dir/$1"
  case "$src" in *.exe) dest="$dest.exe" ;; esac
  cp "$src" "$dest"
  if ! "$dest" --version >/dev/null 2>&1; then
    rm -f "$dest"
    echo "package: warning: bundled '$tool' does not run standalone; dropped" >&2
    return 1
  fi
}
for spec in \
    "opt|A_LLVM_OPT|opt opt-18 opt-17 opt-16 opt-15" \
    "llc|A_LLVM_LLC|llc llc-18 llc-17 llc-16 llc-15" \
    "clang|A_LLVM_CC|clang clang-18 clang-17 clang-16 clang-15 clang++ clang++-18 clang++-17" \
    "mlir-translate|A_MLIR_TRANSLATE|mlir-translate mlir-translate-18 mlir-translate-17"; do
  tool="${spec%%|*}"
  rest="${spec#*|}"
  env_name="${rest%%|*}"
  names="${rest#*|}"
  # Honour the same env overrides the driver does (A_LLVM_OPT etc.).
  override="$(printenv "$env_name" 2>/dev/null || true)"
  if [ -n "$override" ] && "$override" --version >/dev/null 2>&1; then
    bundle_tool "$tool" "$override" && continue
  fi
  resolved="$(pick_tool $names || true)"
  if [ -n "$resolved" ]; then
    bundle_tool "$tool" "$resolved" || true
  else
    echo "package: warning: no working '$tool' found (PATH fallback at runtime)" >&2
  fi
done

# Standard library (drop _acache_ build artifacts and stray objects).
cp -R "$ROOT_DIR/stdlib" "$root/stdlib"
find "$root/stdlib" -type d -name _acache_ -prune -exec rm -rf {} + 2>/dev/null || true

cp "$ROOT_DIR/README.md" "$ROOT_DIR/LICENSE" "$root/" 2>/dev/null || true

tar -C "$stage" -czf "$DIST_DIR/a-$PLATFORM.tar.gz" "$name"
echo "wrote $DIST_DIR/a-$PLATFORM.tar.gz"

if [[ "$PLATFORM" == *windows* ]]; then
  # bsdtar writes zip archives with -a; install.ps1 prefers .zip when present.
  if tar -a -cf "$DIST_DIR/a-$PLATFORM.zip" -C "$stage" "$name" 2>/dev/null; then
    echo "wrote $DIST_DIR/a-$PLATFORM.zip"
  else
    echo "package: warning: could not write .zip (tar.gz still usable)" >&2
  fi
fi

# Editor extension: ship the checked-in vsix under a stable asset name.
vsix="$(ls -1 "$ROOT_DIR"/tools/vscode-a/a-language-*.vsix 2>/dev/null | sort -V | tail -1 || true)"
if [ -n "$vsix" ]; then
  cp "$vsix" "$DIST_DIR/a-language.vsix"
  echo "wrote $DIST_DIR/a-language.vsix"
else
  echo "package: warning: no tools/vscode-a/a-language-*.vsix found" >&2
fi
