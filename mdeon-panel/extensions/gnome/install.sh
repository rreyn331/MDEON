#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
extension_id=mdeon-panel-window-manager@roy
extension_dir="$script_dir/$extension_id"
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
destination="$data_home/gnome-shell/extensions/$extension_id"

if ! command -v gsettings >/dev/null 2>&1 ||
    ! gsettings list-schemas | grep -qx 'org.gnome.shell'; then
    printf '%s\n' 'GNOME Shell settings are unavailable. GNOME Shell is required; Mutter alone cannot load GNOME Shell extensions.' >&2
    exit 1
fi

mkdir -p "$destination"
cp "$extension_dir/metadata.json" "$extension_dir/extension.js" "$destination/"

enabled_extensions=$(gsettings get org.gnome.shell enabled-extensions)
case "$enabled_extensions" in
    *"'$extension_id'"*)
        ;;
    *)
        case "$enabled_extensions" in
            '[]'|'@as []')
                enabled_extensions="['$extension_id']"
                ;;
            *)
                enabled_extensions=${enabled_extensions%]}
                enabled_extensions="$enabled_extensions, '$extension_id']"
                ;;
        esac
        gsettings set org.gnome.shell enabled-extensions "$enabled_extensions"
        ;;
esac

printf 'Installed and enabled %s for this user.\n' "$extension_id"
printf '%s\n' 'GNOME Shell must be running to load the extension; a Mutter compositor without GNOME Shell cannot run it.'
