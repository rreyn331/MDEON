"""Host a MDEON Cairo-rendered application in a native GTK4 window."""

import sys


def run_cairo_gtk(app_class, title, width, height, min_width, min_height):
    try:
        import gi

        gi.require_version("Gdk", "4.0")
        gi.require_version("Gtk", "4.0")
        from gi.repository import Gdk, Gtk
    except (ImportError, ValueError) as exc:
        raise RuntimeError("GTK4 Python bindings are required for the Wayland UI.") from exc

    app_id_suffix = ".".join(part.lower() for part in title.split())
    application = Gtk.Application(application_id=f"org.mdeon.{app_id_suffix}")
    windows = []

    def on_activate(app):
        window = Gtk.ApplicationWindow(application=app)
        windows.append(window)
        window.set_title(title)
        window.set_default_size(width, height)
        window.set_resizable(True)

        area = Gtk.DrawingArea()
        area.set_content_width(width)
        area.set_content_height(height)
        area.set_size_request(min_width, min_height)
        area.set_focusable(True)
        state = {"app": None, "drawing": False, "pointer": (0, 0)}

        def request_draw():
            if not state["drawing"]:
                area.queue_draw()

        def draw(_area, context, draw_width, draw_height):
            cairo_app = state["app"]
            if cairo_app is None:
                return
            if (cairo_app.width, cairo_app.height) != (draw_width, draw_height):
                cairo_app.width = draw_width
                cairo_app.height = draw_height
                cairo_app._init_cairo()
            state["drawing"] = True
            try:
                cairo_app.redraw()
                context.set_source_surface(cairo_app.surface, 0, 0)
                context.paint()
            finally:
                state["drawing"] = False

        area.set_draw_func(draw)
        cairo_app = app_class(width, height, request_draw)
        state["app"] = cairo_app

        click = Gtk.GestureClick.new()

        def on_pressed(gesture, _count, x, y):
            button = gesture.get_current_button()
            state["pointer"] = (x, y)
            area.grab_focus()
            cairo_app.handle_click(x, y, button)

        def on_released(gesture, _count, _x, _y):
            if hasattr(cairo_app, "handle_button_release"):
                cairo_app.handle_button_release(gesture.get_current_button())

        click.connect("pressed", on_pressed)
        click.connect("released", on_released)
        area.add_controller(click)

        motion = Gtk.EventControllerMotion.new()

        def on_motion(_controller, x, y):
            state["pointer"] = (x, y)
            cairo_app.handle_motion(x, y)

        motion.connect("motion", on_motion)
        area.add_controller(motion)

        scroll = Gtk.EventControllerScroll.new(Gtk.EventControllerScrollFlags.VERTICAL)

        def on_scroll(_controller, _dx, dy):
            if hasattr(cairo_app, "handle_scroll"):
                cairo_app.handle_scroll(1 if dy > 0 else -1)
            else:
                x, y = state["pointer"]
                cairo_app.handle_click(x, y, 5 if dy > 0 else 4)
            return True

        scroll.connect("scroll", on_scroll)
        area.add_controller(scroll)

        key_controller = Gtk.EventControllerKey.new()

        def on_key(_controller, keyval, _keycode, modifiers):
            text = ""
            blocked = Gdk.ModifierType.CONTROL_MASK | Gdk.ModifierType.ALT_MASK | Gdk.ModifierType.SUPER_MASK
            if not modifiers & blocked:
                codepoint = Gdk.keyval_to_unicode(keyval)
                if codepoint:
                    candidate = chr(codepoint)
                    if candidate.isprintable():
                        text = candidate
            cairo_app.handle_key(keyval, text)
            return True

        key_controller.connect("key-pressed", on_key)
        area.add_controller(key_controller)

        window.set_child(area)
        window.present()
        area.grab_focus()

    application.connect("activate", on_activate)
    application.run(sys.argv)