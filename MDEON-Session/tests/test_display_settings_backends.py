import importlib.util
import io
import sys
import unittest
from contextlib import redirect_stdout
from importlib.machinery import SourceFileLoader
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "usr/bin"))
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_loader(
    "mdeon_display_settings_module",
    SourceFileLoader(
        "mdeon_display_settings_module",
        str(ROOT / "usr/bin/mdeon-display-settings"),
    ),
)
DISPLAY_MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DISPLAY_MODULE)


class DisplaySettingsBackendTests(unittest.TestCase):
    def test_mdeon_gnome_shell_session_remains_detectable_without_advertising_gnome(self):
        with patch.dict(
            "os.environ",
            {
                "XDG_CURRENT_DESKTOP": "MDEON",
                "DESKTOP_SESSION": "mdeon",
                "MDEON_SESSION_COMPOSITOR": "gnome-shell",
            },
            clear=True,
        ):
            self.assertTrue(DISPLAY_MODULE.is_gnome_session())

    def test_wlroots_prefers_wlr_randr_over_installed_kscreen(self):
        with (
            patch.dict(
                "os.environ",
                {
                    "XDG_CURRENT_DESKTOP": "MDEON",
                    "DESKTOP_SESSION": "mdeon",
                    "MDEON_SESSION_COMPOSITOR": "",
                },
                clear=True,
            ),
            patch.object(DISPLAY_MODULE, "is_wayland_session", return_value=True),
            patch.object(DISPLAY_MODULE.shutil, "which", return_value="/usr/bin/mock"),
            patch.object(
                DISPLAY_MODULE.HyprctlBackend,
                "list_outputs",
                side_effect=RuntimeError("not Hyprland"),
            ),
            patch.object(
                DISPLAY_MODULE.SwayBackend,
                "list_outputs",
                side_effect=RuntimeError("not Sway"),
            ),
            patch.object(
                DISPLAY_MODULE.KScreenBackend,
                "list_outputs",
                return_value=[object()],
            ),
            patch.object(
                DISPLAY_MODULE.GenericWlrBackend,
                "list_outputs",
                return_value=[object()],
            ),
        ):
            backend = DISPLAY_MODULE.get_backend()

        self.assertEqual(backend.name, "wlr-randr")

    def test_wlr_apply_preserves_advertised_refresh_precision(self):
        output = DISPLAY_MODULE.Output("eDP-1")
        output.enabled = True
        output.width = 1366
        output.height = 768
        output.refresh = 59.999001
        output.scale = 1.25

        with patch.object(
            DISPLAY_MODULE, "run_apply_command", return_value=(0, "", "")
        ) as apply_command:
            DISPLAY_MODULE.GenericWlrBackend().apply_outputs([output])

        command = apply_command.call_args.args[0]
        self.assertIn("--mode", command)
        self.assertIn("1366x768@59.999001Hz", command)

    def test_mutter_profile_is_saved_separately_from_other_backends(self):
        config = {
            "profiles": {"default": {"backend": "wlr-randr", "outputs": []}},
        }
        backend = SimpleNamespace(name="mutter")
        output = DISPLAY_MODULE.Output("eDP-1")
        output.scale = 1.5

        DISPLAY_MODULE.save_profile(config, "default", backend, [output])

        self.assertEqual(config["profiles"]["default"]["backend"], "wlr-randr")
        self.assertEqual(
            config["backend_profiles"]["mutter"]["default"]["outputs"][0]["scale"],
            1.5,
        )

    def test_mutter_can_still_apply_a_legacy_mutter_profile(self):
        config = {
            "profiles": {
                "default": {
                    "backend": "mutter",
                    "outputs": [{"name": "eDP-1", "scale": 1.5}],
                },
                "sway-layout": {
                    "backend": "sway",
                    "outputs": [{"name": "eDP-1", "scale": 2.0}],
                },
            },
        }

        profiles = DISPLAY_MODULE.profile_store(config, SimpleNamespace(name="mutter"))

        self.assertEqual(list(profiles), ["default"])
        self.assertEqual(profiles["default"]["outputs"][0]["scale"], 1.5)

    def test_mutter_apply_uses_its_own_saved_profile(self):
        config = {
            "profiles": {
                "default": {
                    "outputs": [{"name": "eDP-1", "scale": 1.0}],
                },
            },
            "backend_profiles": {
                "mutter": {
                    "default": {
                        "outputs": [{"name": "eDP-1", "scale": 1.75}],
                    },
                },
            },
        }
        backend = SimpleNamespace(name="mutter")
        backend.apply_outputs = unittest.mock.Mock()
        args = SimpleNamespace(
            list=False,
            current=False,
            list_profiles=False,
            apply=True,
            apply_profile=None,
            save_profile=None,
        )

        with (
            patch.object(DISPLAY_MODULE, "load_config", return_value=config),
            redirect_stdout(io.StringIO()),
        ):
            DISPLAY_MODULE.handle_cli_commands(args, backend)

        applied_outputs = backend.apply_outputs.call_args.args[0]
        self.assertEqual(applied_outputs[0].scale, 1.75)

    def test_mutter_apply_without_its_profile_does_not_apply_other_backend_profile(self):
        config = {
            "profiles": {
                "default": {
                    "outputs": [{"name": "eDP-1", "scale": 1.0}],
                },
            },
        }
        backend = SimpleNamespace(name="mutter")
        backend.apply_outputs = unittest.mock.Mock()
        args = SimpleNamespace(
            list=False,
            current=False,
            list_profiles=False,
            apply=True,
            apply_profile=None,
            save_profile=None,
        )
        output = io.StringIO()

        with (
            patch.object(DISPLAY_MODULE, "load_config", return_value=config),
            redirect_stdout(output),
        ):
            result = DISPLAY_MODULE.handle_cli_commands(args, backend)

        self.assertTrue(result)
        backend.apply_outputs.assert_not_called()
        self.assertIn("No saved Mutter display profile", output.getvalue())


if __name__ == "__main__":
    unittest.main()