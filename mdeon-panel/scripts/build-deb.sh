#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CHANGELOG="$ROOT/debian/changelog"
CHANGELOG_BACKUP="$(mktemp)"
CHANGELOG_TEMP="$(mktemp)"
cp "$CHANGELOG" "$CHANGELOG_BACKUP"

restore_changelog() {
	cp "$CHANGELOG_BACKUP" "$CHANGELOG"
	rm -f "$CHANGELOG_BACKUP" "$CHANGELOG_TEMP"
}
trap restore_changelog EXIT

SOURCE_PACKAGE="$(awk -F ': ' '$1 == "Source" { print $2; exit }' "$ROOT/debian/control")"
VERSION="$(date '+%Y.%m.%d.%H%M')"
BUILD_DATE="$(date -R)"
{
	printf '%s (%s) unstable; urgency=medium\n\n' "$SOURCE_PACKAGE" "$VERSION"
	printf '  * Build with the MDEON-Session timestamp version format.\n\n'
	printf ' -- mdeon <mdeon@example.com>  %s\n\n' "$BUILD_DATE"
	cat "$CHANGELOG_BACKUP"
} > "$CHANGELOG_TEMP"
mv "$CHANGELOG_TEMP" "$CHANGELOG"

dpkg-buildpackage -us -uc -b