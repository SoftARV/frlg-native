// The scanline interrupts: H-blank and V-count match.
//
// These are what per-scanline effects are built on -- the game's battle
// transitions rewrite scroll and window registers from an H-blank handler and
// expect the change to land on the next line. So the tests here check not only
// that the interrupt fires, but that writing a register from inside it changes
// the picture from that point down.

#include <string.h>

#include "agb/memmap.h"
#include "agb/ppu.h"

#include "harness.h"

#define SCREEN_H 160

#define DISPCNT 0x000
#define DISPSTAT 0x004
#define VCOUNT 0x006
#define BG0CNT 0x008
#define BG0HOFS 0x010
#define IE 0x200
#define IF 0x202
#define IME 0x208

#define DISPCNT_BG0 0x0100
#define DISPCNT_FORCED_BLANK 0x0080

#define DISPSTAT_HBLANK_FLAG 0x0002
#define DISPSTAT_VCOUNT_FLAG 0x0004
#define DISPSTAT_HBLANK_IRQ 0x0010
#define DISPSTAT_VCOUNT_IRQ 0x0020
#define DISPSTAT_VCOUNT(n) ((n) << 8)

#define IRQ_HBLANK 0x0002
#define IRQ_VCOUNT 0x0004

#define BGCNT_SCREEN_BASE(n) ((n) << 8)

#define GREEN 0x03E0

static uint32_t argb(uint16_t bgr)
{
    uint32_t r = (bgr & 0x1F) << 3;
    uint32_t g = ((bgr >> 5) & 0x1F) << 3;
    uint32_t b = ((bgr >> 10) & 0x1F) << 3;

    r |= r >> 5;
    g |= g >> 5;
    b |= b >> 5;
    return (r << 16) | (g << 8) | b;
}

static void io16w(int offset, uint16_t value)
{
    *(volatile uint16_t *)(agb_mem.io + offset) = value;
}

static uint16_t io16r(int offset)
{
    return *(const volatile uint16_t *)(agb_mem.io + offset);
}

static uint32_t px(int x, int y)
{
    return agb_ppu_framebuffer()[y * agb_ppu_width() + x];
}

static int calls;
static int lines_seen[SCREEN_H + 4];
static int first_line = -1;

static void count_handler(void)
{
    if (calls < (int)(sizeof(lines_seen) / sizeof(lines_seen[0])))
        lines_seen[calls] = io16r(VCOUNT);
    if (first_line < 0)
        first_line = io16r(VCOUNT);
    calls++;
}

static void reset(const char *name)
{
    TEST_CASE(name);
    memset(&agb_mem, 0, sizeof(agb_mem));
    memset(gIntrTable, 0, sizeof(gIntrTable));
    memset(lines_seen, 0, sizeof(lines_seen));
    calls = 0;
    first_line = -1;
    io16w(IME, 1);
}

static void test_hblank_fires_once_per_line(void)
{
    reset("h-blank fires once per line");
    io16w(IE, IRQ_HBLANK);
    io16w(DISPSTAT, DISPSTAT_HBLANK_IRQ);
    gIntrTable[INTR_SLOT_HBLANK] = count_handler;
    agb_ppu_render_frame();

    CHECK(calls == SCREEN_H, "expected %d h-blanks, got %d", SCREEN_H, calls);
    CHECK(lines_seen[0] == 0, "the first h-blank was not on line 0, but %d", lines_seen[0]);
    CHECK(lines_seen[SCREEN_H - 1] == SCREEN_H - 1,
          "the last h-blank was not on line %d, but %d", SCREEN_H - 1, lines_seen[SCREEN_H - 1]);
}

// Three separate gates, and any one of them held shut stops the handler.
static void test_hblank_gates(void)
{
    reset("h-blank needs its DISPSTAT enable");
    io16w(IE, IRQ_HBLANK);
    io16w(DISPSTAT, 0);
    gIntrTable[INTR_SLOT_HBLANK] = count_handler;
    agb_ppu_render_frame();
    CHECK(calls == 0, "h-blank fired with its DISPSTAT enable clear (%d times)", calls);

    reset("h-blank needs its IE bit");
    io16w(IE, 0);
    io16w(DISPSTAT, DISPSTAT_HBLANK_IRQ);
    gIntrTable[INTR_SLOT_HBLANK] = count_handler;
    agb_ppu_render_frame();
    CHECK(calls == 0, "h-blank was serviced with its IE bit clear (%d times)", calls);

    reset("h-blank needs IME");
    io16w(IME, 0);
    io16w(IE, IRQ_HBLANK);
    io16w(DISPSTAT, DISPSTAT_HBLANK_IRQ);
    gIntrTable[INTR_SLOT_HBLANK] = count_handler;
    agb_ppu_render_frame();
    CHECK(calls == 0, "h-blank was serviced with IME clear (%d times)", calls);
    // Masked by IME is not the same as never requested: the flag still latches.
    CHECK((io16r(IF) & IRQ_HBLANK) != 0, "the request did not latch in IF while masked");
}

// V-count match fires on exactly the line the register names.
static void test_vcount_match(void)
{
    reset("v-count match");
    io16w(IE, IRQ_VCOUNT);
    io16w(DISPSTAT, DISPSTAT_VCOUNT_IRQ | DISPSTAT_VCOUNT(42));
    gIntrTable[INTR_SLOT_VCOUNT] = count_handler;
    agb_ppu_render_frame();

    CHECK(calls == 1, "expected one v-count match, got %d", calls);
    CHECK(first_line == 42, "v-count matched on line %d, not 42", first_line);

    reset("v-count needs its DISPSTAT enable");
    io16w(IE, IRQ_VCOUNT);
    io16w(DISPSTAT, DISPSTAT_VCOUNT(42)); // the setting, but nothing enabled
    gIntrTable[INTR_SLOT_VCOUNT] = count_handler;
    agb_ppu_render_frame();
    CHECK(calls == 0, "v-count fired with its DISPSTAT enable clear (%d times)", calls);
    CHECK((io16r(DISPSTAT) & DISPSTAT_VCOUNT_FLAG) == 0,
          "the v-count flag was left set on a line that does not match");

    reset("v-count past the last line never matches");
    io16w(IE, IRQ_VCOUNT);
    io16w(DISPSTAT, DISPSTAT_VCOUNT_IRQ | DISPSTAT_VCOUNT(200));
    gIntrTable[INTR_SLOT_VCOUNT] = count_handler;
    agb_ppu_render_frame();
    CHECK(calls == 0, "v-count matched a line that is never drawn (%d times)", calls);
}

// The status flags are readable whether or not anything is enabled to fire.
static void test_status_flags(void)
{
    reset("v-count flag without its interrupt");
    io16w(DISPSTAT, DISPSTAT_VCOUNT(159));
    agb_ppu_render_frame();
    CHECK((io16r(DISPSTAT) & DISPSTAT_VCOUNT_FLAG) != 0,
          "the v-count flag was not set on the matching line");
    CHECK((io16r(DISPSTAT) & DISPSTAT_HBLANK_FLAG) != 0,
          "the h-blank flag was not left set after the last line");
    CHECK(io16r(VCOUNT) == SCREEN_H - 1, "VCOUNT ended at %d, not %d",
          io16r(VCOUNT), SCREEN_H - 1);
}

// The point of the whole thing: a register written from an h-blank handler
// takes effect on the lines below it.
static int split_at = 80;

static void scroll_handler(void)
{
    if (io16r(VCOUNT) == split_at)
        io16w(BG0HOFS, 8);
}

static void test_midframe_register_write(void)
{
    reset("a write from h-blank changes the lines below it");
    io16w(DISPCNT, DISPCNT_BG0);
    io16w(BG0CNT, BGCNT_SCREEN_BASE(1));
    io16w(IE, IRQ_HBLANK);
    io16w(DISPSTAT, DISPSTAT_HBLANK_IRQ);
    gIntrTable[INTR_SLOT_HBLANK] = scroll_handler;

    // Tile 0 is blank; tile 1 is solid. The solid tile fills the map's second
    // column all the way down, so scrolling right by 8 brings it to the left
    // edge on every line rather than only the first.
    memset(agb_mem.vram + 32, 0x11, 32);
    for (int row = 0; row < 32; row++)
        *(volatile uint16_t *)(agb_mem.vram + 0x800 + (row * 32 + 1) * 2) = 1;
    *(volatile uint16_t *)(agb_mem.pltt + 2) = GREEN;
    agb_ppu_render_frame();

    CHECK(px(0, 0) == argb(0), "the top of the frame was already scrolled");
    CHECK(px(8, 0) == argb(GREEN), "the solid tile was not at x=8 before the split");
    // The handler runs after its own line is drawn, so the change lands on the
    // line below the one it fired on.
    CHECK(px(0, split_at) == argb(0), "the split line itself was scrolled");
    CHECK(px(0, split_at + 1) == argb(GREEN), "the scroll did not take effect below the split");
    CHECK(px(0, SCREEN_H - 1) == argb(GREEN), "the scroll did not persist to the bottom");
}

// DISPCNT is re-read per scanline like everything else, so a handler can blank
// the rest of the frame from partway down it.
static void blank_handler(void)
{
    if (io16r(VCOUNT) == split_at)
        io16w(DISPCNT, DISPCNT_FORCED_BLANK);
}

static void test_midframe_forced_blank(void)
{
    reset("forced blank switched on mid-frame");
    io16w(DISPCNT, DISPCNT_BG0);
    io16w(BG0CNT, BGCNT_SCREEN_BASE(1));
    io16w(IE, IRQ_HBLANK);
    io16w(DISPSTAT, DISPSTAT_HBLANK_IRQ);
    gIntrTable[INTR_SLOT_HBLANK] = blank_handler;

    memset(agb_mem.vram + 32, 0x11, 32);
    for (int row = 0; row < 32; row++)
        *(volatile uint16_t *)(agb_mem.vram + 0x800 + (row * 32 + 1) * 2) = 1;
    *(volatile uint16_t *)(agb_mem.pltt + 2) = GREEN;
    agb_ppu_render_frame();

    CHECK(px(8, 0) == argb(GREEN), "the top of the frame was blanked too");
    CHECK(px(8, split_at) == argb(GREEN), "the split line itself was blanked");
    CHECK(px(8, split_at + 1) == 0x00FFFFFF, "forced blank did not start below the split");
    CHECK(px(8, SCREEN_H - 1) == 0x00FFFFFF, "forced blank did not persist to the bottom");
}

// Forced blank stops the picture, not the scanline interrupts.
static void test_forced_blank_still_interrupts(void)
{
    reset("forced blank still raises h-blank");
    io16w(DISPCNT, DISPCNT_FORCED_BLANK);
    io16w(IE, IRQ_HBLANK);
    io16w(DISPSTAT, DISPSTAT_HBLANK_IRQ);
    gIntrTable[INTR_SLOT_HBLANK] = count_handler;
    agb_ppu_render_frame();

    CHECK(calls == SCREEN_H, "expected %d h-blanks under forced blank, got %d", SCREEN_H, calls);
    CHECK(px(0, 0) == 0x00FFFFFF, "forced blank did not whiten the frame");
}

int main(void)
{
    test_hblank_fires_once_per_line();
    test_hblank_gates();
    test_vcount_match();
    test_status_flags();
    test_midframe_register_write();
    test_midframe_forced_blank();
    test_forced_blank_still_interrupts();

    return test_report("ppu scanline interrupts");
}
