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
// pixels, on each side. Zero at 240x160.
//
// Two things in the game are sized to the screen it had, and an NPC standing
// where a wider one can see them needs both:
//
//   - the window object events *spawn* in, so that somebody out there exists at
//     all rather than coming into being as the player walks up;
//   - the test that decides an object is *off screen* and hides its sprite,
//     which is the one that made the first look like it had done nothing.
//
// Pixels rather than metatiles because the second wants pixels; the first
// divides.
u16 agb_port_view_margin_x(void)
{
    return (u16)((agb_ppu_width() - AGB_PPU_MIN_W) / 2);
}

u16 agb_port_view_margin_y(void)
{
    return (u16)((agb_ppu_height() - AGB_PPU_MIN_H) / 2);
}

// The same margin for the spawn window, in metatiles, per side.
//
// Upstream's window already reaches further than the hardware's screen -- ten
// metatiles right of the player, nine below, nine left, seven above -- which is
// why it never had to think about any of this. What is asked for here is only
// what the wider view adds on top of that, and it is asked for per side because
// the two ends are not alike.
//
// Each side asks only for what the view needs. It used to ask for the smaller of
// that and what the object coordinates had left, because OAM keeps nine bits of
// an object's X and eight of its Y and the hardware wraps them -- so an object
// placed near the end of that range came back round to the other side of the
// picture, and the field had to not put one there.
//
// The renderer is given the position the game computed now (ADR 0024), so there
// is no range to run out of and no cap to apply.
#define PLAYER_SCREEN_X 120  // where the player's own sprite sits, in the
#define PLAYER_SCREEN_Y 72   // hardware's coordinates

// How many metatiles past `reach` the view needs, to see `distance` pixels.
static u8 needed_beyond(int distance, int reach)
{
    int needed = (distance + 15) / 16 - reach;

    return needed > 0 ? (u8)needed : 0;
}

u8 agb_port_view_spawn_left(void)
{
    return needed_beyond(PLAYER_SCREEN_X + agb_port_view_margin_x(), 9);
}

u8 agb_port_view_spawn_right(void)
{
    return needed_beyond(AGB_PPU_MIN_W + agb_port_view_margin_x() - PLAYER_SCREEN_X, 10);
}

u8 agb_port_view_spawn_up(void)
{
    return needed_beyond(PLAYER_SCREEN_Y + agb_port_view_margin_y(), 7);
}

u8 agb_port_view_spawn_down(void)
{
    return needed_beyond(AGB_PPU_MIN_H + agb_port_view_margin_y() - PLAYER_SCREEN_Y, 9);
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
