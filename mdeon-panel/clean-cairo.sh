#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$root/scripts/build-dir.sh"
build_dir=$(mdeon_panel_resolve_build_dir "$root")
rm -rf "$build_dir"
printf '%s\n' "Removed $build_dir"