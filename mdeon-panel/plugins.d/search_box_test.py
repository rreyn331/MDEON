#!/usr/bin/env python3
"""GTK4 fuzzy file/folder search plugin using a temporary SQLite index."""

from __future__ import annotations

import os
import tempfile

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Gio", "2.0")
gi.require_version("Pango", "1.0")

from gi.repository import Gdk, Gio, GLib, Gtk, Pango


class HomeSearchIndex:
    """Build and maintain a temporary SQLite index of everything in ~/."""

    def __init__(self):
        self.home_dir = os.path.expanduser("~")
        self.db_path = os.path.join(
            tempfile.gettempdir(),
            f"mdeon-panel-home-search-{os.getuid()}.sqlite3",
        )

        self.ready = self.database_is_usable()
        self.building = False
        self.listeners = []

        # Refresh the index in the background every time the plugin loads.
        # An existing completed index remains available while it rebuilds.
        self.start_build()

    def database_is_usable(self):
        import sqlite3

        if not os.path.isfile(self.db_path):
            return False

        try:
            with sqlite3.connect(self.db_path) as connection:
                row = connection.execute("""
                    SELECT name
                    FROM sqlite_master
                    WHERE type = 'table' AND name = 'files'
                """).fetchone()

            return row is not None

        except sqlite3.Error:
            return False

    def add_ready_listener(self, callback):
        if callback not in self.listeners:
            self.listeners.append(callback)

    def remove_ready_listener(self, callback):
        if callback in self.listeners:
            self.listeners.remove(callback)

    def start_build(self):
        if self.building:
            return

        self.building = True
        import threading

        threading.Thread(
            target=self.build_database,
            daemon=True,
        ).start()

    def build_database(self):
        import sqlite3

        """
        Create an updated database at .building, then atomically replace
        the existing completed database after indexing has finished.
        """
        temporary_path = f"{self.db_path}.building"

        try:
            try:
                os.remove(temporary_path)
            except FileNotFoundError:
                pass

            connection = sqlite3.connect(temporary_path)
            cursor = connection.cursor()

            # This is a temporary/disposable search database.
            cursor.execute("PRAGMA journal_mode = OFF")
            cursor.execute("PRAGMA synchronous = OFF")
            cursor.execute("PRAGMA temp_store = MEMORY")

            cursor.execute("""
                CREATE TABLE files (
                    path TEXT PRIMARY KEY,
                    name TEXT NOT NULL,
                    relative_path TEXT NOT NULL,
                    is_directory INTEGER NOT NULL
                )
            """)

            batch = []

            def walk_error(_error):
                # Skip inaccessible folders without stopping indexing.
                pass

            for directory, directory_names, file_names in os.walk(
                self.home_dir,
                topdown=True,
                followlinks=False,
                onerror=walk_error,
            ):
                for name in directory_names:
                    path = os.path.join(directory, name)

                    batch.append((
                        path,
                        name,
                        os.path.relpath(path, self.home_dir),
                        1,
                    ))

                for name in file_names:
                    path = os.path.join(directory, name)

                    batch.append((
                        path,
                        name,
                        os.path.relpath(path, self.home_dir),
                        0,
                    ))

                if len(batch) >= 2000:
                    cursor.executemany("""
                        INSERT OR REPLACE INTO files
                        (path, name, relative_path, is_directory)
                        VALUES (?, ?, ?, ?)
                    """, batch)

                    connection.commit()
                    batch.clear()

            if batch:
                cursor.executemany("""
                    INSERT OR REPLACE INTO files
                    (path, name, relative_path, is_directory)
                    VALUES (?, ?, ?, ?)
                """, batch)

            connection.commit()
            connection.close()

            # Do not replace an old usable DB until this DB is complete.
            os.replace(temporary_path, self.db_path)

            GLib.idle_add(self.finish_build)

        except Exception as error:
            print(f"[search] Could not build search index: {error}")

            try:
                if os.path.exists(temporary_path):
                    os.remove(temporary_path)
            except OSError:
                pass

            GLib.idle_add(self.build_failed)

    def finish_build(self):
        self.ready = True
        self.building = False

        for callback in list(self.listeners):
            callback()

        return False

    def build_failed(self):
        self.building = False
        return False


class SearchPopover(Gtk.Popover):
    MAX_RESULTS = 30
    SEARCH_DELAY_MS = 250

    def __init__(self, parent_button, search_index):
        super().__init__()

        self.search_index = search_index
        self.search_generation = 0
        self.search_timeout_id = 0
        self.closed = False

        self.set_parent(parent_button)
        self.set_has_arrow(False)

        # Custom class used to override panel/theme popover styling.
        self.add_css_class("search-popover")

        # Important for panel environments: do not close when entry gets focus.
        self.set_autohide(False)

        self.install_css()

        content = Gtk.Box(
            orientation=Gtk.Orientation.VERTICAL,
            spacing=6,
        )
        content.add_css_class("search-content")

        self.search_entry = Gtk.SearchEntry()
        self.search_entry.add_css_class("search-input")
        self.search_entry.set_placeholder_text("Search files and folders...")
        self.search_entry.set_hexpand(True)

        self.search_entry.connect(
            "search-changed",
            self.on_search_changed,
        )
        self.search_entry.connect(
            "activate",
            self.on_search_activated,
        )

        # Gtk.SearchEntry handles Escape itself and emits "stop-search".
        # This reliably closes the popover instead of merely clearing text.
        self.search_entry.connect(
            "stop-search",
            self.on_search_stop,
        )

        # Also catch Escape directly where possible.
        entry_key_controller = Gtk.EventControllerKey()
        entry_key_controller.connect(
            "key-pressed",
            self.on_search_entry_key_pressed,
        )
        self.search_entry.add_controller(entry_key_controller)

        content.append(self.search_entry)

        self.results = Gtk.ListBox()
        self.results.add_css_class("search-results")
        self.results.set_selection_mode(Gtk.SelectionMode.SINGLE)

        self.results.connect(
            "row-activated",
            self.on_result_activated,
        )

        # Keep the popover at a usable size even when there are many matches.
        self.results_scroll = Gtk.ScrolledWindow()
        self.results_scroll.add_css_class("search-results-scroll")
        self.results_scroll.set_policy(
            Gtk.PolicyType.NEVER,
            Gtk.PolicyType.AUTOMATIC,
        )
        self.results_scroll.set_min_content_height(60)
        self.results_scroll.set_max_content_height(340)
        self.results_scroll.set_propagate_natural_height(True)
        self.results_scroll.set_child(self.results)

        content.append(self.results_scroll)
        self.set_child(content)

        self.search_index.add_ready_listener(self.on_index_ready)

    def install_css(self):
        provider = Gtk.CssProvider()

        provider.load_from_data(b"""
            /*
             * Explicit opaque backgrounds for the popover and all of its
             * content. This overrides normal panel/theme transparent styling.
             */
            popover.search-popover,
            popover.search-popover.background,
            .search-popover {
                background-color: @theme_bg_color;
                background-image: none;
                border-radius: 10px;
                border: 1px solid alpha(@borders, 0.85);
                opacity: 1;
            }

            .search-popover > contents,
            .search-popover .search-content {
                background-color: @theme_bg_color;
                background-image: none;
                opacity: 1;
                padding: 14px;
                min-width: 440px;
            }

            .search-popover .search-input,
            .search-popover .search-input text {
                color: @theme_fg_color;
                caret-color: @theme_fg_color;
                background-color: alpha(@theme_fg_color, 0.10);
                background-image: none;
                opacity: 1;
            }

            .search-popover .search-results-scroll,
            .search-popover .search-results {
                background-color: @theme_bg_color;
                background-image: none;
                opacity: 1;
            }

            .search-popover .search-results {
                border-radius: 6px;
                padding: 6px;
            }

            .search-popover .result-item {
                padding: 8px 10px;
                border-radius: 4px;
                color: @theme_fg_color;
                background-color: transparent;
            }

            .search-popover .result-item:hover,
            .search-popover row:selected .result-item {
                background-color: alpha(@theme_fg_color, 0.15);
            }

            .search-popover .result-path {
                opacity: 0.72;
                font-size: 0.9em;
            }

            .search-popover .result-icon {
                color: @theme_fg_color;
                margin-right: 9px;
            }
        """)

        Gtk.StyleContext.add_provider_for_display(
            self.get_display(),
            provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
        )

    def popup_and_focus(self):
        self.popup()
        GLib.idle_add(self.focus_entry)

    def focus_entry(self):
        if not self.closed:
            self.search_entry.grab_focus()

        return False

    def on_search_stop(self, _entry):
        """Gtk.SearchEntry emits this when Escape is pressed."""
        self.popdown()

    def on_search_entry_key_pressed(
        self,
        _controller,
        keyval,
        _keycode,
        _state,
    ):
        if keyval == Gdk.KEY_Escape:
            self.popdown()
            return True

        return False

    def cleanup(self):
        self.closed = True
        self.search_generation += 1

        if self.search_timeout_id:
            GLib.source_remove(self.search_timeout_id)
            self.search_timeout_id = 0

        self.search_index.remove_ready_listener(self.on_index_ready)

    def clear_results(self):
        while True:
            row = self.results.get_first_child()

            if row is None:
                break

            self.results.remove(row)

    def add_message(self, message):
        row = Gtk.ListBoxRow()
        row.set_selectable(False)
        row.set_activatable(False)

        label = Gtk.Label(label=message)
        label.set_xalign(0)
        label.set_wrap(True)
        label.add_css_class("result-item")

        row.set_child(label)
        self.results.append(row)

    def add_result(self, path, name, relative_path, is_directory):
        row = Gtk.ListBoxRow()
        row.add_css_class("result-item")

        # Use GTK icons rather than emoji. Emoji can appear as blank squares
        # if the panel's current font lacks emoji glyphs.
        icon_name = (
            "folder-symbolic"
            if is_directory
            else "text-x-generic-symbolic"
        )

        icon = Gtk.Image.new_from_icon_name(icon_name)
        icon.set_pixel_size(18)
        icon.set_valign(Gtk.Align.START)
        icon.add_css_class("result-icon")

        text_box = Gtk.Box(
            orientation=Gtk.Orientation.VERTICAL,
            spacing=2,
        )
        text_box.set_hexpand(True)

        title = Gtk.Label(label=name)
        title.set_xalign(0)
        title.set_hexpand(True)
        title.set_ellipsize(Pango.EllipsizeMode.END)

        location = Gtk.Label(label=f"~/{relative_path}")
        location.set_xalign(0)
        location.set_hexpand(True)
        location.set_ellipsize(Pango.EllipsizeMode.END)
        location.add_css_class("result-path")

        text_box.append(title)
        text_box.append(location)

        item_box = Gtk.Box(
            orientation=Gtk.Orientation.HORIZONTAL,
            spacing=0,
        )
        item_box.append(icon)
        item_box.append(text_box)

        row.set_child(item_box)
        row.search_path = path

        self.results.append(row)

    @staticmethod
    def fuzzy_score(query, text):
        """Return a fuzzy subsequence score; lower scores are better."""
        query = query.casefold()
        text = text.casefold()

        position = -1
        previous_position = -2
        score = 0

        for character in query:
            match_position = text.find(character, position + 1)

            if match_position == -1:
                return None

            score += match_position

            if match_position == previous_position + 1:
                score -= 10

            if match_position == 0 or text[match_position - 1] in " /_-.":
                score -= 15

            position = match_position
            previous_position = match_position

        score += len(text) // 4
        return score

    @staticmethod
    def fuzzy_like_pattern(word):
        """
        Convert a fuzzy word into a SQLite LIKE prefilter.

        Example:
            doc -> %d%o%c%
        """
        result = ["%"]

        for character in word.casefold():
            if character in ("\\", "%", "_"):
                result.append("\\")

            result.append(character)
            result.append("%")

        return "".join(result)

    def score_item(self, query, name, relative_path, is_directory):
        """
        Prefer shallow folders such as ~/Documents and ~/Downloads before
        deeply nested files.
        """
        words = query.casefold().replace("/", " ").split()

        name_folded = name.casefold()
        depth = relative_path.count(os.sep)

        total_score = 0
        every_word_matches_name = True

        for word in words:
            name_score = self.fuzzy_score(word, name)
            path_score = self.fuzzy_score(word, relative_path)

            if name_score is None and path_score is None:
                return None

            if name_score is not None:
                total_score += name_score
            else:
                total_score += path_score + 35
                every_word_matches_name = False

            # "do" strongly prefers Documents and Downloads.
            if name_folded.startswith(word):
                total_score -= 80
            elif word in name_folded:
                total_score -= 20

        # 0: top-level matching folders in ~/
        # 1: shallow matching folders
        # 2: deep matching folders
        # 3: files
        if is_directory:
            if depth == 0 and every_word_matches_name:
                hierarchy_tier = 0
            elif depth <= 1:
                hierarchy_tier = 1
            else:
                hierarchy_tier = 2

            total_score -= 35
            total_score += depth * 18
        else:
            hierarchy_tier = 3
            total_score += depth * 10

        return (hierarchy_tier, total_score)

    def on_index_ready(self):
        if not self.closed:
            self.on_search_changed(self.search_entry)

    def on_search_changed(self, entry):
        query = entry.get_text().strip()

        self.search_generation += 1
        generation = self.search_generation

        if self.search_timeout_id:
            GLib.source_remove(self.search_timeout_id)
            self.search_timeout_id = 0

        self.clear_results()

        if not query:
            return

        if not self.search_index.ready:
            self.add_message("Building file index for ~/ …")
            return

        self.add_message("Searching…")

        self.search_timeout_id = GLib.timeout_add(
            self.SEARCH_DELAY_MS,
            self.start_delayed_search,
            query,
            generation,
        )

    def start_delayed_search(self, query, generation):
        self.search_timeout_id = 0

        if self.closed or generation != self.search_generation:
            return False

        import threading

        threading.Thread(
            target=self.search_database,
            args=(query, generation),
            daemon=True,
        ).start()

        return False

    def search_database(self, query, generation):
        import heapq
        import sqlite3

        matches = []

        try:
            connection = sqlite3.connect(
                f"file:{self.search_index.db_path}?mode=ro",
                uri=True,
            )
            cursor = connection.cursor()

            words = query.casefold().replace("/", " ").split()
            conditions = []
            parameters = []

            for word in words:
                pattern = self.fuzzy_like_pattern(word)

                conditions.append("""
                    (
                        lower(name) LIKE ? ESCAPE '\\'
                        OR lower(relative_path) LIKE ? ESCAPE '\\'
                    )
                """)

                parameters.extend((pattern, pattern))

            sql = f"""
                SELECT path, name, relative_path, is_directory
                FROM files
                WHERE {" AND ".join(conditions)}
            """

            cursor.execute(sql, parameters)

            for path, name, relative_path, is_directory in cursor:
                if self.closed or generation != self.search_generation:
                    connection.close()
                    return

                is_directory = bool(is_directory)

                score = self.score_item(
                    query,
                    name,
                    relative_path,
                    is_directory,
                )

                if score is not None:
                    matches.append((
                        score,
                        path,
                        name,
                        relative_path,
                        is_directory,
                    ))

            connection.close()

        except sqlite3.Error as error:
            print(f"[search] SQLite search failed: {error}")

            GLib.idle_add(
                self.display_error,
                f"Database search failed: {error}",
                generation,
            )
            return

        best_matches = heapq.nsmallest(
            self.MAX_RESULTS,
            matches,
        )

        GLib.idle_add(
            self.display_results,
            best_matches,
            generation,
        )

    def display_error(self, message, generation):
        if self.closed or generation != self.search_generation:
            return False

        self.clear_results()
        self.add_message(message)

        return False

    def display_results(self, matches, generation):
        if self.closed or generation != self.search_generation:
            return False

        self.clear_results()

        if not matches:
            self.add_message("No matching files or folders found.")
            return False

        for _score, path, name, relative_path, is_directory in matches:
            self.add_result(
                path,
                name,
                relative_path,
                is_directory,
            )

        return False

    def open_path(self, path):
        try:
            uri = Gio.File.new_for_path(path).get_uri()
            Gio.AppInfo.launch_default_for_uri(uri, None)

        except GLib.Error as error:
            print(f"[search] Could not open {path}: {error}")

    def on_search_activated(self, _entry):
        """Enter opens the selected result, or the first result."""
        row = self.results.get_selected_row()

        if row is None:
            row = self.results.get_first_child()

        path = getattr(row, "search_path", None) if row else None

        if path:
            self.open_path(path)
            self.popdown()

    def on_result_activated(self, _listbox, row):
        path = getattr(row, "search_path", None)

        if path:
            self.open_path(path)
            self.popdown()


class SearchPlugin(Gtk.Box):
    def __init__(self):
        super().__init__(
            orientation=Gtk.Orientation.HORIZONTAL,
            spacing=0,
        )

        self.search_index = None

        self.button = Gtk.Button()
        self.button.set_icon_name("system-search-symbolic")
        self.button.add_css_class("flat")
        self.button.add_css_class("flat-icon")
        self.button.add_css_class("sys-button")
        self.button.set_tooltip_text("Search files and folders")
        self.button.set_valign(Gtk.Align.CENTER)
        self.button.set_has_frame(False)

        self.append(self.button)

        self.popover = None
        self.button.connect("clicked", self.toggle)

    def toggle(self, _button):
        if self.popover and self.popover.is_visible():
            self.popover.popdown()
        else:
            self.open()

    def open(self):
        if self.search_index is None:
            self.search_index = HomeSearchIndex()
        self.popover = SearchPopover(
            self.button,
            self.search_index,
        )

        self.popover.connect(
            "closed",
            self.on_popover_closed,
        )

        self.popover.popup_and_focus()

    def on_popover_closed(self, popover):
        popover.cleanup()
        popover.unparent()
        self.popover = None


PLUGIN_CODE = "J"
PLUGIN_NAME = "Search"
PLUGIN_DESCRIPTION = "Fuzzy search files and folders in your home directory"
SINGLETON = True


def create(context):
    return SearchPlugin()
