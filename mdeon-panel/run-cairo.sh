#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$root/scripts/build-dir.sh"
build_dir=$(mdeon_panel_resolve_build_dir "$root")
MDEON_PANEL_BUILD_DIR="$build_dir" "$root/build-cairo.sh"
export MDEON_PANEL_CONFIG=${MDEON_PANEL_CONFIG:-${XDG_CONFIG_HOME:-"$HOME/.config"}/mdeon-panel/config.conf}
mkdir -p "$(dirname -- "$MDEON_PANEL_CONFIG")"
if [ ! -f "$MDEON_PANEL_CONFIG" ]; then
	cp "$root/config-example.conf" "$MDEON_PANEL_CONFIG"
fi
exec "$build_dir/mdeon-panel" "$@"