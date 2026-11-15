# mdeon-panel

A native implementation of the mdeon-panel runtime and configuration editor,
with X11 and Wayland backends. The reference project is not modified.

## Run Without Installing

Install the build dependencies listed in [DEPENDENCIES-cairo.md](DEPENDENCIES-cairo.md), then run:

```sh
./run-cairo.sh
./scripts/run-config-editor.sh
./scripts/test-config-editor-cairo.sh
./test-cairo.sh
```

`./run.sh` and `./run-cairo.sh` both build and launch the native panel.
`./scripts/run-config-editor.sh` builds and launches its native X11
configuration editor. The panel and editor use the same `mdeon-panel` config
file. Its initial user config is
copied to `${XDG_CONFIG_HOME:-$HOME/.config}/mdeon-panel/config.conf`;
the editor and panel use that same file. Set `MDEON_PANEL_CONFIG` or
`MDEON_PANEL_CONFIG_PATH` to select another config. `MDEON_PANEL_BACKEND=x11`
or `MDEON_PANEL_BACKEND=wayland` selects a backend explicitly. GNOME Wayland
uses the included Shell bridge for task/workspace actions; KWin Wayland uses
its D-Bus and scripting interfaces.

Set `panel_position` to `top center horizontal`, `bottom center horizontal`,
`left center vertical`, or `right center vertical` to place the panel on an
edge. Side panels stack launcher and tray items, use compact task icons and a
single-line clock, and open menus toward the screen interior. Wayland edge
placement requires a compositor that supports layer-shell.

`panel_monitor` selects `all` (the default), `primary`, or a connected output
name such as `DP-1`. On X11, output names and geometry come from `xrandr`; on
Wayland, output names come from `wl_output` and per-output placement requires
layer-shell. Wayland's core output protocol has no primary-output flag, so
`primary` selects the first advertised output there. With `panel_monitor = all`,
the panel supervises one output-bound process per active display and polls for
topology changes every two seconds. All those processes use the same config.
For differently configured panels, stop the automatic `all` instance, then
launch one process per output with its own config selected through
`MDEON_PANEL_CONFIG_PATH` and `panel_monitor` set to that output name.
`task_monitor_filter = 1` filters task buttons to that output where the window
backend reports output membership (X11 and the Wayland foreign-toplevel
provider). Other Wayland task providers currently show all windows.

For GNOME Wayland task and workspace controls, install and enable the included
user bridge with `sh extensions/gnome/install.sh`. It updates GNOME Shell's
enabled-extension setting directly, so the GNOME Extensions command-line tool
and the full GNOME desktop are not required. GNOME Shell is still required to
load the extension; standalone Mutter cannot run GNOME Shell extensions. From
an installed package, run
`/usr/share/gnome-shell/extensions/install-mdeon-panel-gnome.sh`.

## Debian Package

Build a binary Debian package with:

```sh
./scripts/build-deb.sh
```

The Debian package installs the native `mdeon-panel` and
`mdeon-panel-config` executables and the GNOME Shell bridge.

## Migration Status

The native runtime currently includes the menu, desktop hub, show-desktop,
virtual desktop, custom button, launcher, executor, spacer, task list, tray,
notifications, volume, network, Bluetooth, battery, and clock modules. The
original `J` file-search plugin source is preserved, but that module is not
yet ported to the native runtime. The editor retains all original configuration
fields; settings beyond those currently consumed remain available for future
porting.
