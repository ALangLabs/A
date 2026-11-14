#!/usr/bin/env bash
# A toolchain installer for Linux and macOS.
#
#   curl -fsSL https://raw.githubusercontent.com/ALangLabs/A/main/install/install.sh | sh
#
# Downloads the latest GitHub release binary, installs it under
# ${A_PREFIX:-$HOME/.local}, adds it to PATH, and installs the VS Code
# extension into every detected VS Code / fork (Cursor, Windsurf, ...).
#
# Options / environment:
#   --prefix DIR     install root (default: $A_PREFIX or ~/.local)
#   --no-extension   skip editor detection/extension install
#   A_VERSION        release tag to install (default: latest)
#   A_PREFIX         same as --prefix
set -eu

REPO="ALangLabs/A"
BASE="${A_RELEASE_BASE:-https://github.com/$REPO/releases}"
RAW="${A_REPO_RAW:-https://raw.githubusercontent.com/$REPO/main}"

PREFIX="${A_PREFIX:-$HOME/.local}"
DO_EXTENSION=1
A_VERSION="${A_VERSION:-latest}"

usage() {
  cat <<'EOF'
Usage: install.sh [--prefix DIR] [--no-extension]

Installs the A toolchain and (by default) the editor extension.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --no-extension) DO_EXTENSION=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "install: unknown argument '$1'" >&2; usage >&2; exit 64 ;;
  esac
done

PREFIX="$(mkdir -p "$PREFIX" && cd "$PREFIX" && pwd)"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# --- platform ---------------------------------------------------------------
os="$(uname -s)"
arch="$(uname -m)"
case "$os" in
  Linux)  osname="linux" ;;
  Darwin) osname="macos" ;;
  *) die "unsupported OS '$os' (supported: Linux, macOS). Build from source: $RAW" ;;
esac
case "$arch" in
  x86_64|amd64) archname="x86_64" ;;
  arm64|aarch64) archname="arm64" ;;
  *) die "unsupported architecture '$arch'" ;;
esac
platform="$osname-$archname"

if [ "$A_VERSION" = "latest" ]; then
  asset_base="$BASE/latest/download"
else
  asset_base="$BASE/download/v$A_VERSION"
fi
pkg="$asset_base/a-$platform.tar.gz"

# --- download + extract -----------------------------------------------------
need() { command -v "$1" >/dev/null 2>&1 || die "'$1' is required but not installed"; }
need tar
if command -v curl >/dev/null 2>&1; then fetch() { curl -fsSL "$1" -o "$2"; }
elif command -v wget >/dev/null 2>&1; then fetch() { wget -qO "$2" "$1"; }
else die "curl or wget is required"; fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
log "Downloading A $A_VERSION for $platform"
if ! fetch "$pkg" "$tmp/a.tar.gz"; then
  die "could not download $pkg — no prebuilt binary for $platform/$A_VERSION.\nBuild from source instead: $RAW (see README)."
fi
mkdir -p "$tmp/x"
tar -xzf "$tmp/a.tar.gz" -C "$tmp/x"
root="$(find "$tmp/x" -mindepth 1 -maxdepth 1 -type d | head -1)"
[ -n "$root" ] || die "malformed release archive"

# --- install ----------------------------------------------------------------
log "Installing to $PREFIX"
mkdir -p "$PREFIX/bin"
cp "$root"/bin/* "$PREFIX/bin/"
rm -rf "$PREFIX"/astralib "$PREFIX"/astralib-*
for d in "$root"/astralib "$root"/astralib-*; do
  [ -e "$d" ] && cp -R "$d" "$PREFIX/"
done
rm -rf "$PREFIX/stdlib"
cp -R "$root/stdlib" "$PREFIX/stdlib"
chmod +x "$PREFIX/bin/a" "$PREFIX/bin/a-lsp" 2>/dev/null || true
for f in "$PREFIX"/bin/astra*; do chmod +x "$f" 2>/dev/null || true; done

# --- PATH -------------------------------------------------------------------
case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *)
    log "Adding $PREFIX/bin to PATH"
    rc=""
    for f in "$HOME/.zshrc" "$HOME/.bashrc" "$HOME/.profile"; do
      [ -f "$f" ] && rc="$f" && break
    done
    [ -n "$rc" ] || rc="$HOME/.profile"
    {
      printf '\n# A toolchain\nexport PATH="%s:$PATH"\n' "$PREFIX/bin"
    } >> "$rc"
    warn "PATH updated in $rc — open a new shell or run: export PATH=\"$PREFIX/bin:\$PATH\""
    ;;
esac

"$PREFIX/bin/a" --version || die "installed binary failed to run"
log "A toolchain installed: $PREFIX"

# --- editor extension --------------------------------------------------------
if [ "$DO_EXTENSION" = "0" ]; then
  log "Skipping editor extension (--no-extension)"
  exit 0
fi

vsix="$tmp/a-language.vsix"
if ! fetch "$asset_base/a-language.vsix" "$vsix" 2>/dev/null; then
  # Fall back to any a-language*.vsix asset on the latest release via the API.
  url="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null \
        | sed -n 's/.*"browser_download_url": *"\([^"]*a-language[^"]*\.vsix\)".*/\1/p' \
        | head -1 || true)"
  [ -n "$url" ] && fetch "$url" "$vsix" 2>/dev/null || true
fi
if [ ! -s "$vsix" ]; then
  warn "editor extension package not found in the release; skipping"
  exit 0
fi

# VS Code-compatible editor CLIs, in preference order. `command -v` covers
# PATH installs; the extra paths cover default macOS app bundles.
editors=""
add_if_found() {
  if command -v "$1" >/dev/null 2>&1; then editors="$editors $1"; fi
}
for cli in code code-insiders codium vscodium cursor windsurf trae kiro void melty; do
  add_if_found "$cli"
done
if [ "$(uname -s)" = "Darwin" ]; then
  for app in "/Applications/Visual Studio Code.app" "/Applications/Visual Studio Code - Insiders.app" \
             "/Applications/VSCodium.app" "/Applications/Cursor.app" "/Applications/Windsurf.app"; do
    [ -d "$app" ] || continue
    for bin in "$app/Contents/Resources/app/bin/"*; do
      [ -x "$bin" ] && editors="$editors $bin"
    done
  done
fi

if [ -z "$editors" ]; then
  log "No VS Code / fork editor found — skipping extension install"
  warn "install it later with: code --install-extension $vsix"
  exit 0
fi

found=0
for cli in $editors; do
  name="$(basename "$cli")"
  if "$cli" --install-extension "$vsix" >/dev/null 2>&1; then
    log "installed A extension into $name"
    found=1
  else
    warn "extension install failed for $name (is the editor up to date?)"
  fi
done
[ "$found" = "1" ] || warn "could not install the extension automatically; retry from the editor's Extensions view"
exit 0
