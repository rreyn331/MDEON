import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "usr/bin/mdeon-gnome-shell"


class GnomeShellLauncherTests(unittest.TestCase):
    def test_enables_bridge_and_forwards_shell_arguments(self):
        self.assertTrue(os.access(LAUNCHER, os.X_OK))
        mode_path = ROOT / "usr/share/gnome-shell/modes/mdeon.json"
        mode = json.loads(mode_path.read_text(encoding="utf-8"))
        self.assertEqual(mode["parentMode"], None)
        self.assertFalse(mode["hasOverview"])
        self.assertEqual(mode["enabledExtensions"], ["mdeon-panel-window-manager@roy"])
        self.assertEqual(mode["components"], [])
        self.assertTrue(mode["hasWindows"])
        self.assertTrue(mode["hasWorkspaces"])
        self.assertEqual(mode["panel"], {"left": [], "center": [], "right": []})
        metadata_path = (
            ROOT.parent
            / "mdeon-panel/extensions/gnome/mdeon-panel-window-manager@roy/metadata.json"
        )
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        self.assertIn("mdeon", metadata["session-modes"])
        self.assertIn("51", metadata["shell-version"])

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            settings_log = temp_path / "gsettings.log"
            shell_log = temp_path / "shell.log"
            gsettings = temp_path / "gsettings"
            gsettings.write_text(
                "#!/bin/sh\n"
                "case \"$1\" in\n"
                "  list-schemas) printf '%s\\n' org.gnome.shell ;;\n"
                "  get) printf \"%s\\n\" \"['user-extension@example']\" ;;\n"
                "  set) printf '%s\\n' \"$*\" >> \"$MDEON_TEST_SETTINGS_LOG\" ;;\n"
                "esac\n",
                encoding="utf-8",
            )
            shell = temp_path / "gnome-shell"
            shell.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" > \"$MDEON_TEST_SHELL_LOG\"\n"
                "printf '%s\\n' \"$XDG_SESSION_TYPE|$XDG_CURRENT_DESKTOP|$XDG_SESSION_DESKTOP|$MDEON_SESSION_COMPOSITOR\" >> \"$MDEON_TEST_SHELL_LOG\"\n",
                encoding="utf-8",
            )
            gsettings.chmod(0o755)
            shell.chmod(0o755)

            env = os.environ.copy()
            env.update({
                "PATH": f"{temp_path}:{env['PATH']}",
                "MDEON_TEST_SETTINGS_LOG": str(settings_log),
                "MDEON_TEST_SHELL_LOG": str(shell_log),
            })
            subprocess.run(
                [str(LAUNCHER), "--test-argument"],
                check=True,
                env=env,
                capture_output=True,
                text=True,
            )

            self.assertIn(
                "set org.gnome.shell enabled-extensions ['user-extension@example', "
                "'mdeon-panel-window-manager@roy']",
                settings_log.read_text(encoding="utf-8"),
            )
            self.assertEqual(
                shell_log.read_text(encoding="utf-8").splitlines(),
                [
                    "--wayland --mode=mdeon --test-argument",
                    "wayland|MDEON|MDEON|gnome-shell",
                ],
            )


if __name__ == "__main__":
    unittest.main()