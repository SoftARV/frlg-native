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
// An object's coordinates are nine bits across and eight down, and the hardware
// wraps them: one placed near the end of that range comes back round to the
// *other* side of the picture. The renderer reproduces that, so the field must
// not put an object there -- but only on the side the wrap runs towards. Going
// the other way an object simply leaves the picture, which costs nothing.
//
// So the near side takes what it needs and the far side takes the smaller of
// what it needs and what the coordinate space has left. At 464x192 that is six
// metatiles left, five right, and nothing either way vertically -- upstream's
// own window already reaches past the top and bottom of a 192-pixel view.
#define OBJECT_SPAN_MAX 32   // the widest and tallest object, in pixels
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
    int margin = agb_port_view_margin_x();
    u8 needed = needed_beyond(AGB_PPU_MIN_W + margin - PLAYER_SCREEN_X, 10);
    // Where the nine bits run out, less the widest object that must fit before
    // them, less where the player already is.
    int allowed = (0x200 - margin - OBJECT_SPAN_MAX - PLAYER_SCREEN_X) / 16 - 10;

    if (allowed < 0)
        allowed = 0;
    return needed < allowed ? needed : (u8)allowed;
}

u8 agb_port_view_spawn_up(void)
{
    return needed_beyond(PLAYER_SCREEN_Y + agb_port_view_margin_y(), 7);
}

u8 agb_port_view_spawn_down(void)
{
    int margin = agb_port_view_margin_y();
    u8 needed = needed_beyond(AGB_PPU_MIN_H + margin - PLAYER_SCREEN_Y, 9);
    int allowed = (0x100 - margin - OBJECT_SPAN_MAX - PLAYER_SCREEN_Y) / 16 - 9;

    if (allowed < 0)
        allowed = 0;
    return needed < allowed ? needed : (u8)allowed;
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
