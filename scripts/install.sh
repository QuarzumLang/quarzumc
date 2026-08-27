#!/usr/bin/env bash
#
# install.sh - Build the Quarzum compiler and install it system-wide on Linux.
#
# What it does:
#   1. Compiles the compiler (C sources) into build/quarzum, with LIB_PATH
#      pointing at the installed standard library so that `import "@std/..."`
#      works from any directory after installation.
#   2. Installs the binary into <PREFIX>/bin/quarzum.
#   3. Copies the lib/ folder into <PREFIX>/lib/quarzum (the standard library).
#
# Usage:
#   ./scripts/install.sh            # installs into /usr/local
#   PREFIX=/usr ./scripts/install.sh   # installs into /usr (i.e. /usr/bin, /usr/lib/quarzum)
#
# The script uses `sudo` automatically when the target directories are not
# writable by the current user.
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
PREFIX="${PREFIX:-/usr/local}"
BINDIR="$PREFIX/bin"
LIBDIR="$PREFIX/lib/quarzum"
BIN_NAME="quarzumc"

# Resolve the repository root (the directory that contains this script's parent).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/src"
LIB_SRC="$REPO_ROOT/lib"
BUILD_DIR="$REPO_ROOT/build"

# Run with sudo only if we lack write permission to the destination prefix.
# (A directory that does not exist yet is fine as long as its parent is writable.)
SUDO=""
if [[ ! -w "$PREFIX" ]]; then
    if [[ -e "$PREFIX" ]] || [[ ! -w "$(dirname "$PREFIX")" ]]; then
        SUDO="sudo"
    fi
fi

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
info()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
die()   { printf '\033[1;31mError:\033[0m %s\n' "$*" >&2; exit 1; }

command -v gcc >/dev/null 2>&1 || die "gcc is required but was not found in PATH."

# ---------------------------------------------------------------------------
# 1. Build the compiler (via the project's Makefile)
# ---------------------------------------------------------------------------
info "Building Quarzum compiler via make (build/${BIN_NAME}-install)"
# LIB_PATH is forwarded to the compiler so imports like `@std/...` resolve
# relative to the installed library directory after installation. -B forces a
# rebuild so the correct (install-time) LIB_PATH is always baked in.
make -B -C "$REPO_ROOT" build/quarzumc-install LIB_PATH="$LIBDIR"

BUILT="$BUILD_DIR/${BIN_NAME}-install"
[[ -x "$BUILT" ]] || die "build failed: $BUILT was not produced."

# ---------------------------------------------------------------------------
# 2. Install the binary
# ---------------------------------------------------------------------------
info "Installing binary to $BINDIR/$BIN_NAME"
$SUDO mkdir -p "$BINDIR"
$SUDO install -m 0755 "$BUILT" "$BINDIR/$BIN_NAME"

# ---------------------------------------------------------------------------
# 3. Register the binary on PATH
# ---------------------------------------------------------------------------
# Most systems already include $BINDIR (e.g. /usr/local/bin) on PATH, but we
# make it explicit so `quarzumc` is available in new shells regardless.
if [[ -n "$SUDO" ]]; then
    PROFILE_D="/etc/profile.d/quarzumc.sh"
    $SUDO bash -c "cat > '$PROFILE_D' <<EOF
# Added by Quarzum installer
case \":\$PATH:\" in
    *\":$BINDIR:\"*) ;;
    *) export PATH=\"\$PATH:$BINDIR\" ;;
esac
EOF"
    $SUDO chmod 0644 "$PROFILE_D"
    info "Registered $BIN_NAME on PATH via $PROFILE_D"
else
    info "Note: $BINDIR is not on the system PATH or this is a user install."
    info "      Add it to your shell profile if needed:  export PATH=\"\$PATH:$BINDIR\""
fi

# ---------------------------------------------------------------------------
# 4. Install the standard library
# ---------------------------------------------------------------------------
info "Installing standard library to $LIBDIR"
# Do a clean install: remove any previous copy first so a reinstall exactly
# matches the current source lib (no stale files left behind). Guard the
# removal so we only ever delete a directory ending in /lib/quarzum.
case "$LIBDIR" in
    */lib/quarzum) ;;
    *) die "refusing to remove unexpected LIBDIR '$LIBDIR' (expected '.../lib/quarzum')." ;;
esac
$SUDO rm -rf "$LIBDIR"
$SUDO mkdir -p "$LIBDIR"
# Copy the contents of lib/ (so $LIBDIR/std, $LIBDIR/errors, ... exist).
$SUDO cp -r "$LIB_SRC/." "$LIBDIR/"

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
info "Quarzum installed successfully."
info "  binary : $BINDIR/$BIN_NAME"
info "  library: $LIBDIR  (LIB_PATH=$LIBDIR)"
info ""
info "Try it:  quarzumc --version"
info "         quarzumc yourfile.qz --build"
