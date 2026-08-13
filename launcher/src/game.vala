// The game, as the launcher sees it: a program to ask questions of and to run.
//
// One binary is one title -- FireRed and LeafGreen are different builds, not
// different data -- so everything the launcher wants to know comes from asking
// that binary rather than from a copy kept here. It answers `key=value` lines
// to `--describe` and `--import`, which is why there is no parser below worth
// the name.

namespace Frlg {

public class Game : Object {
    public string binary { get; construct; }

    public string title { get; private set; default = "Pokémon FireRed"; }
    public string sha1 { get; private set; default = ""; }
    public string cache { get; private set; default = ""; }
    public bool imported { get; private set; default = false; }

    public Game (string binary) {
        Object (binary: binary);
    }

    // The game binary, in the three places it can be: named outright, beside
    // the launcher once installed, or in a build tree while working on it.
    public static string? find () {
        var named = Environment.get_variable ("FRLG_GAME_BIN");
        if (named != null && FileUtils.test (named, FileTest.IS_EXECUTABLE))
            return named;

        var beside = Path.build_filename (
            Path.get_dirname (Environment.get_current_dir ()), "frlg-native");
        if (FileUtils.test (beside, FileTest.IS_EXECUTABLE))
            return beside;

        foreach (var build in new string[] { "retail-play", "linux-release", "retail" }) {
            var candidate = Path.build_filename (
                Environment.get_home_dir (), "Developer", "fireRecomp", "frlg-native",
                "build", build, "ports", "desktop", "frlg-native");
            if (FileUtils.test (candidate, FileTest.IS_EXECUTABLE))
                return candidate;
        }
        return null;
    }

    // key=value, one per line. Anything else on the line is ignored rather than
    // treated as an error: the game is allowed to say more than we read.
    private static HashTable<string, string> parse (string text) {
        var fields = new HashTable<string, string> (str_hash, str_equal);
        foreach (var line in text.split ("\n")) {
            var at = line.index_of_char ('=');
            if (at > 0)
                fields.insert (line.substring (0, at), line.substring (at + 1).strip ());
        }
        return fields;
    }

    private async string? run (string[] argv, out int status) throws Error {
        status = -1;
        var process = new Subprocess.newv (argv,
            SubprocessFlags.STDOUT_PIPE | SubprocessFlags.STDERR_SILENCE);
        string output;
        yield process.communicate_utf8_async (null, null, out output, null);
        status = process.get_if_exited () ? process.get_exit_status () : -1;
        return output;
    }

    public async bool describe () {
        try {
            int status;
            var output = yield run ({ binary, "--describe" }, out status);
            if (output == null)
                return false;

            var fields = parse (output);
            if (fields.contains ("title"))
                title = fields.get ("title");
            if (fields.contains ("sha1"))
                sha1 = fields.get ("sha1");
            if (fields.contains ("cache"))
                cache = fields.get ("cache");
            imported = fields.contains ("imported") && fields.get ("imported") == "yes";
            return true;
        } catch (Error e) {
            warning ("cannot ask %s about itself: %s", binary, e.message);
            return false;
        }
    }

    // Returns null when the ROM was imported, or the reason it was not -- which
    // the game words for a person, so it is shown as it arrives.
    public async string? import (string rom) {
        try {
            int status;
            var output = yield run ({ binary, "--import", rom }, out status);
            var fields = parse (output ?? "");

            if (status == 0 && fields.get ("ok") == "yes") {
                yield describe ();
                return null;
            }
            var reason = fields.get ("error");
            return reason != null ? reason : _("The game could not be imported.");
        } catch (Error e) {
            return e.message;
        }
    }

    // Throws away the imported copy. The player's ROM and saves are untouched;
    // this only makes the next launch import again, which is otherwise a path
    // you get to walk once.
    public async bool forget () {
        try {
            int status;
            yield run ({ binary, "--forget" }, out status);
            yield describe ();
            return status == 0;
        } catch (Error e) {
            warning ("could not remove the imported game: %s", e.message);
            return false;
        }
    }

    // Runs the game and returns when it exits. The caller is expected to be out
    // of the way in the meantime.
    public async void play (string? save_path) throws Error {
        var launcher = new SubprocessLauncher (SubprocessFlags.NONE);
        if (save_path != null)
            launcher.setenv ("FRLG_SAV", save_path, true);
        // Real time, one frame per game step: the clock a recording needs.
        launcher.setenv ("FRLG_LOCKSTEP", "pace", true);

        var process = launcher.spawnv ({ binary, "120000", "60" });
        yield process.wait_async (null);
    }
}

}
