#!/bin/sh
set -eu

uuid=mdeon-panel-window-manager@roy
allow_untested_versions=0
lock_enabled_extensions=0

usage()
{
    printf '%s\n' "Usage: sudo $0 [--allow-untested-shell-versions] [--lock-enabled-extensions]"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --allow-untested-shell-versions)
            allow_untested_versions=1
            ;;
        --lock-enabled-extensions)
            lock_enabled_extensions=1
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'System installation requires root; rerun this script with sudo.' >&2
    exit 1
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
extension_dir="$script_dir/$uuid"
extension_destination="/usr/share/gnome-shell/extensions/$uuid"
schema_dir=/usr/share/glib-2.0/schemas
schema_override="$schema_dir/99_mdeon-panel-window-manager.gschema.override"

mkdir -p "$extension_destination"
cp "$extension_dir/metadata.json" "$extension_dir/extension.js" "$extension_destination/"
chmod 755 "$extension_destination"
chmod 644 "$extension_destination/metadata.json" "$extension_destination/extension.js"

{
    printf '[org.gnome.shell]\n'
    printf "enabled-extensions=['%s']\n" "$uuid"
    if [ "$allow_untested_versions" -eq 1 ]; then
        printf 'disable-extension-version-validation=true\n'
    fi
} > "$schema_override"
glib-compile-schemas "$schema_dir"

if [ "$lock_enabled_extensions" -eq 1 ]; then
    dconf_root=/etc/dconf
    profile="$dconf_root/profile/user"
    system_settings="$dconf_root/db/local.d/99-mdeon-panel-window-manager"
    system_locks="$dconf_root/db/local.d/locks/99-mdeon-panel-window-manager"

    if [ ! -e "$profile" ]; then
        mkdir -p "$(dirname -- "$profile")"
        printf 'user-db:user\n' > "$profile"
    fi
    if ! grep -qxF 'user-db:user' "$profile"; then
        printf 'user-db:user\n' >> "$profile"
    fi
    if ! grep -qxF 'system-db:local' "$profile"; then
        printf 'system-db:local\n' >> "$profile"
    fi

    mkdir -p "$(dirname -- "$system_settings")" "$(dirname -- "$system_locks")"
    {
        printf '[org/gnome/shell]\n'
        printf "enabled-extensions=['%s']\n" "$uuid"
        if [ "$allow_untested_versions" -eq 1 ]; then
            printf 'disable-extension-version-validation=true\n'
        fi
    } > "$system_settings"
    {
        printf '/org/gnome/shell/enabled-extensions\n'
        if [ "$allow_untested_versions" -eq 1 ]; then
            printf '/org/gnome/shell/disable-extension-version-validation\n'
        fi
    } > "$system_locks"
    dconf update
fi

printf 'Installed %s system-wide. Log out and back in to load it.\n' "$uuid"
if [ "$lock_enabled_extensions" -eq 1 ]; then
    printf '%s\n' 'The system policy locks enabled-extensions to mdeon-panel for all users.'
elif [ "$allow_untested_versions" -eq 1 ]; then
    printf '%s\n' 'Version validation is disabled as a system default; newer Shell APIs may still be incompatible.'
else
    printf '%s\n' 'This sets the default for users without a personal enabled-extensions value; use --lock-enabled-extensions to force it for existing profiles.'
fi