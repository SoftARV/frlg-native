// A session: everything needed to reproduce what a player just did.
//
// A crash report is only worth having if the run can be replayed, and a replay
// needs three things that have to be collected *before* anything goes wrong --
// the input trace, the save the run started from, and the log. Asking a tester
// to arrange that by hand is how reports arrive missing one of them, so every
// run collects its own, and a crash only has to decide what to do with what is
// already on disk. See docs/adr/0016-every-session-records-itself.md.

#ifndef GUARD_HOST_SESSION_H
#define GUARD_HOST_SESSION_H

#include <stddef.h>

// Opens this run's directory under the host's data location and prunes older
// ones. Returns 0 if a session is recording, non-zero if it is switched off or
// the directory could not be made -- in which case everything below is inert
// and the port carries on without a session, which is never fatal.
int host_session_open(void);

// This run's directory, or NULL when no session is recording.
const char *host_session_dir(void);

// Builds a path to `name` inside this run's directory. Returns NULL when no
// session is recording, so a caller can use the result directly as "record here
// or do not record".
const char *host_session_file(const char *name, char *buf, size_t len);

// Copies `path` into the session as `name` -- how the save the run started from
// is kept, since playing changes it and a trace only replays against the state
// it began with.
void host_session_keep(const char *path, const char *name);

// Everything written to stderr from here on also lands in the session's log,
// while still reaching the terminal. Idempotent, and quietly does nothing when
// no session is recording.
void host_session_capture_log(void);

// Packs everything the session collected into one zip inside it, and returns
// its path -- what a person attaches to an issue, instead of four files they
// have to find and one they forget. NULL if there is nothing to pack.
const char *host_session_bundle(void);

void host_session_close(void);

#endif // GUARD_HOST_SESSION_H
