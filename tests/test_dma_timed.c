// DMA channels that wait for something.
//
// A channel armed with a start timing does nothing when it is armed; it runs
// when the display reaches the moment it names. The game's battle transitions
// are built on the H-blank one -- a single 16-bit write per line into WIN0H from
// a 160-entry buffer, which is what sweeps a window across the screen -- and so
// is everything scanline_effect.c does.
//
// The PPU drives H-blank, so a test can arm a channel and render a frame; that
// is the path the game actually takes, and it is checked here as well as the
// trigger in isolation.

#include <string.h>

#include "agb/dma.h"
#include "agb/memmap.h"
#include "agb/ppu.h"

#include "harness.h"

#define SCREEN_H 160

#define DMA0_CONTROL 0xB8
#define DMA_REG_STRIDE 12

#define DEST_INC 0x0000u
#define DEST_DEC 0x0020u
#define DEST_FIXED 0x0040u
#define DEST_RELOAD 0x0060u
#define SRC_DEC 0x0080u
#define SRC_FIXED 0x0100u
#define REPEAT 0x0200u
#define UNIT_32BIT 0x0400u
#define START_VBLANK 0x1000u
#define START_HBLANK 0x2000u
#define ENABLE 0x8000u

#define CONTROL(flags, count) (((uint32_t)(flags) << 16) | (uint32_t)(count))

// Somewhere in I/O for a destination, so a channel writing a register can be
// checked the way the game uses one, and a slab of EWRAM for sources.
#define DEST_REG 0x040
#define SOURCE (agb_mem.ewram + 0x100)

static uint16_t *source16(void)
{
    return (uint16_t *)SOURCE;
}

static uint16_t io16r(int offset)
{
    return *(const volatile uint16_t *)(agb_mem.io + offset);
}

static uint32_t control_of(int channel)
{
    return *(const volatile uint32_t *)(agb_mem.io + DMA0_CONTROL
                                        + DMA_REG_STRIDE * channel);
}

static void reset(const char *name)
{
    TEST_CASE(name);
    memset(&agb_mem, 0, sizeof(agb_mem));

    // More than a screen's worth: a scanline effect's buffer holds one entry per
    // line, and a test that ran off the end would be testing the memset behind it.
    for (int i = 0; i < SCREEN_H + 32; i++)
        source16()[i] = (uint16_t)(0x100 + i);
}

// One unit per line, source walking the buffer, destination pinned to one
// register: the shape every scanline effect uses.
static void test_hblank_feeds_one_entry_per_line(void)
{
    reset("an h-blank channel feeds one entry per line");
    agb_dma_set(0, source16(), agb_mem.io + DEST_REG,
                CONTROL(ENABLE | START_HBLANK | REPEAT | DEST_FIXED, 1));

    CHECK(io16r(DEST_REG) == 0, "arming the channel transferred something: %04X",
          io16r(DEST_REG));

    agb_dma_trigger(AGB_DMA_START_HBLANK);
    CHECK(io16r(DEST_REG) == 0x100, "expected the first entry, got %04X", io16r(DEST_REG));

    agb_dma_trigger(AGB_DMA_START_HBLANK);
    CHECK(io16r(DEST_REG) == 0x101, "expected the second entry, got %04X", io16r(DEST_REG));

    agb_dma_trigger(AGB_DMA_START_HBLANK);
    CHECK(io16r(DEST_REG) == 0x102, "expected the third entry, got %04X", io16r(DEST_REG));
}

// The whole point of the timing bits: nothing happens until the moment arrives.
static void test_arming_does_not_transfer(void)
{
    reset("arming a timed channel transfers nothing");
    agb_dma_set(1, source16(), agb_mem.io + DEST_REG,
                CONTROL(ENABLE | START_VBLANK | DEST_FIXED, 1));
    CHECK(io16r(DEST_REG) == 0, "a v-blank channel ran when it was armed");

    agb_dma_trigger(AGB_DMA_START_HBLANK);
    CHECK(io16r(DEST_REG) == 0, "a v-blank channel ran on an h-blank");

    agb_dma_trigger(AGB_DMA_START_VBLANK);
    CHECK(io16r(DEST_REG) == 0x100, "a v-blank channel did not run on a v-blank");
}

// Without the repeat bit a channel is spent after one go, and says so in its own
// enable bit -- which is how the game tells whether a transfer has happened.
static void test_without_repeat_a_channel_runs_once(void)
{
    reset("a channel without repeat runs once and disarms");
    agb_dma_set(2, source16(), agb_mem.io + DEST_REG,
                CONTROL(ENABLE | START_HBLANK | DEST_FIXED, 1));

    agb_dma_trigger(AGB_DMA_START_HBLANK);
    CHECK(io16r(DEST_REG) == 0x100, "the transfer did not happen");
    CHECK(!(control_of(2) & ((uint32_t)ENABLE << 16)),
          "the channel stayed armed after a one-shot transfer");

    agb_dma_trigger(AGB_DMA_START_HBLANK);
    CHECK(io16r(DEST_REG) == 0x100, "a disarmed channel ran again");
}

// A repeating channel with a walking destination has to put it back, or the
// second block lands after the first instead of on top of it.
static void test_destination_modes(void)
{
    uint16_t *out = (uint16_t *)(agb_mem.ewram + 0x800);

    reset("the destination walks, or holds, or reloads");
    agb_dma_set(0, source16(), out,
                CONTROL(ENABLE | START_HBLANK | REPEAT | DEST_INC, 4));
    agb_dma_trigger(AGB_DMA_START_HBLANK);
    agb_dma_trigger(AGB_DMA_START_HBLANK);
    CHECK(out[0] == 0x100 && out[3] == 0x103, "the first block did not land");
    CHECK(out[4] == 0x104 && out[7] == 0x107,
          "an incrementing destination did not carry on: %04X %04X", out[4], out[7]);

    reset("a reloading destination starts over");
    agb_dma_set(0, source16(), out,
                CONTROL(ENABLE | START_HBLANK | REPEAT | DEST_RELOAD, 4));
    agb_dma_trigger(AGB_DMA_START_HBLANK);
    agb_dma_trigger(AGB_DMA_START_HBLANK);
    CHECK(out[0] == 0x104 && out[3] == 0x107,
          "the second block did not land on the first: %04X %04X", out[0], out[3]);
}

static void test_source_modes(void)
{
    uint16_t *out = (uint16_t *)(agb_mem.ewram + 0x800);

    reset("a fixed source repeats one value");
    agb_dma_set(0, source16(), out,
                CONTROL(ENABLE | START_HBLANK | REPEAT | SRC_FIXED | DEST_INC, 3));
    agb_dma_trigger(AGB_DMA_START_HBLANK);
    CHECK(out[0] == 0x100 && out[1] == 0x100 && out[2] == 0x100,
          "a fixed source advanced: %04X %04X %04X", out[0], out[1], out[2]);

    reset("a descending source walks backwards");
    agb_dma_set(0, source16() + 8, out,
                CONTROL(ENABLE | START_HBLANK | REPEAT | SRC_DEC | DEST_INC, 3));
    agb_dma_trigger(AGB_DMA_START_HBLANK);
    CHECK(out[0] == 0x108 && out[1] == 0x107 && out[2] == 0x106,
          "a descending source did not: %04X %04X %04X", out[0], out[1], out[2]);
}

static void test_32_bit_units(void)
{
    uint32_t *out = (uint32_t *)(agb_mem.ewram + 0x800);
    uint32_t *in = (uint32_t *)SOURCE;

    reset("a 32-bit channel moves words");
    in[0] = 0xDEADBEEF;
    in[1] = 0x12345678;
    agb_dma_set(0, in, out,
                CONTROL(ENABLE | START_HBLANK | REPEAT | UNIT_32BIT | DEST_INC, 2));
    agb_dma_trigger(AGB_DMA_START_HBLANK);
    CHECK(out[0] == 0xDEADBEEF && out[1] == 0x12345678,
          "the words did not arrive: %08X %08X", out[0], out[1]);
}

// Channel 0 outranks channel 3, so when both write the same address the lower
// number goes first and the higher one has the last word.
static void test_channel_priority(void)
{
    uint16_t high = 0xAAAA;
    uint16_t low = 0x5555;

    reset("the lower-numbered channel goes first");
    agb_dma_set(0, &low, agb_mem.io + DEST_REG,
                CONTROL(ENABLE | START_HBLANK | REPEAT | SRC_FIXED | DEST_FIXED, 1));
    agb_dma_set(3, &high, agb_mem.io + DEST_REG,
                CONTROL(ENABLE | START_HBLANK | REPEAT | SRC_FIXED | DEST_FIXED, 1));
    agb_dma_trigger(AGB_DMA_START_HBLANK);
    CHECK(io16r(DEST_REG) == high, "channel 3 did not run last: %04X", io16r(DEST_REG));
}

// The path the game takes: the PPU drives the trigger, once per visible line.
static void test_a_rendered_frame_drives_the_channel(void)
{
    reset("a rendered frame feeds one entry per scanline");
    agb_dma_set(0, source16(), agb_mem.io + DEST_REG,
                CONTROL(ENABLE | START_HBLANK | REPEAT | DEST_FIXED, 1));
    agb_ppu_render_frame();

    CHECK(io16r(DEST_REG) == (uint16_t)(0x100 + SCREEN_H - 1),
          "expected entry %d after a frame, got %04X", SCREEN_H - 1, io16r(DEST_REG));
}

int main(void)
{
    test_hblank_feeds_one_entry_per_line();
    test_arming_does_not_transfer();
    test_without_repeat_a_channel_runs_once();
    test_destination_modes();
    test_source_modes();
    test_32_bit_units();
    test_channel_priority();
    test_a_rendered_frame_drives_the_channel();

    return test_report("timed dma");
}
