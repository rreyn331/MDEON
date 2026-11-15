#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if (( $# > 1 )); then
	printf 'Usage: %s [output-directory]\n' "$0" >&2
	exit 2
fi

output_dir=${1:-"$ROOT"}
mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd)
timestamp=$(date '+%Y%m%d-%H%M%S')
archive="$output_dir/mdeon-panel-build-sources-$timestamp.zip"
suffix=1
while [[ -e "$archive" ]]; do
	archive="$output_dir/mdeon-panel-build-sources-$timestamp-$suffix.zip"
	((suffix += 1))
done

cd "$ROOT"
entries=(
	CMakeLists.txt README.md DEPENDENCIES-cairo.md MDEON.svg
	config-example.conf mdeon-panel-config.desktop
	build.sh build-cairo.sh clean.sh clean-cairo.sh test.sh test-cairo.sh
	scripts src include protocols tests extensions/gnome plugins.d
	debian/changelog debian/control debian/install debian/rules
	debian/source/format debian/mdeon-panel-cairo.postinst debian/mdeon-panel-cairo.postrm
)

if command -v zip >/dev/null 2>&1; then
	zip -q -r "$archive" "${entries[@]}"
elif command -v python3 >/dev/null 2>&1; then
	python3 - "$archive" "${entries[@]}" <<'PY'
from pathlib import Path
import sys
from zipfile import ZIP_DEFLATED, ZipFile

archive = sys.argv[1]
with ZipFile(archive, "w", ZIP_DEFLATED) as bundle:
    for entry in sys.argv[2:]:
        path = Path(entry)
        if path.is_dir():
            for child in path.rglob("*"):
                if child.is_file():
                    bundle.write(child, child.as_posix())
        else:
            bundle.write(path, path.as_posix())
PY
else
	printf '%s\n' 'Cannot create backup: install zip or Python 3.' >&2
	exit 1
fi

printf 'Created %s\n' "$archive"