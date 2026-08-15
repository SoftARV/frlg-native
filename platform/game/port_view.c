// Which screens are allowed to show more of the world.
//
// The viewport widens on the field and nowhere else. A menu, a battle or the
// summary screen has no more world to show: its backgrounds are drawn for
// 240x160, and past that edge there is backdrop and wrapped tiles rather than
// anything worth seeing. So those screens keep the hardware's size and the
// window letterboxes them.
//
// Knowing which is which needs a word from the game, because the overworld's
// own callback is static and the port cannot recognise it by comparing
// gMain.callback2. `overworld.patch` calls in from the field's per-frame
// routine; this counts frames since it last did.

#include "global.h"

#include "agb/frame.h"

// Two frames of grace. The field's callback runs once per frame, so one frame
// of silence means a screen change, not a slow frame -- but a fade or a script
// can skip one, and flapping the viewport on a single missed call would be
// worse than being a frame late to letterbox.
#define FIELD_GRACE_FRAMES 2

static u32 sLastFieldFrame;
static bool8 sSeenField;

void agb_port_field_frame(void)
{
    sLastFieldFrame = agb_frame_count();
    sSeenField = TRUE;
}

bool8 agb_port_field_active(void)
{
    u32 now;

    if (!sSeenField)
        return FALSE;

    now = agb_frame_count();
    // The counter only goes forward, but a comparison that assumes so is one
    // that breaks quietly if it ever does not.
    if (now < sLastFieldFrame)
        return TRUE;
    return (now - sLastFieldFrame) <= FIELD_GRACE_FRAMES;
}
