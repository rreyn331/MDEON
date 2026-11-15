import importlib.util
import sys
import subprocess
import tempfile
import unittest
from pathlib import Path
from importlib.machinery import SourceFileLoader
from unittest.mock import MagicMock, patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "usr/bin"))
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_loader(
    "mdeon_desktop_module",
    SourceFileLoader("mdeon_desktop_module", str(ROOT / "usr/bin/mdeon-desktop")),
)
DESKTOP_MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DESKTOP_MODULE)


class DesktopMonitorLayoutTests(unittest.TestCase):
    def test_session_desktops_use_the_installed_theme_icon(self):
        for size in (16, 24, 32, 48, 64, 128, 256, 512):
            icon_path = ROOT / f"usr/share/icons/hicolor/{size}x{size}/apps/mdeon.png"
            with self.subTest(icon_size=size):
                self.assertTrue(icon_path.is_file())

        for symbolic_name in ("mdeon-symbolic.svg", "mdeon-badge-symbolic.svg"):
            symbolic_path = ROOT / "usr/share/icons/hicolor/scalable/apps" / symbolic_name
            with self.subTest(symbolic_icon=symbolic_name):
                self.assertTrue(symbolic_path.is_file())

        for session_path in (
            ROOT / "usr/share/xsessions/mdeon-x11.desktop",
            ROOT / "usr/share/wayland-sessions/mdeon-wayland.desktop",
        ):
            with self.subTest(session=session_path.name):
                desktop_entry = session_path.read_text(encoding="utf-8")
                self.assertIn("\nIcon=mdeon\n", desktop_entry)
                badge_path = (
                    ROOT / "usr/share/slick-greeter/badges" / f"{session_path.stem}.png"
                )
                self.assertTrue(badge_path.is_file())

    def make_desktop(self, layout, items):
        desktop = object.__new__(DESKTOP_MODULE.MdeonCoreDesktop)
        desktop.width = 3000
        desktop.height = 1200
        desktop.layout_config = layout
        desktop.wallpaper_path = "/wallpaper/default.jpg"
        desktop.items = items
        desktop.cell_w = 100
        desktop.cell_h = 100
        desktop.modal_title = ""
        desktop._load_wallpaper_surface = lambda path, _width, _height: path
        desktop.show_default_monitor_recovery = lambda: setattr(
            desktop, "recovery_shown", True
        )
        return desktop

    def test_config_backup_rotation_keeps_two_previous_versions(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            config_path = Path(temp_dir) / "desktop-layout.json"
            for version in ("one", "two", "three"):
                config_path.write_text(version, encoding="utf-8")
                DESKTOP_MODULE.backup_config_file(config_path)

            self.assertEqual(config_path.with_name("desktop-layout.json.bak.1").read_text(), "three")
            self.assertEqual(config_path.with_name("desktop-layout.json.bak.2").read_text(), "two")
            self.assertFalse(config_path.with_name("desktop-layout.json.bak.3").exists())

    def test_wallpaper_chooser_starts_in_current_folder_and_keeps_wayland_heartbeat(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            current_wallpaper = Path(temp_dir) / "current.jpg"
            selected_wallpaper = Path(temp_dir) / "selected.jpg"
            current_wallpaper.touch()
            selected_wallpaper.touch()
            desktop = self.make_desktop({}, [])
            desktop.wallpaper_path = str(current_wallpaper)
            process = MagicMock()
            process.__enter__.return_value = process
            process.communicate.side_effect = [
                subprocess.TimeoutExpired("zenity", 0.5),
                (str(selected_wallpaper), ""),
            ]
            process.returncode = 0

            with (
                patch.object(
                    DESKTOP_MODULE.shutil,
                    "which",
                    side_effect=lambda command: "/usr/bin/zenity" if command == "zenity" else None,
                ),
                patch.object(DESKTOP_MODULE.subprocess, "Popen", return_value=process) as popen,
                patch.object(DESKTOP_MODULE, "update_watchdog_heartbeat") as heartbeat,
            ):
                wallpaper = desktop.choose_wallpaper(
                    "Select Default Wallpaper",
                    desktop.wallpaper_path,
                )

        self.assertEqual(wallpaper, str(selected_wallpaper))
        self.assertIn(f"--filename={current_wallpaper.parent}{'/'}", popen.call_args.args[0])
        heartbeat.assert_called_once_with()

    def test_set_default_wallpaper_saves_the_chosen_path(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            config_dir = Path(temp_dir) / "config"
            config_path = config_dir / "mdeon.conf"
            default_path = Path(temp_dir) / "mdeon.conf.default"
            selected_wallpaper = Path(temp_dir) / "selected.jpg"
            default_path.write_text("# defaults\n", encoding="utf-8")
            selected_wallpaper.touch()
            desktop = self.make_desktop({}, [])
            desktop.config = {}
            desktop.choose_wallpaper = lambda _title, _initial_path: str(selected_wallpaper)
            desktop.update_monitor_topology = lambda: None
            desktop.redraw = lambda: None

            with (
                patch.object(DESKTOP_MODULE, "CONFIG_DIR", config_dir),
                patch.object(DESKTOP_MODULE, "MDEON_CONF", config_path),
                patch.object(DESKTOP_MODULE, "MDEON_DFLT", default_path),
                patch.object(DESKTOP_MODULE, "read_mdeon_conf", return_value={}),
            ):
                desktop.set_default_wallpaper()

            self.assertIn(f'WALLPAPER="{selected_wallpaper}"', config_path.read_text(encoding="utf-8"))
            self.assertEqual(desktop.wallpaper_path, str(selected_wallpaper))

    def test_single_monitor_uses_default_without_mutating_saved_layout(self):
        saved_layout = {
            "default_monitor": "HDMI-1",
            "wallpapers": {"eDP-1": "/wallpaper/custom.jpg"},
            "icon_assignments": {
                "file:/home/user/Desktop/notes.txt": {
                    "monitor": "HDMI-1",
                    "col": 2,
                    "row": 1,
                }
            },
        }
        desktop = self.make_desktop(
            saved_layout,
            [{"layout_id": "file:/home/user/Desktop/notes.txt"}],
        )
        monitors = [{
            "name": "eDP-1",
            "x": 0,
            "y": 0,
            "width": 1366,
            "height": 768,
            "scale": 1.0,
            "primary": True,
        }]

        with patch.object(DESKTOP_MODULE, "get_active_monitors", return_value=monitors):
            desktop.update_monitor_topology()

        self.assertEqual(desktop.active_default_monitor, "eDP-1")
        self.assertEqual(desktop.monitor_wallpapers["eDP-1"], "/wallpaper/default.jpg")
        self.assertEqual(desktop.items[0]["effective_monitor"], "eDP-1")
        self.assertEqual(desktop.layout_config, saved_layout)
        self.assertFalse(desktop.default_monitor_missing)

    def test_missing_default_prompts_and_preserves_monitor_wallpapers(self):
        desktop = self.make_desktop(
            {
                "default_monitor": "HDMI-1",
                "wallpapers": {"DP-1": "/wallpaper/dp.jpg"},
                "icon_assignments": {
                    "file:/home/user/Desktop/notes.txt": {
                        "monitor": "HDMI-1",
                        "col": 2,
                        "row": 1,
                    }
                },
            },
            [{"layout_id": "file:/home/user/Desktop/notes.txt"}],
        )
        monitors = [
            {
                "name": "eDP-1",
                "x": 0,
                "y": 0,
                "width": 1366,
                "height": 768,
                "scale": 1.0,
                "primary": True,
            },
            {
                "name": "DP-1",
                "x": 1366,
                "y": 0,
                "width": 1920,
                "height": 1080,
                "scale": 1.0,
                "primary": False,
            },
        ]

        with patch.object(DESKTOP_MODULE, "get_active_monitors", return_value=monitors):
            desktop.update_monitor_topology()

        self.assertTrue(desktop.default_monitor_missing)
        self.assertTrue(desktop.recovery_shown)
        self.assertEqual(desktop.active_default_monitor, None)
        self.assertEqual(desktop.monitor_wallpapers["DP-1"], "/wallpaper/dp.jpg")
        self.assertEqual(desktop.items[0]["effective_monitor"], "eDP-1")
        self.assertEqual(desktop.layout_config["default_monitor"], "HDMI-1")

    def test_dragging_selected_icons_persists_target_monitor_grid(self):
        desktop = self.make_desktop(
            {
                "default_monitor": "eDP-1",
                "wallpapers": {},
                "icon_assignments": {},
            },
            [
                {
                    "layout_id": "file:/home/user/Desktop/one.txt",
                    "x": 20,
                    "y": 20,
                    "col": 0,
                    "row": 0,
                    "effective_monitor": "eDP-1",
                },
                {
                    "layout_id": "file:/home/user/Desktop/two.txt",
                    "x": 20,
                    "y": 120,
                    "col": 0,
                    "row": 1,
                    "effective_monitor": "eDP-1",
                },
            ],
        )
        desktop.monitors = [
            {"name": "eDP-1", "x": 0, "y": 0, "width": 1000, "height": 800},
            {"name": "DP-1", "x": 1000, "y": 0, "width": 1000, "height": 800},
        ]
        desktop.active_default_monitor = "eDP-1"
        desktop.selected_idx = -1
        desktop.selected_indices = set()
        desktop.last_click_time = 0
        desktop.drag_candidate = None
        desktop.dragging = False
        desktop.drag_start = (0, 0)
        desktop.drag_item_origins = {}
        desktop.rubberbanding = False
        desktop.rubberband_rect = (0, 0, 0, 0)
        desktop.modal_active = False
        desktop.menu_visible = False
        desktop.submenu_visible = False
        desktop.redraw = lambda: None

        desktop.handle_click(30, 30, 1)
        desktop.handle_release(30, 30, 1)
        desktop.handle_click(30, 130, 1, state=4)
        desktop.handle_release(30, 130, 1)
        desktop.handle_click(30, 30, 1)
        desktop.handle_motion(1130, 230)
        desktop.handle_release(1130, 230, 1)

        assignments = desktop.layout_config["icon_assignments"]
        self.assertEqual(assignments[desktop.items[0]["layout_id"]]["monitor"], "DP-1")
        self.assertEqual(assignments[desktop.items[1]["layout_id"]]["monitor"], "DP-1")


if __name__ == "__main__":
    unittest.main()