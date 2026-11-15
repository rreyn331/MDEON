#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$root/scripts/build-dir.sh"
build_dir=$(mdeon_panel_resolve_build_dir "$root")
cmake -S "$root" -B "$build_dir" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}" "$@"
cmake --build "$build_dir" --parallel