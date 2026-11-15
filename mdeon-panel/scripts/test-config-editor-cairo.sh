#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/build-dir.sh"
BUILD_DIR="$(mdeon_panel_resolve_build_dir "$ROOT")"

if [[ -z "${DISPLAY:-}" ]]; then
  echo "The mdeon-panel config editor needs an X11 display (DISPLAY)." >&2
  echo "In a Wayland session, make sure XWayland is running." >&2
  exit 1
fi

export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
export MDEON_PANEL_CONFIG="${MDEON_PANEL_CONFIG:-$XDG_CONFIG_HOME/mdeon-panel/config.conf}"
export MDEON_PANEL_CONFIG_PATH="$MDEON_PANEL_CONFIG"

mkdir -p "$(dirname "$MDEON_PANEL_CONFIG")"
if [[ ! -f "$MDEON_PANEL_CONFIG" ]]; then
  cp "$ROOT/config-example.conf" "$MDEON_PANEL_CONFIG"
fi

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}" "$@"
cmake --build "$BUILD_DIR" --target mdeon-panel-config --parallel
exec "$BUILD_DIR/mdeon-panel-config"