#!/usr/bin/env bash
#
# Puts the iOS Linux engine's static library and C header where the linker
# expects them, by fetching the release built for this exact revision of
# fl_userland, or by building them locally if there is no matching release.
#
# Run by `hook/build.dart` for an iOS arm64 build, which links `libiul.a` and
# includes `iul.h` from IUL_BUILD_DIR into fl_tty's library.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/third_party/fl_userland"
REPO="lollipopkit/fl_userland"

# The hook sets it when the engine is wanted; an app that set `iul: false`
# links none of it, and there is nothing to fetch.
[ "${SBM_IUL:-0}" = "1" ] || exit 0

case "${PLATFORM_NAME:-}" in
  iphonesimulator) target=simulator; asset=libiul-iphonesimulator.a ;;
  iphoneos) target=device; asset=libiul-iphoneos.a ;;
  # macOS reaches a shell through flutter_pty, and anything else is not a
  # platform this engine is built for.
  *) exit 0 ;;
esac

# Set alongside the link flags, so the two cannot disagree about where the
# libraries are meant to be.
lib_dir="${IUL_BUILD_DIR:-}"
if [ -z "$lib_dir" ]; then
  echo "warning: IUL_BUILD_DIR is unset, leaving the engine to the linker" >&2
  exit 0
fi

if [ ! -d "$SRC_DIR/.git" ] && [ ! -f "$SRC_DIR/.git" ]; then
  echo "error: third_party/fl_userland is not checked out" >&2
  echo "       git submodule update --init third_party/fl_userland" >&2
  exit 1
fi

sha="$(git -C "$SRC_DIR" rev-parse HEAD)"
stamp="$lib_dir/.iul-libs-sha"

version_tag() {
  local tags
  tags="$(git -C "$SRC_DIR" tag --points-at HEAD --list 'v[0-9]*' --sort=-v:refname)"
  printf '%s\n' "${tags%%$'\n'*}"
}

tag="$(version_tag)"
if [ -z "$tag" ]; then
  git -C "$SRC_DIR" fetch --tags --quiet origin 2>/dev/null || true
  tag="$(version_tag)"
fi

have_libs() {
  [ -f "$lib_dir/libiul.a" ] && [ -f "$lib_dir/iul.h" ]
}

stamp_libs() {
  printf '%s\n' "$sha" > "$stamp"
}

if have_libs; then
  [ "$(cat "$stamp" 2>/dev/null)" = "$sha" ] && exit 0
  echo "note: the engine in $lib_dir was built for another revision, replacing it" >&2
fi

build_locally() {
  # Clear IUL_BUILD_DIR (the work root in build-iul-ios.sh) and pass the
  # target output directory via IUL_OUTPUT_DIR, skipping Alpine rootfs fetch.
  IUL_OUTPUT_DIR="$lib_dir" IUL_SKIP_ALPINE=1 \
    env -u IUL_BUILD_DIR "$SCRIPT_DIR/build-iul-ios.sh" "$target" >&2
}

if [ -n "$tag" ]; then
  base="https://github.com/$REPO/releases/download/$tag"
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT

  echo "note: fetching the iOS Linux engine for $target, $tag" >&2
  if curl -fsSL --retry 2 -o "$tmp/$asset" "$base/$asset" &&
     curl -fsSL --retry 2 -o "$tmp/iul.h" "$base/iul.h" &&
     curl -fsSL --retry 2 -o "$tmp/SHA256SUMS" "$base/SHA256SUMS"; then
    awk -v asset="$asset" '
      NF == 2 && $1 ~ /^[0-9a-fA-F]{64}$/ && $2 == asset { seen_a = 1; print }
      NF == 2 && $1 ~ /^[0-9a-fA-F]{64}$/ && $2 == "iul.h" { seen_h = 1; print }
      END { exit (seen_a == 1 && seen_h == 1) ? 0 : 1 }
    ' "$tmp/SHA256SUMS" > "$tmp/check.sha256" || {
      echo "error: release $tag SHA256SUMS is incomplete or malformed" >&2
      exit 1
    }
    (cd "$tmp" && shasum -a 256 -c "$tmp/check.sha256" >/dev/null) || {
      echo "error: checksum verification failed for release $tag" >&2
      exit 1
    }
    mkdir -p "$lib_dir"
    rm -f "$lib_dir/libiul.a" "$lib_dir/iul.h"
    mv "$tmp/$asset" "$lib_dir/libiul.a"
    mv "$tmp/iul.h" "$lib_dir/iul.h"
    have_libs || { echo "error: release $tag did not provide libiul.a and iul.h" >&2; exit 1; }
    stamp_libs
    echo "note: installed into $lib_dir" >&2
    exit 0
  fi
fi

# No release for this revision or download failed.
for tool in cargo rustc; do
  command -v "$tool" >/dev/null 2>&1 && continue
  echo "error: no release ${tag:-for $sha}, and no Rust toolchain to build locally" >&2
  echo "       Point third_party/fl_userland at a tagged release, or install rustup/cargo." >&2
  exit 1
done

echo "note: no release ${tag:-for $sha}, building from source instead" >&2
build_locally
have_libs || { echo "error: the local build left no libiul.a/iul.h in $lib_dir" >&2; exit 1; }
stamp_libs
