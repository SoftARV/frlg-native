namespace Frlg {

[GtkTemplate (ui = "/io/github/softarv/frlg/window.ui")]
public class Window : Adw.ApplicationWindow {
    [GtkChild] private unowned Gtk.Stack stack;
    [GtkChild] private unowned Gtk.Button import_button;
    [GtkChild] private unowned Gtk.Button play_button;
    [GtkChild] private unowned Gtk.Label import_error;
    [GtkChild] private unowned Adw.ActionRow game_row;
    [GtkChild] private unowned Adw.StatusPage import_page;
    [GtkChild] private unowned Adw.PreferencesGroup saves_group;
    [GtkChild] private unowned Gtk.Button add_save_button;
    [GtkChild] private unowned Gtk.Button import_save_button;
    [GtkChild] private unowned Adw.ToastOverlay toasts;

    private Game? game = null;
    private SimpleAction? forget_action = null;
    private Profiles profiles = new Profiles ();
    private Settings settings = new Settings ();
    private Gtk.CheckButton? radio_group = null;
    private Gtk.Widget[] save_rows = {};

    public Window (Gtk.Application app) {
        Object (application: app);
    }

    construct {
        forget_action = new SimpleAction ("forget", null);
        forget_action.activate.connect (this.on_forget);
        forget_action.set_enabled (false);
        this.add_action (forget_action);

        import_button.clicked.connect (this.on_import_clicked);
        play_button.clicked.connect (this.on_play_clicked);
        add_save_button.clicked.connect (this.on_add_save);
        import_save_button.clicked.connect (this.on_import_save);

        // The game has to exist before the saves are listed: only it can read a
        // save, and a list built before it is found silently skips every lookup
        // and leaves the rows saying nothing but "Saved game".
        var binary = Game.find ();
        if (binary == null) {
            this.fill_saves ();
            import_page.title = _("The game is not installed");
            import_page.description =
                _("The launcher could not find the frlg-native binary. Set FRLG_GAME_BIN to its path.");
            import_button.sensitive = false;
            return;
        }

        game = new Game (binary);
        this.fill_saves ();
        this.refresh.begin ();
    }

    private async void refresh () {
        if (game == null)
            return;

        yield game.describe ();
        game_row.title = game.title;
        stack.visible_child_name = game.imported ? "library" : "import";
        forget_action.set_enabled (game.imported);
    }

    // Rebuilt rather than patched: the list is a handful of rows, and a rule
    // that always redraws is one that cannot disagree with the model.
    private void fill_saves () {
        foreach (var row in save_rows)
            saves_group.remove (row);
        save_rows = {};
        radio_group = null;

        var chosen = profiles.selected ();
        for (uint i = 0; i < profiles.items.get_n_items (); i++) {
            var profile = (Profile) profiles.items.get_item (i);
            var row = new Adw.ActionRow () {
                title = profile.name,
                subtitle = profile.has_save ? _("Saved game") : _("Not started yet"),
            };

            var pick = new Gtk.CheckButton () { valign = Gtk.Align.CENTER };
            if (radio_group == null)
                radio_group = pick;
            else
                pick.group = radio_group;
            pick.active = (chosen != null && profile.id == chosen.id);

            pick.toggled.connect (() => {
                if (pick.active)
                    profiles.select (profile);
            });

            row.add_prefix (pick);
            row.add_suffix (this.make_row_menu (profile));
            row.activatable_widget = pick;
            saves_group.add (row);
            save_rows += row;

            // Asked for rather than assumed: only the game can read its own
            // save, so the row starts with what is known and fills in.
            if (profile.has_save && game != null)
                this.describe_save.begin (profile, row);
        }
    }

    // A menu per row rather than three buttons: the row stays readable, and
    // there is somewhere obvious to put rename and delete later.
    private Gtk.Widget make_row_menu (Profile profile) {
        var box = new Gtk.Box (Gtk.Orientation.VERTICAL, 0) { margin_top = 6,
            margin_bottom = 6, margin_start = 6, margin_end = 6 };
        var popover = new Gtk.Popover () { child = box };
        var button = new Gtk.MenuButton () {
            icon_name = "view-more-symbolic",
            valign = Gtk.Align.CENTER,
            popover = popover,
            tooltip_text = _("Save options"),
        };
        button.add_css_class ("flat");

        var show = new Gtk.Button.with_label (_("Show files")) {
            halign = Gtk.Align.FILL, has_frame = false };
        show.get_first_child ().halign = Gtk.Align.START;
        show.clicked.connect (() => { popover.popdown (); this.show_files (profile); });
        box.append (show);

        var rename = new Gtk.Button.with_label (_("Rename…")) {
            halign = Gtk.Align.FILL, has_frame = false };
        rename.get_first_child ().halign = Gtk.Align.START;
        rename.clicked.connect (() => { popover.popdown (); this.rename_save (profile); });
        box.append (rename);

        var export_save = new Gtk.Button.with_label (_("Export a copy…")) {
            halign = Gtk.Align.FILL, has_frame = false };
        export_save.get_first_child ().halign = Gtk.Align.START;
        export_save.sensitive = profile.has_save;
        export_save.clicked.connect (() => { popover.popdown (); this.export_save (profile); });
        box.append (export_save);

        var remove = new Gtk.Button.with_label (_("Delete…")) {
            halign = Gtk.Align.FILL, has_frame = false };
        remove.get_first_child ().halign = Gtk.Align.START;
        remove.add_css_class ("destructive-action");
        remove.clicked.connect (() => { popover.popdown (); this.delete_save (profile); });
        box.append (remove);

        return button;
    }

    private void show_files (Profile profile) {
        DirUtils.create_with_parents (profile.directory, 0755);
        var launcher = new Gtk.FileLauncher (File.new_for_path (profile.directory));
        launcher.launch.begin (this, null, null);
    }

    // The save is the flash image an emulator writes, so this is how a game
    // moves between mGBA, a cartridge dump and here. Which is also why the size
    // is checked: a file of the wrong length is some other game's save, and
    // loading it would look like corruption rather than a mistake.
    private const int64 SAVE_BYTES = 131072;

    // Importing brings a save *in*; it never writes over one that is already
    // there. A confirmation would have been the other way to do this, and a
    // dialog is a weaker guard than an action with nothing to destroy.
    private void on_import_save () {
        var filter = new Gtk.FileFilter () { name = _("Save file") };
        filter.add_pattern ("*.sav");
        var filters = new ListStore (typeof (Gtk.FileFilter));
        filters.append (filter);

        var dialog = new Gtk.FileDialog () {
            title = _("Import a save"), filters = filters, modal = true };

        dialog.open.begin (this, null, (source, result) => {
            try {
                var file = dialog.open.end (result);
                if (file == null)
                    return;

                var size = file.query_info ("standard::size", FileQueryInfoFlags.NONE).get_size ();
                if (size != SAVE_BYTES) {
                    this.complain (_("That file is %lld bytes; a save for this game is %lld.")
                        .printf (size, SAVE_BYTES));
                    return;
                }

                // Named after the file, since that is the only clue there is
                // until the game has been asked what is inside it. Renaming is
                // one menu away.
                var label = file.get_basename ();
                if (label.has_suffix (".sav"))
                    label = label.substring (0, label.length - 4);

                var profile = profiles.add (label);
                try {
                    file.copy (File.new_for_path (profile.save_path), FileCopyFlags.NONE);
                } catch (Error e) {
                    // Nothing was imported, so nothing should be left behind.
                    profiles.remove (profile);
                    this.complain (e.message);
                    this.fill_saves ();
                    return;
                }
                this.fill_saves ();
                toasts.add_toast (new Adw.Toast (_("Imported as “%s”").printf (label)));
            } catch (Error e) {
            }
        });
    }

    private void export_save (Profile profile) {
        var dialog = new Gtk.FileDialog () {
            title = _("Export a copy of this save"),
            initial_name = "%s.sav".printf (profile.id),
            modal = true,
        };

        dialog.save.begin (this, null, (source, result) => {
            try {
                var target = dialog.save.end (result);
                if (target == null)
                    return;
                File.new_for_path (profile.save_path).copy (target, FileCopyFlags.OVERWRITE);
                toasts.add_toast (new Adw.Toast (_("Save exported")));
            } catch (Error e) {
            }
        });
    }

    private void complain (string message) {
        var dialog = new Adw.AlertDialog (_("That did not work"), message);
        dialog.add_response ("close", _("Close"));
        dialog.present (this);
    }

    private async void describe_save (Profile profile, Adw.ActionRow row) {
        var summary = yield game.save_summary (profile.save_path);
        if (summary != null)
            row.subtitle = summary;
    }

    private void rename_save (Profile profile) {
        var entry = new Gtk.Entry () { text = profile.name };
        var dialog = new Adw.AlertDialog (_("Rename this save"), null);

        dialog.set_extra_child (entry);
        dialog.add_response ("cancel", _("Cancel"));
        dialog.add_response ("rename", _("Rename"));
        dialog.set_response_appearance ("rename", Adw.ResponseAppearance.SUGGESTED);
        dialog.set_default_response ("rename");
        dialog.set_close_response ("cancel");

        dialog.response.connect ((response) => {
            var name = entry.text.strip ();
            if (response == "rename" && name != "") {
                profiles.rename (profile, name);
                this.fill_saves ();
            }
        });
        dialog.present (this);
    }

    private void delete_save (Profile profile) {
        // The one destructive action here with nothing behind it -- the imported
        // game can be imported again, a save cannot -- so the way out is named
        // rather than implied.
        var body = profile.has_save
            ? _("“%s” has a game in it, and deleting is permanent. Export a copy first if you might want it back.").printf (profile.name)
            : _("“%s” has nothing saved in it yet.").printf (profile.name);

        var dialog = new Adw.AlertDialog (_("Delete this save?"), body);
        dialog.add_response ("cancel", _("Cancel"));
        dialog.add_response ("delete", _("Delete"));
        dialog.set_response_appearance ("delete", Adw.ResponseAppearance.DESTRUCTIVE);
        dialog.set_default_response ("cancel");
        dialog.set_close_response ("cancel");

        dialog.response.connect ((response) => {
            if (response == "delete") {
                profiles.remove (profile);
                this.fill_saves ();
                toasts.add_toast (new Adw.Toast (_("Save deleted")));
            }
        });
        dialog.present (this);
    }

    // Two groups, and the split is not cosmetic: one is about this computer and
    // applies before any save is loaded, the other is stored inside the save it
    // belongs to. The second is not built yet, and says so rather than showing
    // switches that would not stick.
    public void show_settings () {
        var page = new Adw.PreferencesPage ();

        var mine = new Adw.PreferencesGroup () {
            title = _("This computer"),
            description = _("Applies to every game and every save."),
        };

        var recording = new Adw.SwitchRow () {
            title = _("Record every session"),
            subtitle = _("Keeps what you pressed, the save you started from and the log, so a crash can be replayed. Five runs are kept."),
            active = settings.get_bool ("record-sessions", true),
        };
        recording.notify["active"].connect (() => {
            settings.set_bool ("record-sessions", recording.active);
        });
        mine.add (recording);

        var scale = new Adw.SpinRow.with_range (1, 8, 1) {
            title = _("Window size"),
            subtitle = _("How many screen pixels to a Game Boy Advance pixel."),
            value = settings.get_int ("scale", 6),
        };
        scale.notify["value"].connect (() => {
            settings.set_int ("scale", (int) scale.value);
        });
        mine.add (scale);
        page.add (mine);

        var profile = profiles.selected ();
        var theirs = new Adw.PreferencesGroup () {
            title = profile != null
                ? _("The save “%s”").printf (profile.name)
                : _("The selected save"),
            description = _("The game keeps these inside the save itself, so each one has its own."),
        };
        page.add (theirs);

        if (profile != null && profile.has_save && game != null)
            this.fill_options.begin (theirs, profile);
        else
            theirs.add (new Adw.ActionRow () {
                title = _("Nothing saved yet"),
                subtitle = _("Play once and save, and the game's own options appear here."),
            });

        var dialog = new Adw.PreferencesDialog ();
        dialog.add (page);
        dialog.present (this);
    }

    // The game's own options, in the order its OPTION screen lists them. The
    // frame style and the map zoom are left out: one is twenty numbered borders
    // and the other is not a setting, it is where the map was last left.
    private async void fill_options (Adw.PreferencesGroup group, Profile profile) {
        var values = yield game.options (profile.save_path);
        if (values.size () == 0) {
            group.add (new Adw.ActionRow () { title = _("This save could not be read") });
            return;
        }

        this.add_choice (group, profile, values, "sound", _("Sound"),
                         { "mono", "stereo" }, { _("Mono"), _("Stereo") });
        this.add_choice (group, profile, values, "text-speed", _("Text speed"),
                         { "slow", "mid", "fast" }, { _("Slow"), _("Medium"), _("Fast") });
        this.add_choice (group, profile, values, "battle-style", _("Battle style"),
                         { "shift", "set" }, { _("Shift"), _("Set") });
        this.add_choice (group, profile, values, "battle-scene", _("Battle animations"),
                         { "on", "off" }, { _("On"), _("Off") });
        this.add_choice (group, profile, values, "button-mode", _("Button mode"),
                         { "help", "lr", "l-equals-a" },
                         { _("Help"), _("L and R"), _("L equals A") });
    }

    private void add_choice (Adw.PreferencesGroup group, Profile profile,
                             HashTable<string, string> values, string key,
                             string title, string[] options, string[] labels) {
        var model = new Gtk.StringList (labels);
        var row = new Adw.ComboRow () { title = title, model = model };

        var current = values.get (key);
        for (uint i = 0; i < options.length; i++)
            if (options[i] == current)
                row.selected = i;

        // Set after the initial selection, or populating the row would write
        // the value back to the save it just came from.
        row.notify["selected"].connect (() => {
            var chosen = options[row.selected];
            this.apply_option.begin (profile, key, chosen);
        });
        group.add (row);
    }

    private async void apply_option (Profile profile, string key, string value) {
        if (!yield game.set_option (profile.save_path, key, value))
            this.complain (_("That option could not be saved."));
    }

    private void on_add_save () {
        var entry = new Gtk.Entry () { placeholder_text = _("Name") };
        var dialog = new Adw.AlertDialog (_("New save"),
            _("A separate game, with its own progress and its own options."));

        dialog.set_extra_child (entry);
        dialog.add_response ("cancel", _("Cancel"));
        dialog.add_response ("create", _("Create"));
        dialog.set_response_appearance ("create", Adw.ResponseAppearance.SUGGESTED);
        dialog.set_default_response ("create");
        dialog.set_close_response ("cancel");

        dialog.response.connect ((response) => {
            var name = entry.text.strip ();
            if (response == "create" && name != "") {
                profiles.add (name);
                this.fill_saves ();
            }
        });
        dialog.present (this);
    }

    private void on_import_clicked () {
        var filter = new Gtk.FileFilter ();
        filter.name = _("Game Boy Advance ROM");
        filter.add_pattern ("*.gba");

        var filters = new ListStore (typeof (Gtk.FileFilter));
        filters.append (filter);

        var dialog = new Gtk.FileDialog () {
            title = _("Choose a Pokémon FireRed ROM"),
            filters = filters,
            modal = true,
        };

        dialog.open.begin (this, null, (source, result) => {
            try {
                var file = dialog.open.end (result);
                if (file != null && file.get_path () != null)
                    this.do_import.begin (file.get_path ());
            } catch (Error e) {
                // Dismissing the chooser is not a failure worth reporting.
            }
        });
    }

    private async void do_import (string path) {
        import_button.sensitive = false;
        import_error.visible = false;
        import_button.label = _("Importing…");

        var problem = yield game.import (path);

        import_button.label = _("Choose a ROM…");
        import_button.sensitive = true;
        if (problem != null) {
            import_error.label = problem;
            import_error.visible = true;
            return;
        }
        yield this.refresh ();
    }

    // Destructive and easy to hit by accident from a menu, so it asks -- and
    // says what it does not touch, which is the part worth being sure about.
    private void on_forget () {
        var dialog = new Adw.AlertDialog (
            _("Remove the imported game?"),
            _("The game will have to be imported from a ROM again. Your saves and your ROM file are not touched."));
        dialog.add_response ("cancel", _("Cancel"));
        dialog.add_response ("remove", _("Remove"));
        dialog.set_response_appearance ("remove", Adw.ResponseAppearance.DESTRUCTIVE);
        dialog.set_default_response ("cancel");
        dialog.set_close_response ("cancel");

        dialog.response.connect ((response) => {
            if (response == "remove")
                this.do_forget.begin ();
        });
        dialog.present (this);
    }

    private async void do_forget () {
        yield game.forget ();
        yield this.refresh ();
    }

    private void on_play_clicked () {
        this.play.begin ();
    }

    // Out of the way while the game is up, back when it exits. The window is
    // hidden rather than closed: closing the last window would end the process,
    // and there would be nothing left to come back.
    private async void play () {
        stack.visible_child_name = "playing";
        this.set_visible (false);

        var profile = profiles.selected ();
        try {
            yield game.play (profile != null ? profile.save_path : null);
        } catch (Error e) {
            warning ("could not start the game: %s", e.message);
        }

        yield this.refresh ();
        this.fill_saves ();   // A first run turns "not started yet" into a save.
        this.set_visible (true);
        this.present ();
    }
}

}
