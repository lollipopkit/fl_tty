#!/usr/bin/env bash
# Prints the directory holding iul's benchmark assets (shellbench.sh and the
# optional prebuilt aarch64 binary), keeping fl_tty's internal third_party
# layout private to this package.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
assets="$root/third_party/iul/benchmark/assets"

if [ ! -f "$assets/shellbench.sh" ]; then
    echo "no $assets/shellbench.sh — git submodule update --init --recursive" >&2
    exit 1
fi

printf '%s\n' "$assets"
