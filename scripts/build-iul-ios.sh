#!/usr/bin/env bash
#
# Builds the iul (libiul.a) engine for iOS, and optionally unpacks an
# Alpine minirootfs tree for local testing.
#
# What this produces
# ------------------
#   build-<name>/libiul.a   static library for one SDK and arch
#   build-<name>/iul.h      C embedder header copied from third_party/iul
#   alpine-tree/            unpacked Alpine minirootfs (for simulator/device)
#
# Usage: scripts/build-iul-ios.sh [simulator|device|macos]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${IUL_BUILD_DIR:-$REPO_ROOT/build/iul}"
SRC_DIR="$REPO_ROOT/third_party/iul"

ALPINE_VERSION=3.22.5
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/aarch64/alpine-minirootfs-${ALPINE_VERSION}-aarch64.tar.gz"
ALPINE_SHA256=3fbc6285032ed46821b511292633d7b2a6306a2e254f590e92bdafff56cf2f70

TARGET="${1:-simulator}"

log() { printf '\033[0;34m==>\033[0m %s\n' "$*" >&2; }
die() { printf '\033[0;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

for tool in cargo rustc git curl; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool is not installed"
done

mkdir -p "$WORK_DIR"

require_source() {
  if [ ! -f "$SRC_DIR/Cargo.toml" ] || [ ! -x "$SRC_DIR/tools/build-libs.sh" ]; then
    die "third_party/iul is missing — run: git submodule update --init third_party/iul"
  fi
}

build_libs() {
  local name="$1" rust_target="$2"
  local build_dir="${IUL_OUTPUT_DIR:-$WORK_DIR/build-$name}"

  log "Building $name ($rust_target)"
  "$SRC_DIR/tools/build-libs.sh" "$build_dir" --release --target "$rust_target"
  [ -f "$build_dir/libiul.a" ] || die "libiul.a did not build"

  if [ "$rust_target" = "aarch64-apple-ios" ] && [ -x "$SRC_DIR/tools/ios-symbol-floor.sh" ]; then
    local llvm_nm
    llvm_nm="$(rustc --print sysroot)/lib/rustlib/$(rustc -vV | sed -n 's/^host: //p')/bin/llvm-nm"
    if [ -x "$llvm_nm" ]; then
      log "Checking iOS symbol floor for $name"
      "$SRC_DIR/tools/ios-symbol-floor.sh" "$build_dir/libiul.a"
    fi
  fi

  cp "$SRC_DIR/iul.h" "$build_dir/iul.h"
  git -C "$SRC_DIR" rev-parse HEAD > "$build_dir/.iul-libs-sha"
  log "Built into $build_dir"
  ls -la "$build_dir/libiul.a" "$build_dir/iul.h"
}

fetch_alpine() {
  local tarball="$1"
  if [ ! -f "$tarball" ]; then
    log "Fetching Alpine $ALPINE_VERSION"
    curl -fsSL --retry 3 -o "$tarball" "$ALPINE_URL"
  fi
  local got
  got="$(sha256 "$tarball")"
  [ "$got" = "$ALPINE_SHA256" ] || { rm -f "$tarball"; die "Alpine tarball digest mismatch: expected $ALPINE_SHA256, got $got"; }
}

build_tree() {
  local tarball="$WORK_DIR/alpine-$ALPINE_VERSION.tar.gz"
  local out="$WORK_DIR/alpine-tree"
  fetch_alpine "$tarball"
  [ -d "$out" ] && { log "Tree already unpacked at $out"; return; }
  log "Unpacking the Alpine tree"
  mkdir -p "$out"
  tar xzf "$tarball" -C "$out" 2>/dev/null || true
  [ -f "$out/etc/alpine-release" ] || die "the tree did not unpack"
  log "Unpacked $out"
}

require_source
case "$TARGET" in
  simulator)
    build_libs iossim-arm64 aarch64-apple-ios-sim
    [ "${IUL_SKIP_ALPINE:-0}" = "1" ] || build_tree
    ;;
  device)
    build_libs ios-arm64 aarch64-apple-ios
    [ "${IUL_SKIP_ALPINE:-0}" = "1" ] || build_tree
    ;;
  macos)
    build_libs macos-arm64 aarch64-apple-darwin
    [ "${IUL_SKIP_ALPINE:-0}" = "1" ] || build_tree
    ;;
  *) die "unknown target: $TARGET (simulator|device|macos)" ;;
esac

cat <<'NOTE'

Built. The next iOS build links libiul.a: the engine is on by default (the
`iul` hook user-define), and these carry the stamp that keeps the hook from
replacing them with a published release.
NOTE
