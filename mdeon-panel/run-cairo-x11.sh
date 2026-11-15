#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export MDEON_PANEL_BACKEND=x11
exec "$root/run-cairo.sh" "$@"
