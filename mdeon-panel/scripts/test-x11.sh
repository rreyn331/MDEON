#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export MDEON_PANEL_DEBUG=1
exec "$ROOT/run-cairo-x11.sh" "$@"
