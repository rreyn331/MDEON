#!/usr/bin/env bash
set -euo pipefail

# Test installer for MDEON files
#
# Default source tree:
#   /home/roy/Desktop/tweaks/MDEON
#
# Usage:
#   sudo ./install-mdeon-test.sh
#
# Optional:
#   sudo ./install-mdeon-test.sh /path/to/MDEON
#
# Optional staging install:
#   sudo DESTDIR=/tmp/mdeon-test-root ./install-mdeon-test.sh

SRC_ROOT="${1:-/home/roy/Desktop/tweaks/MDEON}"
DESTDIR="${DESTDIR:-}"
BACKUP_DIR="${DESTDIR}/var/backups/mdeon-test-install/$(date +%Y%m%d-%H%M%S)"

if [[ $EUID -ne 0 && -z "$DESTDIR" ]]; then
  echo "This installer must be run as root."
  echo "Use: sudo $0 (or set DESTDIR for a staged install)"
  exit 1
fi

if [[ ! -d "$SRC_ROOT" ]]; then
  echo "Source directory not found: $SRC_ROOT"
  exit 1
fi

# A deb installs the data tree outside DEBIAN. Keep the test installer in sync
# with that tree instead of maintaining a second, manually edited manifest.
mapfile -d '' FILES < <(
  find "$SRC_ROOT/etc" "$SRC_ROOT/usr" -type f -print0 | sort -z
)

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "No package files found under $SRC_ROOT/etc or $SRC_ROOT/usr"
  exit 1
fi

echo "Installing MDEON test files"
echo "Source:      $SRC_ROOT"
echo "Destination: ${DESTDIR:-/}"
echo

for src in "${FILES[@]}"; do
  relpath="${src#"$SRC_ROOT"/}"
  dest="${DESTDIR}/$relpath"
  dest_dir="$(dirname "$dest")"

  mkdir -p "$dest_dir"

  if [[ -e "$dest" ]]; then
    backup_path="$BACKUP_DIR/$relpath"
    mkdir -p "$(dirname "$backup_path")"
    cp -a "$dest" "$backup_path"
    echo "Backed up existing file: $dest"
  fi

  install -m "$(stat -c '%a' "$src")" "$src" "$dest"

  echo "Installed: /$relpath"
done

echo

if command -v update-desktop-database >/dev/null 2>&1; then
  echo "Updating desktop application database..."
  update-desktop-database "${DESTDIR}/usr/share/applications" || true
fi

echo
echo "MDEON test installation complete."

if [[ -d "$BACKUP_DIR" ]]; then
  echo "Backups saved in:"
  echo "  $BACKUP_DIR"
fi
