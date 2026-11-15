#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/build-dir.sh"
BUILD_DIR="$(mdeon_panel_resolve_build_dir "$ROOT")"
rm -rf "$BUILD_DIR" "$ROOT"/dist "$ROOT"/*.egg-info "$ROOT"/debbuild "$ROOT"/deb "$ROOT"/__pycache__
find "$ROOT" -type d -name "__pycache__" -prune -exec rm -rf {} +
find "$ROOT" -type f \( -name "*.pyc" -o -name "*.pyo" \) -delete

echo "Build and temporary outputs cleaned."
