# Dependencies

## Debian and Ubuntu

Install the compiler, build tools, and development headers with:

```sh
sudo apt update
sudo apt install build-essential cmake pkg-config libcairo2-dev libpango1.0-dev libxcb1-dev libx11-dev libglib2.0-dev libgdk-pixbuf-2.0-dev librsvg2-common libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev
```

Requirements:

- GCC/G++ or another C++20-capable compiler (`build-essential` provides GCC/G++)
- CMake 3.22 or newer
- `pkg-config`
- Cairo with the `cairo-xcb` development module (`libcairo2-dev`)
- PangoCairo development files (`libpango1.0-dev`)
- XCB development files (`libxcb1-dev`)
- X11 development files (`libx11-dev`) for the Cairo configuration editor
- GLib/GIO development files (`libglib2.0-dev`) for the main loop and D-Bus
- GdkPixbuf (`libgdk-pixbuf-2.0-dev`) and its SVG loader (`librsvg2-common`) for theme icons

Those development packages also install the shared libraries needed to run the
program. Wayland support is optional; install `libwayland-dev`, `libwayland-bin`,
`wayland-protocols`, and `libxkbcommon-dev` to build the direct Wayland backend.

Runtime use requires an active X11 session or a Wayland compositor supporting
xdg-shell. Wayland layer-shell support is optional and provides panel anchoring
and reserved space; without it the panel uses a regular xdg-toplevel window.
On GNOME Wayland, the launcher prefers XWayland/EWMH when `DISPLAY` and `wmctrl`
are available; `SIMPANEL_BACKEND=wayland` forces the non-reserving xdg-toplevel
fallback.
On X11, an EWMH-compatible window manager is needed for dock and reserved-space
properties to take effect.

## Verify package discovery

```sh
pkg-config --modversion cairo-xcb pangocairo xcb gio-2.0 gdk-pixbuf-2.0
pkg-config --modversion wayland-client wayland-protocols xkbcommon
```

Network, volume, Bluetooth, show-desktop, task-list, and workspace controls may
use optional runtime utilities. `wmctrl` provides X11 workspace control;
Wayland task lists use the GNOME D-Bus bridge or the KWin scripting API on
Plasma 6, then `wlrctl`, Sway/i3 IPC, or the compositor's wlroots
foreign-toplevel protocol. JSON-C enables the GNOME/KWin/Sway parsers; the
foreign-toplevel client is generated when the Wayland development dependencies
are present.
`swaymsg` and `hyprctl` provide workspace control for Sway and Hyprland;
KDE/Plasma 6 uses KWin's D-Bus interfaces for workspace control and
show-desktop. Labwc workspace controls use its `rc.xml` desktop count and
`wtype`. Wayland show-desktop can also use GNOME's D-Bus API, `wlrctl`,
`ydotool`, or `wtype` depending on the compositor.
`nmcli`, `pactl`/`wpctl`/`amixer`, and `bluetoothctl` enable their respective
system controls. Unsupported task-list or workspace modules are omitted for
the active backend.