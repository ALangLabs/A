#!/usr/bin/env bash
# Assemble a redistributable A toolchain payload from an existing build tree.
#
# The payload mirrors what the `a` driver discovers at run time:
#   <root>/bin/a, a-lsp, astra, astra-<arch>...   (frontend + backend drivers)
#   <root>/astralib/, astralib-<arch>/...          (staged runtime sources + libs)
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

# Astra backend drivers (host arch + every cross arch built for this OS).
shopt -s nullglob
for drv in "$BUILD_DIR"/astra/astra*; do
  [ -f "$drv" ] && cp "$drv" "$root/bin/"
done

# Staged per-arch runtime directories (runtime C sources + libastra1.a +
# libaruntime.a + headers) — `a run`/`a build` need these next to the prefix.
for lib in "$BUILD_DIR"/astralib*; do
  [ -d "$lib" ] && cp -R "$lib" "$root/"
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
