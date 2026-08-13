// Catching a fault in game code and turning it into something a person can send.
//
// POSIX signals, so this lives with the port rather than in platform/agb --
// phase 8's Windows target answers the same three questions through structured
// exception handling instead.

#ifndef GUARD_PORT_CRASH_H
#define GUARD_PORT_CRASH_H

// Installs the fault handlers on the calling thread. Called from the game
// thread, whose stack is the one game code faults on.
void crash_install(void);

// Non-zero once a fault has been caught and unwound.
int crash_happened(void);

// A sentence naming what went wrong, for a person rather than a debugger.
const char *crash_summary(void);

// FRLG_CRASH_TEST=segv|bus|abort faults on purpose at the frame named by
// FRLG_CRASH_FRAME, so the whole path can be exercised rather than trusted.
// Called once a frame from the key source, on the game thread.
void crash_test_tick(unsigned frame);

#endif // GUARD_PORT_CRASH_H
