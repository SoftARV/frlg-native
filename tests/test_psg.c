// The four hardware sound channels.
//
// These are driven by register writes rather than by calls, so every test here
// sets registers and then asks for samples -- which is exactly how the sequencer
// uses them. Expected values come from the Game Boy hardware's documented
// behaviour, which the GBA carries forward unchanged.

#include <string.h>

#include "agb/memmap.h"
#include "agb/psg.h"

#include "gba/io_reg.h"

#include "harness.h"

#define RATE 13379
#define FRAME 64

// A frequency register giving about 885 Hz, so a 64-sample frame at the mixer's
// rate holds several periods. A low tone can sit entirely within one half of its
// duty cycle for a whole frame, which looks exactly like silence.
#define TONE 1900

static int8_t right[FRAME];
static int8_t left[FRAME];

static void put(int offset, uint16_t value)
{
    memcpy(agb_mem.io + offset, &value, sizeof(value));
}

static uint16_t get(int offset)
{
    uint16_t value;

    memcpy(&value, agb_mem.io + offset, sizeof(value));
    return value;
}

static void reset(const char *name)
{
    TEST_CASE(name);
    memset(agb_mem.io, 0, 0x100);
    memset(right, 0, sizeof(right));
    memset(left, 0, sizeof(left));
    agb_psg_reset();

    // Master on, both sides at full volume, every channel routed to both, and
    // the PSG at its full share against direct sound.
    put(REG_OFFSET_SOUNDCNT_X, 0x0080);
    put(REG_OFFSET_SOUNDCNT_L, 0xFF77);
    put(REG_OFFSET_SOUNDCNT_H, 0x0002);
}

// How far the samples swing, which is what "is this channel sounding" means.
static int span(const int8_t *buf, int n)
{
    int lo = buf[0], hi = buf[0];

    for (int i = 1; i < n; i++)
    {
        if (buf[i] < lo) lo = buf[i];
        if (buf[i] > hi) hi = buf[i];
    }
    return hi - lo;
}

// Nothing sounds until a channel is triggered, however its other registers are
// set: the sequencer sets a voice up and starts it separately.
static void test_silent_until_triggered(void)
{
    reset("an untriggered channel is silent");
    put(REG_OFFSET_SOUND1CNT_H, 0xF080); // full volume, 50% duty
    put(REG_OFFSET_SOUND1CNT_X, TONE);   // a frequency, but no trigger

    agb_psg_mix(right, left, FRAME, RATE);

    CHECK(span(right, FRAME) == 0, "an untriggered channel produced %d of swing",
          span(right, FRAME));
}

static void test_square_sounds(void)
{
    reset("a triggered square sounds");
    put(REG_OFFSET_SOUND1CNT_H, 0xF080);
    put(REG_OFFSET_SOUND1CNT_X, 0x8000 | TONE);

    agb_psg_mix(right, left, FRAME, RATE);

    CHECK(span(right, FRAME) > 0, "a triggered square produced nothing");
    CHECK(span(left, FRAME) > 0, "it reached only one side");
}

// The trigger is write-only on the hardware: it starts the channel and reads
// back as zero.
static void test_trigger_is_consumed(void)
{
    reset("the trigger bit is taken, not left standing");
    put(REG_OFFSET_SOUND1CNT_H, 0xF080);
    put(REG_OFFSET_SOUND1CNT_X, 0x8000 | TONE);

    agb_psg_mix(right, left, FRAME, RATE);

    CHECK(!(get(REG_OFFSET_SOUND1CNT_X) & 0x8000), "the trigger bit survived");
    CHECK((get(REG_OFFSET_SOUND1CNT_X) & 0x7FF) == TONE, "the frequency was disturbed");
}

// A square at full volume swings its whole four-bit range; a quieter envelope
// swings less.
static void test_envelope_volume(void)
{
    int loud, quiet;

    reset("a louder envelope swings further");
    put(REG_OFFSET_SOUND1CNT_H, 0xF080);
    put(REG_OFFSET_SOUND1CNT_X, 0x8000 | TONE);
    agb_psg_mix(right, left, FRAME, RATE);
    loud = span(right, FRAME);

    reset("a quieter envelope swings less");
    put(REG_OFFSET_SOUND1CNT_H, 0x4080); // a quarter of the volume
    put(REG_OFFSET_SOUND1CNT_X, 0x8000 | TONE);
    agb_psg_mix(right, left, FRAME, RATE);
    quiet = span(right, FRAME);

    CHECK(quiet < loud, "the quieter envelope swung %d against %d", quiet, loud);
    CHECK(quiet > 0, "the quieter envelope was silent");
}

// The envelope steps down over time, so a channel left running fades.
static void test_envelope_decays(void)
{
    int first, later;

    reset("an envelope with a step decays");
    // Full volume, falling, the fastest step.
    put(REG_OFFSET_SOUND1CNT_H, 0xF180);
    put(REG_OFFSET_SOUND1CNT_X, 0x8000 | TONE);

    agb_psg_mix(right, left, FRAME, RATE);
    first = span(right, FRAME);

    // Long enough for many envelope steps at 64 Hz.
    for (int i = 0; i < 40; i++)
    {
        memset(right, 0, sizeof(right));
        agb_psg_mix(right, left, FRAME, RATE);
    }
    later = span(right, FRAME);

    CHECK(later < first, "the envelope did not decay: %d then %d", first, later);
}

// The length counter switches a channel off when it runs out, but only when the
// channel asked for it.
static void test_length_expires(void)
{
    reset("a short length silences the channel");
    put(REG_OFFSET_SOUND1CNT_H, 0xF03F); // volume held, the shortest length
    put(REG_OFFSET_SOUND1CNT_X, 0xC000 | TONE); // trigger, length enabled

    // The length runs at 256 Hz, so one tick is plenty.
    for (int i = 0; i < 20; i++)
    {
        memset(right, 0, sizeof(right));
        agb_psg_mix(right, left, FRAME, RATE);
    }

    CHECK(span(right, FRAME) == 0, "the channel still swings %d after its length",
          span(right, FRAME));

    reset("without the length bit the channel holds");
    put(REG_OFFSET_SOUND1CNT_H, 0xF03F);
    put(REG_OFFSET_SOUND1CNT_X, 0x8000 | TONE); // trigger, no length enable
    for (int i = 0; i < 20; i++)
    {
        memset(right, 0, sizeof(right));
        agb_psg_mix(right, left, FRAME, RATE);
    }
    CHECK(span(right, FRAME) > 0, "the channel stopped although its length was off");
}

// The wave channel plays what is in wave RAM, and says nothing when the table is
// flat.
static void test_wave_channel(void)
{
    reset("the wave channel plays its table");
    for (int i = 0; i < 16; i++)
        agb_mem.io[REG_OFFSET_WAVE_RAM0 + i] = (uint8_t)((i & 1) ? 0x00 : 0xFF);
    put(REG_OFFSET_SOUND3CNT_L, 0x0080);        // the channel's own enable
    put(REG_OFFSET_SOUND3CNT_H, 0x2000);        // full volume
    put(REG_OFFSET_SOUND3CNT_X, 0x8000 | TONE);

    agb_psg_mix(right, left, FRAME, RATE);

    CHECK(span(right, FRAME) > 0, "the wave channel produced nothing");

    reset("a flat wave table is silent");
    for (int i = 0; i < 16; i++)
        agb_mem.io[REG_OFFSET_WAVE_RAM0 + i] = 0x88; // the midpoint, both nibbles
    put(REG_OFFSET_SOUND3CNT_L, 0x0080);
    put(REG_OFFSET_SOUND3CNT_H, 0x2000);
    put(REG_OFFSET_SOUND3CNT_X, 0x8000 | TONE);
    agb_psg_mix(right, left, FRAME, RATE);
    CHECK(span(right, FRAME) == 0, "a flat table swung %d", span(right, FRAME));
}

// The wave channel is held off by its own enable bit whatever else is set.
static void test_wave_enable_bit(void)
{
    reset("the wave channel obeys its enable bit");
    for (int i = 0; i < 16; i++)
        agb_mem.io[REG_OFFSET_WAVE_RAM0 + i] = (uint8_t)((i & 1) ? 0x00 : 0xFF);
    put(REG_OFFSET_SOUND3CNT_L, 0x0000); // off
    put(REG_OFFSET_SOUND3CNT_H, 0x2000);
    put(REG_OFFSET_SOUND3CNT_X, 0x8000 | TONE);

    agb_psg_mix(right, left, FRAME, RATE);

    CHECK(span(right, FRAME) == 0, "a disabled wave channel swung %d",
          span(right, FRAME));
}

static void test_noise_channel(void)
{
    reset("the noise channel sounds");
    put(REG_OFFSET_SOUND4CNT_L, 0xF000);        // full volume, held
    put(REG_OFFSET_SOUND4CNT_H, 0x8000 | 0x30); // trigger, a middling rate

    agb_psg_mix(right, left, FRAME, RATE);

    CHECK(span(right, FRAME) > 0, "the noise channel produced nothing");
}

// The master switch silences everything, and the per-side routing decides which
// ear a channel reaches.
static void test_master_controls(void)
{
    reset("the master switch silences the lot");
    put(REG_OFFSET_SOUNDCNT_X, 0x0000);
    put(REG_OFFSET_SOUND1CNT_H, 0xF080);
    put(REG_OFFSET_SOUND1CNT_X, 0x8000 | TONE);
    agb_psg_mix(right, left, FRAME, RATE);
    CHECK(span(right, FRAME) == 0, "sound came through with the master off");

    reset("routing decides which side a channel reaches");
    put(REG_OFFSET_SOUNDCNT_L, 0x0177); // channel one on the right only
    put(REG_OFFSET_SOUND1CNT_H, 0xF080);
    put(REG_OFFSET_SOUND1CNT_X, 0x8000 | TONE);
    agb_psg_mix(right, left, FRAME, RATE);
    CHECK(span(right, FRAME) > 0, "the right side got nothing");
    CHECK(span(left, FRAME) == 0, "the left side got %d and should have got none",
          span(left, FRAME));
}

// The PSG is added to what the software mixer already put there rather than
// replacing it.
static void test_adds_to_existing(void)
{
    reset("the psg adds to the buffer it is given");
    for (int i = 0; i < FRAME; i++)
        right[i] = 40;
    put(REG_OFFSET_SOUND1CNT_H, 0xF080);
    put(REG_OFFSET_SOUND1CNT_X, 0x8000 | TONE);

    agb_psg_mix(right, left, FRAME, RATE);

    int away = 0;
    for (int i = 0; i < FRAME; i++)
        if (right[i] != 40)
            away++;
    CHECK(away > 0, "the buffer was left exactly as it was");
    CHECK(span(right, FRAME) > 0, "the existing content was replaced by silence");
}

// A sample is an interval, not an instant, and a channel can flip more than once
// inside one. Reading the level at a single point of it keeps energy the speaker
// never sees, which is aliasing: the tone comes back as something else, at full
// amplitude, instead of cancelling. The mix runs at 13379 Hz, so anything above
// about 6.7 kHz cannot be represented and should fade rather than fold back.
static int energy(const int8_t *buf, int n)
{
    long sum = 0;

    for (int i = 0; i < n; i++)
        sum += (long)buf[i] * buf[i];
    return (int)(sum / n);
}

static void test_a_square_below_nyquist_keeps_its_swing(void)
{
    reset("a square below the mix rate is untouched");
    put(REG_OFFSET_SOUND1CNT_H, 0xF080);
    put(REG_OFFSET_SOUND1CNT_X, 0x8000 | TONE);   // about 885 Hz
    agb_psg_mix(right, left, FRAME, RATE);

    CHECK(span(right, FRAME) > 20, "an audible tone came out quiet: span %d",
          span(right, FRAME));
}

// 2047 is the highest the frequency register goes, 131 kHz, ten times the mix
// rate. Every sample spans whole cycles, and a square's two levels are weighted
// to cancel across one, so almost nothing should be left.
static void test_a_square_above_nyquist_does_not_alias(void)
{
    int quiet;

    reset("a square above the mix rate fades instead of folding back");
    put(REG_OFFSET_SOUND1CNT_H, 0xF080);
    put(REG_OFFSET_SOUND1CNT_X, 0x8000 | 2047);
    agb_psg_mix(right, left, FRAME, RATE);
    quiet = span(right, FRAME);

    reset("and the audible one it is measured against");
    put(REG_OFFSET_SOUND1CNT_H, 0xF080);
    put(REG_OFFSET_SOUND1CNT_X, 0x8000 | TONE);
    agb_psg_mix(right, left, FRAME, RATE);

    CHECK(quiet * 3 < span(right, FRAME),
          "a tone far above the mix rate came back nearly as loud as an audible "
          "one: %d against %d", quiet, span(right, FRAME));
}

// The noise channel is the one that was doing this in play: its clock reaches
// 262 kHz, twenty times the mix rate, and the drums came out too loud and harsh
// with the effects buried under them. Faster than the mix rate, it has to
// average down rather than stay at full amplitude.
static void test_noise_above_nyquist_averages_down(void)
{
    int fast, slow;

    reset("the fastest noise clock");
    put(REG_OFFSET_SOUND4CNT_L, 0xF000);           // full volume, no decay
    put(REG_OFFSET_SOUND4CNT_H, 0x8004);           // shift 0
    agb_psg_mix(right, left, FRAME, RATE);
    fast = energy(right, FRAME);

    reset("a clock the mix rate can follow");
    put(REG_OFFSET_SOUND4CNT_L, 0xF000);
    put(REG_OFFSET_SOUND4CNT_H, 0x8044);           // shift 4
    agb_psg_mix(right, left, FRAME, RATE);
    slow = energy(right, FRAME);

    CHECK(slow > 0, "the slower noise should sound at all");
    CHECK(fast * 10 < slow * 8,
          "noise far above the mix rate should average down: %d against %d",
          fast, slow);
}

int main(void)
{
    test_silent_until_triggered();
    test_square_sounds();
    test_trigger_is_consumed();
    test_envelope_volume();
    test_envelope_decays();
    test_length_expires();
    test_wave_channel();
    test_wave_enable_bit();
    test_noise_channel();
    test_master_controls();
    test_adds_to_existing();
    test_a_square_below_nyquist_keeps_its_swing();
    test_a_square_above_nyquist_does_not_alias();
    test_noise_above_nyquist_averages_down();

    return test_report("psg");
}
