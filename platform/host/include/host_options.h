// What the player chose for *this save*, as opposed to host_settings.h, which
// is what they chose for the install and which the port only reads.
//
// Two differences, and both are the reason this is a separate thing:
//
//   - **Per profile.** The launcher gives each profile its own save, and these
//     travel with it. Changing a display mode while playing one file does not
//     change another.
//   - **Written from inside the game.** The port's own options screen sets
//     these, so unlike settings.ini there is a writer here.
//
// They are *not* kept in the save. `SaveBlock2` matches the cartridge byte for
// byte ([ADR 0019](../../docs/adr/0019-saves-match-the-cartridge.md)) -- that is
// what lets a real cartridge save be imported -- so anything the port adds lives
// beside the save instead, and the save file stays something the original
// hardware would recognise.
//
// The environment still wins, the same as everywhere else: a run driven by
// FRLG_ variables must not depend on what somebody clicked.
#ifndef GUARD_HOST_OPTIONS_H
#define GUARD_HOST_OPTIONS_H

// Binds the store to a save file and loads what is beside it. Passing NULL, or
// a path with no directory, leaves the store empty but usable -- reads return
// their fallback and writes are kept in memory, which is what a run with no
// save should do.
void host_options_open(const char *save_path);

int host_option_get(const char *key, int fallback);
void host_option_set(const char *key, int value);

// Writes the file if anything changed. Returns 0 on success, -1 if there was
// nowhere to write or the write failed.
int host_options_flush(void);

#endif // GUARD_HOST_OPTIONS_H
