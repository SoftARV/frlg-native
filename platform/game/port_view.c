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
#include "quest_log.h"
#include "constants/quest_log.h"

#include "agb/frame.h"
#include "agb/ppu.h"

// How much further than the hardware's screen the field view reaches, in
// metatiles, on each side.
//
// Object events spawn in a window around the player, and upstream sizes that
// window to the screen it had. A wider one shows ground the game has not spawned
// anything on, so an NPC standing there does not exist until the player walks
// close enough, and then appears out of nothing a few tiles inside the edge.
//
// Zero at 240x160, which is what leaves upstream's window exactly as it was.
u8 agb_port_view_margin_x(void)
{
    int extra = (agb_ppu_width() - AGB_PPU_MIN_W) / 2;

    // Rounded up: half a metatile of view still shows whoever stands in it.
    return (u8)((extra + 15) / 16);
}

u8 agb_port_view_margin_y(void)
{
    int extra = (agb_ppu_height() - AGB_PPU_MIN_H) / 2;

    return (u8)((extra + 15) / 16);
}

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

    // A quest log replay runs on the field's own callback, so it counts as the
    // field by every test above -- but what it shows is a recording being
    // narrated, framed by boxes drawn for a 240-pixel screen. Widening it shows
    // more of a map the recording never walked and leaves the narration boxes
    // floating in it. It keeps the hardware's size, like a menu.
    if (QL_IS_PLAYBACK_STATE)
        return FALSE;

    now = agb_frame_count();
    // The counter only goes forward, but a comparison that assumes so is one
    // that breaks quietly if it ever does not.
    if (now < sLastFieldFrame)
        return TRUE;
    return (now - sLastFieldFrame) <= FIELD_GRACE_FRAMES;
}
