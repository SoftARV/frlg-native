namespace Frlg {

public class Application : Adw.Application {
    public Application () {
        Object (application_id: "io.github.softarv.frlg",
                flags: ApplicationFlags.DEFAULT_FLAGS);
    }

    construct {
        ActionEntry[] entries = {
            { "about", this.on_about },
            { "issues", this.on_issues },
            { "settings", this.on_settings },
            { "quit", this.quit },
        };
        this.add_action_entries (entries, this);
        this.set_accels_for_action ("app.quit", { "<primary>q" });
    }

    protected override void activate () {
        var window = this.active_window;
        if (window == null)
            window = new Frlg.Window (this);
        window.present ();
    }

    private void on_about () {
        var about = new Adw.AboutDialog () {
            application_name = "frlg-native",
            application_icon = "application-x-executable",
            developer_name = "SoftARV",
            version = "0.1.0",
            comments = _("A native port of Pokémon FireRed. Ships no game data: the game comes from a ROM you legally own."),
            website = "https://github.com/SoftARV/frlg-native",
            issue_url = "https://github.com/SoftARV/frlg-native/issues",
            license_type = Gtk.License.UNKNOWN,
        };
        about.present (this.active_window);
    }

    private void on_issues () {
        Gtk.UriLauncher launcher = new Gtk.UriLauncher (
            "https://github.com/SoftARV/frlg-native/issues");
        launcher.launch.begin (this.active_window, null, null);
    }

    private void on_settings () {
        var window = this.active_window as Frlg.Window;
        if (window != null)
            window.show_settings ();
    }
}

}
