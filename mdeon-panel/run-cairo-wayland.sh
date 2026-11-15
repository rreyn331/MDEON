#!/usr/bin/env bash
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
state_dir=${XDG_STATE_HOME:-"$HOME/.local/state"}/mdeon-panel
log_file=${MDEON_PANEL_DEBUG_LOG:-${SIMPANEL_DEBUG_LOG:-"$state_dir/wayland-debug.log"}}

umask 077
mkdir -p "$(dirname -- "$log_file")"
if test -f "$log_file" && test "$(wc -c < "$log_file")" -ge 5242880; then
	mv -f "$log_file" "$log_file.1"
fi

{
	printf '\n=== Simpanel Wayland run started: %s ===\n' "$(date --iso-8601=seconds)"
	printf 'launcher_pid=%s\n' "$$"
	printf 'session_type=%s\n' "${XDG_SESSION_TYPE-}"
	printf 'session_id=%s\n' "${XDG_SESSION_ID-}"
	printf 'desktop=%s\n' "${XDG_CURRENT_DESKTOP-}"
	printf '\n--- recent GNOME user journal ---\n'
	journalctl --user -b -n 200 --no-pager 2>&1 || true
	printf '\n--- live user journal follows ---\n'
} >> "$log_file"

journalctl --user --follow --output=short-iso --no-pager >> "$log_file" 2>&1 &
journal_pid=$!
cleanup()
{
	kill "$journal_pid" 2>/dev/null || true
	wait "$journal_pid" 2>/dev/null || true
}
trap cleanup EXIT

export MDEON_PANEL_BACKEND=wayland
if "$root/run-cairo.sh" "$@" 2>&1 | tee -a "$log_file"; then
	panel_status=0
else
	panel_status=${PIPESTATUS[0]}
fi
printf 'Simpanel Wayland launcher exited at %s with status %s\n' \
	"$(date --iso-8601=seconds)" "$panel_status" >> "$log_file"
exit "$panel_status"
