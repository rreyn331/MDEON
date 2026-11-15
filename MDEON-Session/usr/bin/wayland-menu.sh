#!/bin/bash
# /usr/bin/wayland-menu.sh
# Universal Application Menu Launcher for Minimal Wayland

# Check if rofi (or rofi-wayland) is available in the system PATH
if command -v rofi >/dev/null 2>&1; then
    exec rofi -show drun -theme-str 'window {width: 50%;} listview {lines: 10;} container {padding: 10px;}'

# If rofi is missing, check for the specific custom tmenu binary
elif [ -x /usr/bin/tmenu ]; then
    exec /usr/bin/tmenu

# Failsafe Wayland Alternatives (Just in case you uninstall rofi/tmenu)
elif command -v wofi >/dev/null 2>&1; then
    exec wofi --show drun -I
elif command -v fuzzel >/dev/null 2>&1; then
    exec fuzzel --background-color=222222dd --text-color=ffffffff --selection-color=444444ff

# Final fallback error message
else
    # Send a graphical notification to the desktop so you actually see the error
    if command -v notify-send >/dev/null 2>&1; then
        notify-send "Menu Error" "Neither 'rofi' nor '/usr/bin/tmenu' was found."
    fi
    echo "Error: Neither 'rofi' nor '/usr/bin/tmenu' was found."
    exit 1
fi
