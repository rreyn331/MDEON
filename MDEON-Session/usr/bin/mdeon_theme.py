import subprocess


def prefers_light_scheme():
    try:
        result = subprocess.run(
            ["gsettings", "get", "org.gnome.desktop.interface", "color-scheme"],
            capture_output=True,
            text=True,
            timeout=0.5,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return result.returncode == 0 and result.stdout.strip().strip("'\"") == "prefer-light"


class ThemedContext:
    def __init__(self, context, light_mode):
        self._context = context
        self._light_mode = light_mode

    def __getattr__(self, name):
        return getattr(self._context, name)

    def _map_color(self, red, green, blue):
        if not self._light_mode or max(red, green, blue) - min(red, green, blue) > 0.06:
            return red, green, blue
        shade = max(0.06, min(0.94, 0.98 - ((red + green + blue) / 3) * 0.9))
        return shade, shade, shade

    def set_source_rgb(self, red, green, blue):
        self._context.set_source_rgb(*self._map_color(red, green, blue))

    def set_source_rgba(self, red, green, blue, alpha):
        self._context.set_source_rgba(*self._map_color(red, green, blue), alpha)

    def set_source_rgb_raw(self, red, green, blue):
        self._context.set_source_rgb(red, green, blue)

    def set_source_rgba_raw(self, red, green, blue, alpha):
        self._context.set_source_rgba(red, green, blue, alpha)