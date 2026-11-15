#!/usr/bin/env bash
set -euo pipefail

# Test uninstaller for MDEON files
#
# Usage:
#   sudo ./remove-mdeon-test.sh
#
# Optional source tree (used to identify the deb payload):
#   sudo ./remove-mdeon-test.sh /path/to/MDEON
#
# Optional staged removal:
#   sudo DESTDIR=/tmp/mdeon-test-root ./remove-mdeon-test.sh

SRC_ROOT="${1:-/home/roy/Desktop/tweaks/MDEON}"
DESTDIR="${DESTDIR:-}"

if [[ $EUID -ne 0 && -z "$DESTDIR" ]]; then
  echo "This remover must be run as root."
  echo "Use: sudo $0 (or set DESTDIR for a staged removal)"
  exit 1
fi

if [[ ! -d "$SRC_ROOT" ]]; then
  echo "Source directory not found: $SRC_ROOT"
  exit 1
fi

mapfile -d '' FILES < <(
  find "$SRC_ROOT/etc" "$SRC_ROOT/usr" -type f -print0 | sort -z
)

mapfile -d '' DIRS < <(
  find "$SRC_ROOT/etc" "$SRC_ROOT/usr" -mindepth 1 -type d -print0 | sort -zr
)

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "No package files found under $SRC_ROOT/etc or $SRC_ROOT/usr"
  exit 1
fi

echo "Removing MDEON test files"
echo "Destination: ${DESTDIR:-/}"
echo

for src in "${FILES[@]}"; do
  relpath="${src#"$SRC_ROOT"/}"
  target="${DESTDIR}/$relpath"

  if [[ -e "$target" || -L "$target" ]]; then
    rm -f "$target"
    echo "Removed: /$relpath"
  else
    echo "Not found: /$relpath"
  fi
done

echo
echo "Removing empty directories if possible..."
echo

for srcdir in "${DIRS[@]}"; do
  relpath="${srcdir#"$SRC_ROOT"/}"
  dir="${DESTDIR}/$relpath"

  if [[ -d "$dir" ]]; then
    rmdir "$dir" 2>/dev/null && echo "Removed empty directory: /$relpath" || true
  fi
done

if command -v update-desktop-database >/dev/null 2>&1; then
  if [[ -d "${DESTDIR}/usr/share/applications" ]]; then
    echo
    echo "Updating desktop application database..."
    update-desktop-database "${DESTDIR}/usr/share/applications" || true
  fi
fi

echo
echo "MDEON test files removed."
