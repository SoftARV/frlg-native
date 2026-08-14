// Saves, as things a person picks between rather than one file in a directory.
//
// The game knows nothing about any of this: it takes a path in FRLG_SAV and
// writes a save there. Profiles are the launcher's idea, which is what lets a
// vanilla run and -- later -- a modded one exist side by side without either
// knowing the other is there.
//
// One directory per profile rather than one file, because a profile is going to
// grow: the mods it runs with, and the game options that live inside its save.

namespace Frlg {

public class Profile : Object {
    public string id { get; construct; }
    public string name { get; set; }

    public Profile (string id, string name) {
        Object (id: id, name: name);
        this.name = name;
    }

    public string directory {
        owned get { return Path.build_filename (Profiles.root (), id); }
    }

    public string save_path {
        owned get { return Path.build_filename (directory, "game.sav"); }
    }

    // A profile whose game has never been saved is a real profile with nothing
    // in it yet, which is worth saying rather than hiding.
    public bool has_save {
        get { return FileUtils.test (save_path, FileTest.EXISTS); }
    }
}

public class Profiles : Object {
    private KeyFile store = new KeyFile ();
    private string file;

    public ListStore items { get; private set; }
    public string selected_id { get; set; default = "vanilla"; }

    public static string root () {
        return Path.build_filename (Environment.get_user_data_dir (),
                                    "frlg-native", "saves");
    }

    public Profiles () {
        items = new ListStore (typeof (Profile));
        file = Path.build_filename (root (), "profiles.ini");
        DirUtils.create_with_parents (root (), 0755);
        load ();
    }

    private void load () {
        try {
            store.load_from_file (file, KeyFileFlags.NONE);
        } catch (Error e) {
            // No file yet is the normal first run, not a problem to report.
        }

        foreach (var group in store.get_groups ()) {
            if (!group.has_prefix ("save:"))
                continue;
            var id = group.substring ("save:".length);
            string name;
            try {
                name = store.get_string (group, "name");
            } catch (Error e) {
                name = id;
            }
            items.append (new Profile (id, name));
        }

        try {
            if (store.has_group ("launcher"))
                selected_id = store.get_string ("launcher", "selected");
        } catch (Error e) {
        }

        // Somewhere to play on a first run, so the library is never an empty
        // list with a Play button that has nothing to save into.
        if (items.get_n_items () == 0)
            add (_("Vanilla"));
    }

    public void save () {
        store.set_string ("launcher", "selected", selected_id);
        try {
            FileUtils.set_contents (file, store.to_data ());
        } catch (Error e) {
            warning ("could not write %s: %s", file, e.message);
        }
    }

    // Ids are derived from the name but must not collide or contain anything a
    // path would object to, since one becomes a directory.
    private string make_id (string name) {
        var builder = new StringBuilder ();
        foreach (var c in name.down ().to_utf8 ()) {
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                builder.append_c ((char) c);
            else if (builder.len > 0 && builder.str[builder.len - 1] != '-')
                builder.append_c ('-');
        }
        var basis = builder.str.strip ();
        if (basis == "" || basis == "-")
            basis = "save";

        var candidate = basis;
        for (int n = 2; store.has_group ("save:" + candidate); n++)
            candidate = "%s-%d".printf (basis, n);
        return candidate;
    }

    public Profile add (string name) {
        var id = make_id (name);
        var profile = new Profile (id, name);

        DirUtils.create_with_parents (profile.directory, 0755);
        store.set_string ("save:" + id, "name", name);
        items.append (profile);
        selected_id = id;
        save ();
        return profile;
    }

    public Profile? selected () {
        for (uint i = 0; i < items.get_n_items (); i++) {
            var profile = (Profile) items.get_item (i);
            if (profile.id == selected_id)
                return profile;
        }
        return items.get_n_items () > 0 ? (Profile) items.get_item (0) : null;
    }

    public void select (Profile profile) {
        selected_id = profile.id;
        save ();
    }
}

}
