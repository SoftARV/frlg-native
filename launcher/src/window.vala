namespace Frlg {

[GtkTemplate (ui = "/io/github/softarv/frlg/window.ui")]
public class Window : Adw.ApplicationWindow {
    [GtkChild] private unowned Gtk.Stack stack;
    [GtkChild] private unowned Gtk.Button import_button;
    [GtkChild] private unowned Gtk.Button play_button;
    [GtkChild] private unowned Gtk.Label import_error;
    [GtkChild] private unowned Adw.ActionRow game_row;
    [GtkChild] private unowned Adw.StatusPage import_page;

    private Game? game = null;
    private SimpleAction? forget_action = null;

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

        var binary = Game.find ();
        if (binary == null) {
            import_page.title = _("The game is not installed");
            import_page.description =
                _("The launcher could not find the frlg-native binary. Set FRLG_GAME_BIN to its path.");
            import_button.sensitive = false;
            return;
        }

        game = new Game (binary);
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

        try {
            yield game.play (null);
        } catch (Error e) {
            warning ("could not start the game: %s", e.message);
        }

        yield this.refresh ();
        this.set_visible (true);
        this.present ();
    }
}

}
