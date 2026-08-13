// Which half of the mixer is audible.
//
// Its own translation unit because both halves ask, and neither should pull the
// other in behind it: the sequencer's object carries unresolved references to
// the game's own code, which is fine in the port and fatal in a unit test that
// links the platform alone.

#include <stdlib.h>

#include "agb/audio.h"

// Read once: the environment cannot change under a running mixer, and this sits
// in the per-sample path.
int agb_audio_muted(int side)
{
    static int sides = -1;

    if (sides < 0)
    {
        const char *m = getenv("FRLG_AUDIO_MUTE");

        sides = 0;
        if (m != NULL && m[0] == 'd')
            sides = AGB_AUDIO_DIRECT;
        else if (m != NULL && m[0] == 'p')
            sides = AGB_AUDIO_PSG;
    }
    return (sides & side) != 0;
}
