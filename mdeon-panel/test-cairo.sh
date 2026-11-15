#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$root/scripts/build-dir.sh"
build_dir=$(mdeon_panel_resolve_build_dir "$root")
binary="$build_dir/mdeon-panel"
export MDEON_PANEL_DEBUG=1

"$root/build-cairo.sh" "$@"

if [ ! -x "$binary" ]; then
	printf '%s\n' "Build smoke check failed: $binary was not created." >&2
	exit 1
fi

if [ -n "${DISPLAY:-}" ]; then
	if ! command -v timeout >/dev/null 2>&1; then
		printf '%s\n' "Skipping X11 startup check: timeout is unavailable."
	elif timeout --signal=TERM 2s "$binary"; then
		printf '%s\n' "X11 startup check failed: the panel exited before the timeout." >&2
		exit 1
	else
		status=$?
		if [ "$status" -ne 124 ]; then
			printf '%s\n' "X11 startup check failed with exit status $status." >&2
			exit 1
		fi
		printf '%s\n' "X11 startup check passed; the test panel was stopped after 2 seconds."
	fi
else
	printf '%s\n' "Skipping X11 startup check: DISPLAY is not set."
fi

if ctest --test-dir "$build_dir" -N | grep -Eq 'Total Tests: [1-9][0-9]*'; then
	ctest --test-dir "$build_dir" --output-on-failure
else
	printf '%s\n' "Build smoke check passed; no CTest tests are registered yet."
fi