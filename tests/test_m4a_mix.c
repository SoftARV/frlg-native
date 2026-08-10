// The fixed-frequency mixing path: walking a wave one sample per output sample
// and accumulating it into the two PCM buffers.
//
// Expected values are worked out by hand from the original ARM. The one that
// matters most is that the accumulate wraps at eight bits rather than clamping:
// the original packs four samples in a register and rotates them past an
// accumulator, and nothing in that arrangement saturates.

#include <stddef.h>
#include <string.h>

#include "agb/m4a.h"

#include "harness.h"

#define SF_LOOP 0x10
#define SF_ENV_SUSTAIN 0x01

#define FRAME 8
#define FW_FRACTION_BITS 23

static struct SoundChannel chan;
static s8 right[FRAME];
static s8 left[FRAME];

// A wave with room for its samples inline, which the game's struct declares as
// a one-element array.
static union
{
    struct WaveData header;
    unsigned char raw[sizeof(struct WaveData) + 64];
} wave;

// The game declares the sample array as one element and relies on the wave
// being longer, so the bytes are reached through the union's own storage
// rather than by indexing past the end of the declared array.
static s8 *sample_storage(void)
{
    return (s8 *)(wave.raw + offsetof(struct WaveData, data));
}

static void reset(const char *name, int count)
{
    TEST_CASE(name);
    memset(&chan, 0, sizeof(chan));
    memset(&wave, 0, sizeof(wave));
    memset(right, 0, sizeof(right));
    memset(left, 0, sizeof(left));

    wave.header.size = count;
    chan.wav = &wave.header;
    chan.currentPointer = sample_storage();
    chan.count = count;
    chan.statusFlags = SF_ENV_SUSTAIN;
    chan.envelopeVolumeRight = 255;
    chan.envelopeVolumeLeft = 255;
}

static void set_samples(const int *values, int n)
{
    s8 *out = sample_storage();

    for (int i = 0; i < n; i++)
        out[i] = (s8)values[i];
}

// Volume scales each sample by (volume * sample) >> 8 before it lands.
static void test_scaling(void)
{
    const int samples[4] = {64, -64, 127, -128};

    reset("full volume", 8); // longer than the frame, so it does not end here
    set_samples(samples, 4);
    chan.envelopeVolumeRight = 255;
    chan.envelopeVolumeLeft = 128;
    CHECK(agb_m4a_mix_fixed(&chan, right, left, 4), "the wave ended early");

    // (255*64)>>8 = 63; (255*-64)>>8 = -64; (255*127)>>8 = 126; (255*-128)>>8 = -128.
    CHECK(right[0] == 63, "right[0] was %d, not 63", right[0]);
    CHECK(right[1] == -64, "right[1] was %d, not -64", right[1]);
    CHECK(right[2] == 126, "right[2] was %d, not 126", right[2]);
    CHECK(right[3] == -128, "right[3] was %d, not -128", right[3]);

    // The left side takes the same samples at its own volume: (128*64)>>8 = 32.
    CHECK(left[0] == 32, "left[0] was %d, not 32", left[0]);
    CHECK(left[1] == -32, "left[1] was %d, not -32", left[1]);

    reset("silent channel contributes nothing", 8);
    set_samples(samples, 4);
    chan.envelopeVolumeRight = 0;
    chan.envelopeVolumeLeft = 0;
    agb_m4a_mix_fixed(&chan, right, left, 4);
    for (int i = 0; i < 4; i++)
        CHECK(right[i] == 0 && left[i] == 0, "a silent channel wrote %d at %d", right[i], i);
}

// Channels accumulate into a shared buffer rather than replacing it.
static void test_accumulates(void)
{
    const int samples[2] = {64, 64};

    reset("mixing adds to what is already there", 8);
    set_samples(samples, 2);
    right[0] = 10;
    left[0] = -10;
    agb_m4a_mix_fixed(&chan, right, left, 2);

    CHECK(right[0] == 10 + 63, "expected the sum, got %d", right[0]);
    CHECK(left[0] == -10 + 63, "expected the sum, got %d", left[0]);
}

// Eight bits, wrapping. Two loud channels do not flatten into a ceiling.
static void test_wraps_rather_than_clamps(void)
{
    const int samples[1] = {127};

    reset("a loud sum wraps", 8);
    set_samples(samples, 1);
    right[0] = 100; // 100 + 126 = 226, which as a signed byte is -30
    agb_m4a_mix_fixed(&chan, right, left, 1);

    CHECK(right[0] == -30, "expected the sum to wrap to -30, got %d", right[0]);
}

// The sample pointer and remaining count carry across frames.
static void test_advances_across_frames(void)
{
    const int samples[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    reset("a wave is walked across frames", 8);
    set_samples(samples, 8);
    chan.envelopeVolumeRight = 255;

    CHECK(agb_m4a_mix_fixed(&chan, right, left, 4), "the wave ended early");
    CHECK(chan.count == 4, "expected 4 samples left, got %u", (unsigned)chan.count);
    CHECK(chan.currentPointer == sample_storage() + 4, "the pointer did not advance by 4");

    memset(right, 0, sizeof(right));
    // Spending the last sample releases the channel, even though the frame was
    // exactly filled: the count is checked after every sample, not at the end.
    CHECK(!agb_m4a_mix_fixed(&chan, right, left, 4), "a spent wave stayed active");
    // The second frame starts at sample 5: (255*5)>>8 = 4.
    CHECK(right[0] == 4, "the second frame restarted the wave, got %d", right[0]);
    CHECK(right[3] == 7, "the last sample did not sound, got %d", right[3]);
    CHECK(chan.statusFlags == 0, "the channel was not released");
}

// Filling a frame exactly still ends the wave, because the count is spent on
// the last sample rather than checked before the next one.
static void test_exact_fill_ends(void)
{
    const int samples[4] = {127, 127, 127, 127};

    reset("a wave exactly filling a frame still ends", 4);
    set_samples(samples, 4);
    CHECK(!agb_m4a_mix_fixed(&chan, right, left, 4), "an exactly spent wave stayed active");
    CHECK(chan.statusFlags == 0, "the channel was not released");
    for (int i = 0; i < 4; i++)
        CHECK(right[i] == 126, "sample %d did not sound, got %d", i, right[i]);
}

// Running out with no loop releases the channel, and the rest of the frame
// gets nothing from it.
static void test_end_of_wave(void)
{
    const int samples[2] = {127, 127};

    reset("a wave that ends releases the channel", 2);
    set_samples(samples, 2);
    CHECK(!agb_m4a_mix_fixed(&chan, right, left, FRAME), "a spent wave kept going");
    CHECK(chan.statusFlags == 0, "the channel was not released");

    CHECK(right[0] == 126 && right[1] == 126, "the wave's own samples did not sound");
    for (int i = 2; i < FRAME; i++)
        CHECK(right[i] == 0, "sample %d was written past the end of the wave: %d", i, right[i]);
}

// A looping wave restarts at loopStart rather than at the beginning.
static void test_loop(void)
{
    const int samples[4] = {10, 20, 30, 40};

    reset("a looping wave restarts at its loop point", 4);
    set_samples(samples, 4);
    chan.statusFlags = SF_ENV_SUSTAIN | SF_LOOP;
    wave.header.loopStart = 2;
    chan.envelopeVolumeRight = 255;

    CHECK(agb_m4a_mix_fixed(&chan, right, left, 8), "a looping wave ended");

    // 10, 20, 30, 40 then back to sample 2: 30, 40, 30, 40.
    const int expect[8] = {9, 19, 29, 39, 29, 39, 29, 39};
    for (int i = 0; i < 8; i++)
        CHECK(right[i] == expect[i], "sample %d was %d, expected %d", i, right[i], expect[i]);

    CHECK(chan.count == 2, "the loop left the wrong count, %u", (unsigned)chan.count);
    CHECK(chan.statusFlags & SF_LOOP, "the loop flag was lost");
}

// A wave whose loop point is its start repeats the whole thing.
static void test_loop_from_start(void)
{
    const int samples[2] = {64, -64};

    reset("a wave looping from its start repeats whole", 2);
    set_samples(samples, 2);
    chan.statusFlags = SF_ENV_SUSTAIN | SF_LOOP;
    wave.header.loopStart = 0;

    agb_m4a_mix_fixed(&chan, right, left, 4);
    CHECK(right[0] == 63 && right[1] == -64, "the first pass was wrong");
    CHECK(right[2] == 63 && right[3] == -64, "the wave did not repeat from its start");
}

// ---------------------------------------------------------------- pitched ---
//
// The step is divFreq * frequency in 9.23, so a step of 1<<23 walks the wave at
// its own rate and interpolates nothing.
#define ONE_STEP (1 << FW_FRACTION_BITS)

static struct SoundInfo info;

static void reset_pitched(const char *name, int count, uint32_t step)
{
    reset(name, count);
    memset(&info, 0, sizeof(info));
    // Any pair whose product is the wanted step will do.
    info.divFreq = (int32_t)step;
    chan.frequency = 1;
    chan.fw = 0;
}

// At the wave's own rate the output is the wave, sample for sample.
static void test_pitched_unit_step(void)
{
    const int samples[8] = {0, 32, 64, 96, 127, -32, -64, -128};

    reset_pitched("pitched at the wave's own rate", 16, ONE_STEP);
    set_samples(samples, 8);
    CHECK(agb_m4a_mix_pitched(&info, &chan, right, left, 8), "the wave ended early");

    for (int i = 0; i < 8; i++)
    {
        int expect = (255 * samples[i]) >> 8;

        CHECK(right[i] == expect, "sample %d was %d, expected %d", i, right[i], expect);
    }
    CHECK(chan.fw == 0, "a whole-sample step left a fraction behind: %u", (unsigned)chan.fw);
}

// Half the rate puts an interpolated sample between each pair.
static void test_pitched_half_step(void)
{
    const int samples[4] = {0, 64, 0, -64};

    reset_pitched("pitched at half rate interpolates", 16, ONE_STEP / 2);
    set_samples(samples, 4);
    agb_m4a_mix_pitched(&info, &chan, right, left, 6);

    // 0, then halfway to 64 is 32, then 64, then halfway back to 0 is 32, ...
    const int source[6] = {0, 32, 64, 32, 0, -32};
    for (int i = 0; i < 6; i++)
    {
        int expect = (255 * source[i]) >> 8;

        CHECK(right[i] == expect, "sample %d was %d, expected %d (from %d)", i, right[i],
              expect, source[i]);
    }
}

// Double the rate takes every other sample and skips the one between.
static void test_pitched_double_step(void)
{
    const int samples[8] = {0, 99, 32, 99, 64, 99, 96, 99};

    reset_pitched("pitched at double rate skips samples", 16, ONE_STEP * 2);
    set_samples(samples, 8);
    agb_m4a_mix_pitched(&info, &chan, right, left, 4);

    const int source[4] = {0, 32, 64, 96};
    for (int i = 0; i < 4; i++)
    {
        int expect = (255 * source[i]) >> 8;

        CHECK(right[i] == expect, "sample %d was %d, expected %d", i, right[i], expect);
    }
    CHECK(chan.count == 16 - 8, "double rate consumed %u samples, not 8",
          (unsigned)(16 - chan.count));
}

// The fractional position carries across frames rather than restarting.
static void test_pitched_phase_persists(void)
{
    // A rising ramp rather than an alternating pair: with a symmetric wave the
    // midpoint is the same whichever sample you resume from, which hides a
    // pointer left one place along.
    const int samples[8] = {0, 40, 80, 120, 80, 40, 0, -40};

    reset_pitched("the fractional position carries over", 16, ONE_STEP / 2);
    set_samples(samples, 8);
    agb_m4a_mix_pitched(&info, &chan, right, left, 1);
    CHECK(chan.fw == ONE_STEP / 2, "expected a half-sample phase, got %u", (unsigned)chan.fw);
    CHECK(chan.currentPointer == sample_storage(),
          "the pointer left the sample still being played");

    memset(right, 0, sizeof(right));
    agb_m4a_mix_pitched(&info, &chan, right, left, 1);
    // Halfway between sample 0 and sample 1 is 20, not 60.
    CHECK(right[0] == ((255 * 20) >> 8), "the second frame resumed in the wrong place, got %d",
          right[0]);
}

// Running out with no loop releases the channel part-way through the frame.
static void test_pitched_end(void)
{
    const int samples[2] = {127, 127};

    reset_pitched("a pitched wave that ends releases the channel", 2, ONE_STEP);
    set_samples(samples, 2);
    CHECK(!agb_m4a_mix_pitched(&info, &chan, right, left, FRAME), "a spent wave kept going");
    CHECK(chan.statusFlags == 0, "the channel was not released");
    CHECK(right[0] == 126, "the first sample did not sound");
    CHECK(right[FRAME - 1] == 0, "samples were written past the end of the wave");
}

// Overrunning the loop point resumes the right distance into the loop, even
// when the step jumps clean past the whole loop more than once.
static void test_pitched_loop_overrun(void)
{
    const int samples[6] = {0, 0, 10, 20, 30, 40};

    reset_pitched("a pitched loop resumes at the right offset", 6, ONE_STEP * 4);
    set_samples(samples, 6);
    chan.statusFlags = SF_ENV_SUSTAIN | SF_LOOP;
    wave.header.loopStart = 2;   // the loop is samples 2..5, four long
    chan.envelopeVolumeRight = 255;

    // Four samples a step: 0, then 4, then 8 -- which is two past the end of a
    // six-sample wave, so it resumes two into a four-sample loop, at sample 4.
    agb_m4a_mix_pitched(&info, &chan, right, left, 3);

    const int source[3] = {0, 30, 30};
    for (int i = 0; i < 3; i++)
    {
        int expect = (255 * source[i]) >> 8;

        CHECK(right[i] == expect, "sample %d was %d, expected %d (from %d)", i, right[i],
              expect, source[i]);
    }
    CHECK(chan.statusFlags & SF_LOOP, "the loop flag was lost");
}

// A step that clears the whole loop more than once has to keep rewinding until
// it lands inside it, which is the only reason that rewind is a loop.
static void test_pitched_loop_overrun_twice(void)
{
    const int samples[6] = {0, 0, 0, 0, 50, 60};

    reset_pitched("a pitched loop rewinds more than once", 6, ONE_STEP * 8);
    set_samples(samples, 6);
    chan.statusFlags = SF_ENV_SUSTAIN | SF_LOOP;
    wave.header.loopStart = 4;   // a two-sample loop, samples 4 and 5
    chan.envelopeVolumeRight = 255;

    // Eight samples a step overshoots a six-sample wave by two, which is a whole
    // two-sample loop plus nothing: it lands back at the loop's first sample.
    agb_m4a_mix_pitched(&info, &chan, right, left, 2);

    CHECK(right[0] == 0, "the first sample was not the wave's own start");
    CHECK(right[1] == ((255 * 50) >> 8), "did not land on the loop's first sample, got %d",
          right[1]);
    CHECK(chan.statusFlags & SF_LOOP, "the loop flag was lost");
}

// ------------------------------------------------------------- reversed waves ---

#define SF_SPECIAL 0x20
#define TYPE_FIX 0x08
#define TYPE_REV 0x10

// A reversed channel starts where a forward one would and is turned round on
// its first mix, so the fixture is the ordinary one plus the type bit.
static void reset_reversed(const char *name, int count, uint32_t step)
{
    reset_pitched(name, count, step);
    chan.type = TYPE_REV;
}

// The play position is turned round to the same distance from the end, and only
// once however many frames the note lasts.
static void test_reversed_turns_round_once(void)
{
    const int samples[8] = {0, 1, 2, 3, 4, 5, 6, 7};

    reset_reversed("a reversed wave is turned round on its first frame", 8, ONE_STEP);
    set_samples(samples, 8);
    CHECK(chan.currentPointer == sample_storage(), "the fixture did not start at the front");

    agb_m4a_mix_reversed(&info, &chan, right, left, 2);

    CHECK(chan.statusFlags & SF_SPECIAL, "the channel was not marked as turned round");
    // Two samples in from the end, plus the one the pointer sits past.
    CHECK(chan.currentPointer == sample_storage() + 8 - 2,
          "the position came out %d samples in, not 6",
          (int)(chan.currentPointer - sample_storage()));

    // A second frame must carry on rather than turn round again.
    agb_m4a_mix_reversed(&info, &chan, right, left, 2);
    CHECK(chan.currentPointer == sample_storage() + 8 - 4,
          "the second frame turned the position round again, it is at %d",
          (int)(chan.currentPointer - sample_storage()));
}

// At the wave's own rate a reversed wave is the wave backwards.
static void test_reversed_plays_backwards(void)
{
    const int samples[8] = {0, 1, 2, 3, 4, 5, 6, 7};

    reset_reversed("a reversed wave plays from the end", 8, ONE_STEP);
    set_samples(samples, 8);

    agb_m4a_mix_reversed(&info, &chan, right, left, 6);

    for (int i = 0; i < 6; i++)
    {
        int expect = (255 * samples[7 - i]) >> 8;

        CHECK(right[i] == expect, "sample %d was %d, expected %d (the wave backwards)",
              i, right[i], expect);
        CHECK(left[i] == expect, "the left side disagreed at sample %d", i);
    }
}

// The fixed-frequency variant is handled here too, as a step of exactly one
// sample whatever the channel's frequency says.
static void test_reversed_fixed_ignores_frequency(void)
{
    const int samples[8] = {0, 1, 2, 3, 4, 5, 6, 7};

    reset_reversed("a fixed reversed wave steps one sample", 8, ONE_STEP * 4);
    chan.type = TYPE_REV | TYPE_FIX;
    set_samples(samples, 8);

    agb_m4a_mix_reversed(&info, &chan, right, left, 4);

    // A step of four would have taken samples 7, 3 and then run out.
    for (int i = 0; i < 4; i++)
    {
        int expect = (255 * samples[7 - i]) >> 8;

        CHECK(right[i] == expect, "sample %d was %d, expected %d", i, right[i], expect);
    }
}

// Half the rate puts an interpolated sample between each pair, going backwards.
static void test_reversed_interpolates(void)
{
    const int samples[4] = {0, 64, 0, -64};

    reset_reversed("a reversed wave interpolates", 4, ONE_STEP / 2);
    set_samples(samples, 4);

    agb_m4a_mix_reversed(&info, &chan, right, left, 4);

    // Backwards the wave is -64, 0, 64, 0; halfway between -64 and 0 is -32,
    // and between 0 and 64 is 32.
    const int expect[4] = {-64, -32, 0, 32};

    for (int i = 0; i < 4; i++)
    {
        int want = (255 * expect[i]) >> 8;

        CHECK(right[i] == want, "sample %d was %d, expected %d", i, right[i], want);
    }
}

// A note can be told to start partway into its wave, and the turn-round has to
// mirror that -- not simply drop the position at the end.
static void test_reversed_turns_round_from_partway(void)
{
    const int samples[8] = {0, 1, 2, 3, 4, 5, 6, 7};

    reset_reversed("a reversed wave started partway in", 8, ONE_STEP);
    set_samples(samples, 8);
    // Two samples in, as ply_note leaves a channel given a start offset.
    chan.currentPointer = sample_storage() + 2;
    chan.count = 8 - 2;

    agb_m4a_mix_reversed(&info, &chan, right, left, 2);

    // Mirrored to two from the end, so the first sample played is 5, not 7.
    CHECK(right[0] == ((255 * 5) >> 8), "the first sample was %d, expected %d",
          right[0], (255 * 5) >> 8);
    CHECK(right[1] == ((255 * 4) >> 8), "the second sample was %d, expected %d",
          right[1], (255 * 4) >> 8);
}

// A step of more than one sample skips, rather than advancing one at a time.
static void test_reversed_multi_sample_step(void)
{
    const int samples[8] = {0, 1, 2, 3, 4, 5, 6, 7};

    reset_reversed("a reversed wave at twice the rate", 8, ONE_STEP * 2);
    set_samples(samples, 8);

    agb_m4a_mix_reversed(&info, &chan, right, left, 4);

    // Two samples back each time: 7, 5, 3, 1 -- not 7, 6, 5, 4.
    const int expect[4] = {7, 5, 3, 1};

    for (int i = 0; i < 4; i++)
    {
        int want = (255 * expect[i]) >> 8;

        CHECK(right[i] == want, "sample %d was %d, expected %d", i, right[i], want);
    }
}

// The remaining count is written back, so the next frame knows how much is left.
static void test_reversed_count_carries(void)
{
    const int samples[8] = {0, 1, 2, 3, 4, 5, 6, 7};

    reset_reversed("a reversed wave carries its remaining count", 8, ONE_STEP);
    set_samples(samples, 8);

    agb_m4a_mix_reversed(&info, &chan, right, left, 3);

    CHECK(chan.count == 8 - 3, "the count came out %u, not 5", (unsigned)chan.count);
}

// A reversed wave does not loop, however loud the wave's own flags are about it.
static void test_reversed_does_not_loop(void)
{
    const int samples[4] = {10, 20, 30, 40};

    reset_reversed("a reversed wave ends rather than looping", 4, ONE_STEP);
    set_samples(samples, 4);
    // Everything a forward wave would need to loop.
    wave.header.status = SF_LOOP << 8;
    wave.header.loopStart = 0;
    chan.statusFlags |= SF_LOOP;

    CHECK(!agb_m4a_mix_reversed(&info, &chan, right, left, FRAME),
          "a reversed wave looped instead of ending");
    CHECK(chan.statusFlags == 0, "the channel was not silenced, its flags are %02X",
          chan.statusFlags);
    // Exactly four samples, and the frame is abandoned there: a wave that ran one
    // sample past its end would have written a fifth.
    for (int i = 0; i < 4; i++)
        CHECK(right[i] != 0, "sample %d was not mixed", i);
    for (int i = 4; i < FRAME; i++)
        CHECK(right[i] == 0, "sample %d was mixed past the end of the wave, it is %d",
              i, right[i]);
}

// The end lands exactly on zero. Testing that needs a count shorter than the
// distance to the start of the wave: when the two are coupled, running one
// sample too far reads the wave header rather than a sample, and a zero there
// looks the same as having stopped.
static void test_reversed_stops_exactly_at_zero(void)
{
    const int samples[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    reset_reversed("a reversed wave stops the moment its count runs out", 8, ONE_STEP);
    set_samples(samples, 8);
    chan.count = 4; // four samples' worth, though four more lie before them

    CHECK(!agb_m4a_mix_reversed(&info, &chan, right, left, FRAME),
          "the wave did not end when its count ran out");

    const int expect[4] = {8, 7, 6, 5};

    for (int i = 0; i < 4; i++)
        CHECK(right[i] == ((255 * expect[i]) >> 8), "sample %d was %d, expected %d",
              i, right[i], (255 * expect[i]) >> 8);
    // A fifth sample was available and must not have been taken.
    CHECK(right[4] == 0, "the wave ran one sample past its count, sample 4 is %d",
          right[4]);
}

// The fraction carries from one frame to the next, as it does going forwards.
static void test_reversed_phase_persists(void)
{
    // A ramp rather than a symmetric pair: with a symmetric wave the midpoint is
    // the same whichever sample you resume from, which would hide a pointer left
    // one place along.
    const int samples[4] = {10, 20, 40, 80};

    reset_reversed("a reversed wave carries its fraction", 4, ONE_STEP / 2);
    set_samples(samples, 4);

    agb_m4a_mix_reversed(&info, &chan, right, left, 1);
    CHECK(chan.fw == ONE_STEP / 2, "expected a half-sample phase, got %u",
          (unsigned)chan.fw);

    // The buffers accumulate, so the second frame needs a clean one to be read.
    memset(right, 0, sizeof(right));
    agb_m4a_mix_reversed(&info, &chan, right, left, 1);
    // Backwards from 80 towards 40, halfway is 60.
    CHECK(right[0] == ((255 * 60) >> 8), "the second frame resumed in the wrong place,"
          " got %d", right[0]);
}

int main(void)
{
    test_scaling();
    test_accumulates();
    test_wraps_rather_than_clamps();
    test_advances_across_frames();
    test_exact_fill_ends();
    test_end_of_wave();
    test_loop();
    test_loop_from_start();
    test_pitched_unit_step();
    test_pitched_half_step();
    test_pitched_double_step();
    test_pitched_phase_persists();
    test_pitched_end();
    test_pitched_loop_overrun();
    test_pitched_loop_overrun_twice();

    test_reversed_turns_round_once();
    test_reversed_plays_backwards();
    test_reversed_fixed_ignores_frequency();
    test_reversed_interpolates();
    test_reversed_turns_round_from_partway();
    test_reversed_multi_sample_step();
    test_reversed_count_carries();
    test_reversed_does_not_loop();
    test_reversed_stops_exactly_at_zero();
    test_reversed_phase_persists();

    return test_report("m4a mixing");
}
