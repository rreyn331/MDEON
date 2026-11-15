import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Clutter from 'gi://Clutter';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import { Extension } from 'resource:///org/gnome/shell/extensions/extension.js';

const BUS_NAME = 'org.mdeon.Panel.WindowManager';
const OBJECT_PATH = '/org/mdeon/Panel/WindowManager';
const INTERFACE_NAME = 'org.mdeon.Panel.WindowManager';
const REFRESH_SECONDS = 1;

const DBUS_XML = `<node>
  <interface name="${INTERFACE_NAME}">
    <method name="GetWindows">
      <arg name="windows_json" type="s" direction="out"/>
    </method>
    <method name="Activate">
      <arg name="xid" type="u" direction="in"/>
    </method>
    <method name="Minimize">
      <arg name="xid" type="u" direction="in"/>
    </method>
    <method name="Maximize">
      <arg name="xid" type="u" direction="in"/>
    </method>
    <method name="Close">
      <arg name="xid" type="u" direction="in"/>
    </method>
        <method name="GetWorkspaces">
            <arg name="current_index" type="i" direction="out"/>
            <arg name="workspace_count" type="i" direction="out"/>
        </method>
        <method name="SwitchWorkspace">
            <arg name="index" type="i" direction="in"/>
            <arg name="switched" type="b" direction="out"/>
        </method>
        <method name="GetShowingDesktop">
            <arg name="showing" type="b" direction="out"/>
        </method>
        <method name="SetShowingDesktop">
            <arg name="showing" type="b" direction="in"/>
            <arg name="changed" type="b" direction="out"/>
        </method>
        <method name="BeginWindowKill">
            <arg name="started" type="b" direction="out"/>
        </method>
        <method name="GetWindowKillActive">
            <arg name="active" type="b" direction="out"/>
        </method>
    <signal name="WindowsChanged"/>
  </interface>
</node>`;

export default class MdeonPanelWindowManagerExtension extends Extension {
    enable() {
        this._shellPanel = Main.panel?.actor ?? Main.panel ?? null;
        this._shellPanelWasVisible = this._shellPanel?.visible ?? false;
        this._shellPanel?.hide();
        this._showingDesktop = false;
        this._showDesktopWindowIds = [];
        this._nativePanelActor = null;
        this._nativePanelWindow = null;
        this._nativePanelWindowState = null;
        this._windowKillActor = null;
        this._windowKillGrab = null;
        this._windowKillTimeout = 0;
        this._exported = Gio.DBusExportedObject.wrapJSObject(DBUS_XML, this);
        this._exported.export(Gio.DBus.session, OBJECT_PATH);
        this._ownerId = Gio.bus_own_name(
            Gio.BusType.SESSION,
            BUS_NAME,
            Gio.BusNameOwnerFlags.NONE,
            null,
            null,
            null,
        );
        this._lastSnapshot = this._snapshot();
        this._syncNativePanel();
        this._refreshId = GLib.timeout_add_seconds(
            GLib.PRIORITY_DEFAULT,
            REFRESH_SECONDS,
            () => {
                this._syncNativePanel();
                const snapshot = this._snapshot();
                if (snapshot !== this._lastSnapshot) {
                    this._lastSnapshot = snapshot;
                    this._exported.emit_signal('WindowsChanged', null);
                }
                return GLib.SOURCE_CONTINUE;
            },
        );
    }

    disable() {
        this._stopWindowKillMode();
        this._removeNativePanelChrome();
        if (this._shellPanelWasVisible)
            this._shellPanel?.show();
        this._shellPanel = null;
        this._shellPanelWasVisible = undefined;
        if (this._refreshId !== undefined) {
            GLib.Source.remove(this._refreshId);
            this._refreshId = undefined;
        }
        if (this._ownerId !== undefined) {
            Gio.bus_unown_name(this._ownerId);
            this._ownerId = undefined;
        }
        if (this._exported !== undefined) {
            this._exported.unexport();
            this._exported = undefined;
        }
        this._lastSnapshot = undefined;
    }

    _findNativePanelWindow() {
        return global.display.get_tab_list(Meta.TabList.NORMAL_ALL, null)
            .find(window => {
                if (window.get_client_type() !== Meta.WindowClientType.WAYLAND)
                    return false;
                return this._isMdeonPanelWindow(window);
            }) ?? null;
    }

    _isMdeonPanelWindow(window) {
        const appId = Shell.WindowTracker.get_default().get_window_app(window)?.get_id() ||
            window.get_gtk_application_id?.() || '';
        return appId === 'org.mdeon.Panel' || window.get_title() === 'mdeon-panel';
    }

    _windowIsUsable(window) {
        if (window === null || window === undefined)
            return false;
        try {
            return global.display.get_tab_list(Meta.TabList.NORMAL_ALL, null)
                .includes(window);
        } catch (error) {
            console.warn(`[mdeon-panel-window-manager] window check failed: ${error}`);
            return false;
        }
    }

    _safeWindowOperation(window, action, callback) {
        if (!this._windowIsUsable(window))
            return false;
        try {
            callback();
            return true;
        } catch (error) {
            console.warn(`[mdeon-panel-window-manager] ${action} failed: ${error}`);
            return false;
        }
    }

    _syncNativePanel() {
        const window = this._findNativePanelWindow();
        if (window === null) {
            this._removeNativePanelChrome();
            return;
        }

        if (this._nativePanelWindow !== window) {
            this._removeNativePanelChrome();
            this._nativePanelWindow = window;
        }

        if (!this._windowIsUsable(window)) {
            this._nativePanelWindow = null;
            this._nativePanelWindowState = null;
            return;
        }

        try {
            const monitor = Main.layoutManager.monitors[window.get_monitor()];
            if (monitor === undefined)
                return;

            const frame = window.get_frame_rect();
            const panelHeight = Math.max(1, frame.height || 30);
            const panelX = monitor.x;
            const panelY = monitor.y + monitor.height - panelHeight;

            if (this._nativePanelWindowState === null) {
                this._nativePanelWindowState = {
                    x: frame.x,
                    y: frame.y,
                    width: frame.width,
                    height: frame.height,
                    wasAbove: window.is_above(),
                    wasSticky: window.is_on_all_workspaces(),
                };
            }

            this._safeWindowOperation(window, 'make_above/stick', () => {
                if (!window.is_above())
                    window.make_above();
                if (!window.is_on_all_workspaces())
                    window.stick();
            });

            if (frame.x !== panelX || frame.y !== panelY ||
                frame.width !== monitor.width || frame.height !== panelHeight) {
                this._safeWindowOperation(window, 'move_resize_frame', () => {
                    window.move_resize_frame(true, panelX, panelY, monitor.width, panelHeight);
                });
            }

            if (this._nativePanelActor === null) {
                this._nativePanelActor = new St.Widget({
                    reactive: false,
                    can_focus: false,
                    track_hover: false,
                    opacity: 0,
                });
                try {
                    Main.layoutManager.addChrome(this._nativePanelActor, {
                        affectsStruts: false,
                        trackFullscreen: true,
                    });
                } catch (error) {
                    console.warn(`[mdeon-panel-window-manager] addChrome failed: ${error}`);
                    this._nativePanelActor.destroy();
                    this._nativePanelActor = null;
                    return;
                }
            }
            this._nativePanelActor.set_position(
                monitor.x, monitor.y + monitor.height - panelHeight);
            this._nativePanelActor.set_size(monitor.width, panelHeight);
        } catch (error) {
            console.warn(`[mdeon-panel-window-manager] syncNativePanel failed: ${error}`);
            this._removeNativePanelChrome();
        }
    }

    _removeNativePanelChrome() {
        if (this._nativePanelActor !== null) {
            try {
                Main.layoutManager.removeChrome(this._nativePanelActor);
            } catch (error) {
                console.warn(`[mdeon-panel-window-manager] removeChrome failed: ${error}`);
            }
            this._nativePanelActor.destroy();
            this._nativePanelActor = null;
        }
        const managedWindow = this._nativePanelWindow;
        const windowIsManaged = managedWindow !== null && this._windowIsUsable(managedWindow);
        if (windowIsManaged && this._nativePanelWindowState !== null) {
            const state = this._nativePanelWindowState;
            try {
                if (!state.wasAbove)
                    managedWindow.unmake_above();
                if (!state.wasSticky)
                    managedWindow.unstick();
                managedWindow.move_resize_frame(true,
                    state.x, state.y,
                    state.width, state.height);
            } catch (error) {
                console.warn(`[mdeon-panel-window-manager] restore panel window failed: ${error}`);
            }
        }
        this._nativePanelWindow = null;
        this._nativePanelWindowState = null;
    }

    GetWindows() {
        return this._snapshot();
    }

    Activate(xid) {
        const window = this._findWindow(xid);
        if (window === null)
            return;
        window.unminimize();
        window.activate(global.get_current_time());
    }

    Minimize(xid) {
        this._findWindow(xid)?.minimize();
    }

    Maximize(xid) {
        const window = this._findWindow(xid);
        if (window === null)
            return;
        if (this._isMaximized(window))
            window.unmaximize(Meta.MaximizeFlags.BOTH);
        else
            window.maximize(Meta.MaximizeFlags.BOTH);
    }

    Close(xid) {
        this._findWindow(xid)?.delete(global.get_current_time());
    }

    GetWorkspaces() {
        const manager = global.workspace_manager;
        return [manager.get_active_workspace_index(), manager.n_workspaces];
    }

    SwitchWorkspace(index) {
        const workspace = global.workspace_manager.get_workspace_by_index(index);
        if (workspace === null)
            return false;
        workspace.activate(global.get_current_time());
        return true;
    }

    GetShowingDesktop() {
        return this._showingDesktop;
    }

    SetShowingDesktop(showing) {
        if (showing === this._showingDesktop)
            return true;
        if (showing) {
            this._showDesktopWindowIds = this._listWindows()
                .filter(window => !window.minimized)
                .map(window => window.get_id());
            for (const id of this._showDesktopWindowIds)
                this._findWindow(id)?.minimize();
        } else {
            for (const id of this._showDesktopWindowIds) {
                const window = this._findWindow(id);
                if (window?.minimized)
                    window.unminimize();
            }
            this._showDesktopWindowIds = [];
        }
        this._showingDesktop = showing;
        return true;
    }

    BeginWindowKill() {
        this._stopWindowKillMode();
        const actor = new St.Widget({
            reactive: true,
            can_focus: true,
            track_hover: false,
        });
        actor.set_position(0, 0);
        actor.set_size(global.stage.width, global.stage.height);
        actor.connect('key-press-event', (_actor, event) => {
            if (event.get_key_symbol() === Clutter.KEY_Escape)
                this._stopWindowKillMode();
            return Clutter.EVENT_STOP;
        });
        actor.connect('button-press-event', (_actor, event) => {
            const [x, y] = event.get_coords();
            const window = global.display.get_tab_list(Meta.TabList.NORMAL_ALL, null)
                .find(candidate => {
                    if (!this._windowIsUsable(candidate) ||
                        this._isMdeonPanelWindow(candidate) || candidate.is_skip_taskbar()) {
                        return false;
                    }
                    const frame = candidate.get_frame_rect();
                    return x >= frame.x && x < frame.x + frame.width &&
                        y >= frame.y && y < frame.y + frame.height;
                });
            this._stopWindowKillMode();
            if (window !== undefined) {
                this._safeWindowOperation(window, 'close selected window', () => {
                    window.delete(global.get_current_time());
                });
            }
            return Clutter.EVENT_STOP;
        });
        Main.layoutManager.addChrome(actor, {
            affectsStruts: false,
            trackFullscreen: true,
        });
        const grab = Main.pushModal(actor);
        if (!grab) {
            Main.layoutManager.removeChrome(actor);
            actor.destroy();
            return false;
        }
        this._windowKillActor = actor;
        this._windowKillGrab = grab;
        actor.grab_key_focus();
        try {
            global.display.set_cursor(Meta.Cursor.CROSSHAIR);
        } catch (error) {
            console.warn(`[mdeon-panel-window-manager] crosshair cursor unavailable: ${error}`);
        }
        this._windowKillTimeout = GLib.timeout_add_seconds(
            GLib.PRIORITY_DEFAULT, 30, () => {
                this._windowKillTimeout = 0;
                this._stopWindowKillMode();
                return GLib.SOURCE_REMOVE;
            });
        return true;
    }

    GetWindowKillActive() {
        return this._windowKillActor !== null;
    }

    _stopWindowKillMode() {
        if (this._windowKillActor === null)
            return;
        const actor = this._windowKillActor;
        const grab = this._windowKillGrab;
        this._windowKillActor = null;
        this._windowKillGrab = null;
        if (this._windowKillTimeout !== 0) {
            GLib.Source.remove(this._windowKillTimeout);
            this._windowKillTimeout = 0;
        }
        try {
            if (grab !== null)
                Main.popModal(grab);
        } catch (error) {
            console.warn(`[mdeon-panel-window-manager] popModal failed: ${error}`);
        } finally {
            try {
                global.display.set_cursor(Meta.Cursor.DEFAULT);
            } catch (error) {
                console.warn(`[mdeon-panel-window-manager] default cursor restore failed: ${error}`);
            }
            Main.layoutManager.removeChrome(actor);
            actor.destroy();
        }
    }

    _findWindow(xid) {
        const id = Number(xid);
        return this._listWindows().find(window => window.get_id() === id) ?? null;
    }

    _listWindows() {
        return global.display.get_tab_list(Meta.TabList.NORMAL_ALL, null)
            .filter(window => !this._isMdeonPanelWindow(window))
            .filter(window => !window.is_skip_taskbar())
            .filter(window => window.get_id() > 0);
    }

    _isMaximized(window) {
        return window.maximized_horizontally && window.maximized_vertically;
    }

    _snapshot() {
        const focused = global.display.focus_window;
        const tracker = Shell.WindowTracker.get_default();
        return JSON.stringify(this._listWindows().map(window => {
            const app = tracker.get_window_app(window);
            return {
                xid: String(window.get_id()),
                title: window.get_title() || 'Window',
                app_id: app?.get_id() || window.get_gtk_application_id?.() ||
                    window.get_wm_class_instance?.() || window.get_wm_class?.() || '',
                active: window === focused,
                iconified: window.minimized,
                maximized: this._isMaximized(window),
            };
        }));
    }
}
