#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_ROOT="$SCRIPT_DIR"
OUTPUT_DIR="${1:-$SCRIPT_DIR/dist}"
CONTROL_FILE="$PACKAGE_ROOT/DEBIAN/control"

if ! command -v dpkg-deb >/dev/null 2>&1; then
  echo "dpkg-deb is required to build the package." >&2
  exit 1
fi

for path in "$CONTROL_FILE" "$PACKAGE_ROOT/DEBIAN/postinst" \
            "$PACKAGE_ROOT/DEBIAN/prerm" "$PACKAGE_ROOT/etc" "$PACKAGE_ROOT/usr"; do
  if [[ ! -e "$path" ]]; then
    echo "Required package file or directory is missing: $path" >&2
    exit 1
  fi
done

PACKAGE="$(awk -F ': ' '$1 == "Package" { print $2; exit }' "$CONTROL_FILE")"
VERSION="$(date '+%Y.%m.%d.%H%M')"
ARCHITECTURE="$(awk -F ': ' '$1 == "Architecture" { print $2; exit }' "$CONTROL_FILE")"

if [[ ! "$PACKAGE" =~ ^[a-z0-9][a-z0-9+.-]*$ ||
      ! "$VERSION" =~ ^[0-9][a-zA-Z0-9.+:~_-]*$ ||
      ! "$ARCHITECTURE" =~ ^[a-z0-9][a-z0-9-]*$ ]]; then
  echo "Package, Version, or Architecture is missing or invalid in $CONTROL_FILE" >&2
  exit 1
fi

mkdir -p "$OUTPUT_DIR"
STAGING_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGING_DIR"' EXIT
mkdir -p "$STAGING_DIR/DEBIAN"
cp -a "$PACKAGE_ROOT/etc" "$PACKAGE_ROOT/usr" "$STAGING_DIR/"
find "$STAGING_DIR/etc" "$STAGING_DIR/usr" -type d -exec chmod 755 {} +
find "$STAGING_DIR/etc" "$STAGING_DIR/usr" -type f -perm /111 -exec chmod 755 {} +
find "$STAGING_DIR/etc" "$STAGING_DIR/usr" -type f ! -perm /111 -exec chmod 644 {} +
sed "s/^Version: .*/Version: $VERSION/" "$CONTROL_FILE" > "$STAGING_DIR/DEBIAN/control"
chmod 644 "$STAGING_DIR/DEBIAN/control"
install -m 644 "$PACKAGE_ROOT/DEBIAN/conffiles" "$STAGING_DIR/DEBIAN/conffiles"
install -m 755 "$PACKAGE_ROOT/DEBIAN/postinst" "$STAGING_DIR/DEBIAN/postinst"
install -m 755 "$PACKAGE_ROOT/DEBIAN/prerm" "$STAGING_DIR/DEBIAN/prerm"

DEB_PATH="$OUTPUT_DIR/${PACKAGE}_${VERSION}_${ARCHITECTURE}.deb"
dpkg-deb --build --root-owner-group "$STAGING_DIR" "$DEB_PATH"
printf 'Built package: %s\n' "$DEB_PATH"