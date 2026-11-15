#!/bin/bash
# /usr/bin/gen-wayland-menu.sh
# Generates the right-click menu for labwc, or launches the app drawer elsewhere

# 1. Check if we are running under labwc
if [ "$XDG_CURRENT_DESKTOP" = "labwc" ] || pgrep -x labwc > /dev/null; then
    
    MENU_DIR="$HOME/.config/labwc"
    MENU_FILE="$MENU_DIR/menu.xml"
    mkdir -p "$MENU_DIR"

    # If labwc-menu-generator is installed, use it! It's the standard tool.
    if command -v labwc-menu-generator >/dev/null; then
        labwc-menu-generator > "$MENU_FILE"
        # Tell labwc to reload its menu
        labwc -C reconfigure
        exit 0
    fi

    # Fallback: Generate a very basic labwc menu manually if the tool is missing
    cat > "$MENU_FILE" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu>
  <menu id="root-menu" label="Minimal Wayland">
    <item label="Terminal"><action name="Execute" command="x-terminal-emulator" /></item>
    <item label="Web Browser"><action name="Execute" command="xdg-open http://" /></item>
    <item label="File Manager"><action name="Execute" command="xdg-open ~/" /></item>
    <separator />
    <item label="Run Applications...">
      <action name="Execute" command="/usr/bin/wayland-menu.sh" />
    </item>
    <separator />
    <item label="Settings"><action name="Execute" command="/usr/bin/minimal-wayland-settings" /></item>
    <separator />
    <item label="Exit Wayland"><action name="Exit" /></item>
  </menu>
</openbox_menu>
EOF
    labwc -C reconfigure
    exit 0
fi

# 2. If we are NOT using labwc (e.g., Sway, Wayfire, Xminimal Desktop)
# Just directly launch the unified menu script.
exec /usr/bin/wayland-menu.sh
