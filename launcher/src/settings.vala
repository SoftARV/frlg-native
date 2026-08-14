// The port's own settings -- the ones the game has no concept of.
//
// Written here, read by the port at startup, in the platform's config location
// rather than beside the saves: these are choices about this computer, not
// about a game. The other kind of setting, the seven the game itself keeps,
// lives inside each save and is reached through the game instead (ADR 0017).

namespace Frlg {

public class Settings : Object {
    private KeyFile store = new KeyFile ();
    private string file;

    private const string GROUP = "settings";

    public Settings () {
        var directory = Path.build_filename (Environment.get_user_config_dir (),
                                             "frlg-native");
        DirUtils.create_with_parents (directory, 0755);
        file = Path.build_filename (directory, "settings.ini");
        try {
            store.load_from_file (file, KeyFileFlags.KEEP_COMMENTS);
        } catch (Error e) {
            // No file yet means every default, which is the usual first run.
        }
    }

    private void flush () {
        try {
            FileUtils.set_contents (file, store.to_data ());
        } catch (Error e) {
            warning ("could not write %s: %s", file, e.message);
        }
    }

    public bool get_bool (string key, bool fallback) {
        try {
            return store.get_boolean (GROUP, key);
        } catch (Error e) {
            return fallback;
        }
    }

    public void set_bool (string key, bool value) {
        store.set_boolean (GROUP, key, value);
        flush ();
    }

    public int get_int (string key, int fallback) {
        try {
            var value = store.get_integer (GROUP, key);
            return value > 0 ? value : fallback;
        } catch (Error e) {
            return fallback;
        }
    }

    public void set_int (string key, int value) {
        store.set_integer (GROUP, key, value);
        flush ();
    }
}

}
