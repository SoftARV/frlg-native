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

    return test_report("m4a mixing");
}
