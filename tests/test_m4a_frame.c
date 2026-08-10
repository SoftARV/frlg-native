// Frame setup: which part of the PCM area a frame writes to, and how its two
// buffers are prepared before any channel is mixed in.
//
// Expected values are worked out by hand from the original ARM.

#include <stddef.h>
#include <string.h>

#include "agb/m4a.h"

#include "harness.h"

#define SAMPLES 16

static struct SoundInfo info;

static void reset(const char *name)
{
    TEST_CASE(name);
    memset(&info, 0, sizeof(info));
    info.pcmSamplesPerVBlank = SAMPLES;
    info.pcmDmaPeriod = 4;
}

// The area holds several frames; the DMA counter says how far in to write.
static void test_frame_position(void)
{
    reset("frame position");

    info.pcmDmaCounter = 0;
    CHECK(agb_m4a_frame_buffer(&info) == info.pcmBuffer,
          "a counter of zero did not use the first frame");

    info.pcmDmaCounter = 1;
    CHECK(agb_m4a_frame_buffer(&info) == info.pcmBuffer,
          "a counter of one did not use the first frame");

    // period 4, counter 2: (4 - 1) frames in.
    info.pcmDmaCounter = 2;
    CHECK(agb_m4a_frame_buffer(&info) == info.pcmBuffer + 3 * SAMPLES,
          "a counter of two landed in the wrong frame");

    // period 4, counter 4: (4 - 3) frames in.
    info.pcmDmaCounter = 4;
    CHECK(agb_m4a_frame_buffer(&info) == info.pcmBuffer + 1 * SAMPLES,
          "a counter of four landed in the wrong frame");

    reset("frame position follows the period");
    info.pcmDmaPeriod = 2;
    info.pcmDmaCounter = 2;
    CHECK(agb_m4a_frame_buffer(&info) == info.pcmBuffer + 1 * SAMPLES,
          "a shorter period did not shorten the stride");
}

// With reverb off the buffers are cleared, so channels accumulate from silence.
static void test_clear(void)
{
    s8 *frame;

    reset("reverb off clears both buffers");
    info.reverb = 0;
    frame = info.pcmBuffer;
    memset(info.pcmBuffer, 0x7F, sizeof(info.pcmBuffer));

    agb_m4a_prepare_frame(&info, frame, SAMPLES);

    for (int i = 0; i < SAMPLES; i++)
    {
        CHECK(frame[i] == 0, "right[%d] was not cleared, it is %d", i, frame[i]);
        CHECK(frame[i + PCM_DMA_BUF_SIZE] == 0, "left[%d] was not cleared, it is %d", i,
              frame[i + PCM_DMA_BUF_SIZE]);
    }
    // Only this frame's worth is cleared; the rest of the area is left alone.
    CHECK(frame[SAMPLES] == 0x7F, "cleared past the end of the frame");
}

// The original clears in whole words, so a count that is not a multiple of four
// leaves a tail behind. Kept, and pinned here so it cannot drift silently.
static void test_clear_tail(void)
{
    s8 *frame;

    reset("a ragged sample count leaves a tail");
    info.reverb = 0;
    frame = info.pcmBuffer;
    memset(info.pcmBuffer, 0x7F, sizeof(info.pcmBuffer));

    agb_m4a_prepare_frame(&info, frame, 6);

    CHECK(frame[3] == 0, "the first whole word was not cleared");
    CHECK(frame[4] == 0x7F, "the ragged tail was cleared after all, at 4");
    CHECK(frame[5] == 0x7F, "the ragged tail was cleared after all, at 5");
}

// Reverb sums both sides of this frame with both sides of another, scales, and
// writes the same value to both sides.
static void test_reverb(void)
{
    s8 *frame;
    const s8 *next;

    reset("reverb folds two frames together");
    info.reverb = 64;
    info.pcmDmaCounter = 3; // not two, so the other frame is the one ahead
    frame = info.pcmBuffer;
    next = frame + SAMPLES;

    frame[0] = 10;
    frame[0 + PCM_DMA_BUF_SIZE] = 20;
    ((s8 *)next)[0] = 30;
    ((s8 *)next)[0 + PCM_DMA_BUF_SIZE] = 40;

    agb_m4a_prepare_frame(&info, frame, SAMPLES);

    // 20 + 10 + 40 + 30 = 100; (100 * 64) >> 9 = 12; bit seven clear, so no step.
    CHECK(frame[0] == 12, "right came out %d, not 12", frame[0]);
    CHECK(frame[0 + PCM_DMA_BUF_SIZE] == 12, "left came out %d, not 12",
          frame[0 + PCM_DMA_BUF_SIZE]);
}

// A negative result has bit seven set, which the original nudges by one.
static void test_reverb_negative_nudge(void)
{
    s8 *frame;
    s8 *next;

    reset("a negative reverb result is nudged towards zero");
    info.reverb = 64;
    info.pcmDmaCounter = 3;
    frame = info.pcmBuffer;
    next = frame + SAMPLES;

    frame[0] = -10;
    frame[0 + PCM_DMA_BUF_SIZE] = -20;
    next[0] = -30;
    next[0 + PCM_DMA_BUF_SIZE] = -40;

    agb_m4a_prepare_frame(&info, frame, SAMPLES);

    // Sum is -100; (-100 * 64) >> 9 floors to -13; bit seven is set, so -12.
    CHECK(frame[0] == -12, "expected -12 after the nudge, got %d", frame[0]);
}

// The nudge keys on bit seven of the result, not on its sign: a positive result
// that has run up into the sign bit gets it too. -13 has bit eight set as well,
// so it cannot tell the two apart -- this can.
static void test_reverb_nudge_is_bit_seven(void)
{
    s8 *frame;
    s8 *next;

    reset("the nudge keys on bit seven, not bit eight");
    info.reverb = 255;
    info.pcmDmaCounter = 3;
    frame = info.pcmBuffer;
    next = frame + SAMPLES;

    frame[0] = 127;
    frame[0 + PCM_DMA_BUF_SIZE] = 127;
    next[0] = 8;
    next[0 + PCM_DMA_BUF_SIZE] = 0;

    agb_m4a_prepare_frame(&info, frame, SAMPLES);

    // Sum is 262; (262*255)>>9 = 130, which is 0x82: bit seven set, bit eight
    // clear. The nudge makes it 131, and the byte store leaves -125.
    CHECK(frame[0] == -125, "expected -125 after the nudge, got %d", frame[0]);
}

// A DMA counter of two means this is the last frame of the area, so the frame it
// folds in is the one at the very start rather than the one after it.
static void test_reverb_wraps_to_the_start(void)
{
    s8 *frame;

    reset("reverb wraps to the start of the area");
    info.reverb = 128; // a quarter scale -- reverb is a byte, so 512 is not available
    info.pcmDmaCounter = 2;
    info.pcmDmaPeriod = 4;

    frame = agb_m4a_frame_buffer(&info);
    CHECK(frame == info.pcmBuffer + 3 * SAMPLES, "the test set up the wrong frame");

    // Put a marker at the start of the area, and a different one just past this
    // frame -- which is what would be read if it folded the frame ahead instead.
    info.pcmBuffer[0] = 8;
    frame[SAMPLES] = 100;

    agb_m4a_prepare_frame(&info, frame, SAMPLES);

    // 0 + 0 from this frame, plus 8 + 0 from the area's start: (8*128)>>9 = 2.
    // Folding the frame ahead would have given (100*128)>>9 = 25.
    CHECK(frame[0] == 2, "expected the area's start to be folded in, got %d", frame[0]);
}


// ------------------------------------------------------------- the mixer driver ---

// The 64-byte clear the sequencer reaches through its dispatch table. It must
// clear exactly 64 bytes: it is handed a MusicPlayerInfo, and the fields past
// the first 64 are the ones a restarted player keeps.
static void test_clear64(void)
{
    static u8 block[80];
    // Upstream's header declares this with no parameter although it takes one.
    // The game only ever reaches it through its unprototyped dispatch table, so
    // this test reaches it the same way.
    void (*clear64)(void *) = (void (*)(void *))SoundMainBTM;

    TEST_CASE("the 64-byte clear stops at 64");
    memset(block, 0xAB, sizeof(block));

    clear64(block);

    for (int i = 0; i < 64; i++)
        CHECK(block[i] == 0, "byte %d was not cleared, it is %02X", i, block[i]);
    for (int i = 64; i < 80; i++)
        CHECK(block[i] == 0xAB, "byte %d was cleared and should not have been", i);
}

// A channel the driver can actually mix: one sample, held at sustain, at full
// volume on both sides.
static union
{
    struct WaveData header;
    unsigned char raw[sizeof(struct WaveData) + 64];
} driver_wave;

static void arm_channel(struct SoundChannel *chan, u8 type)
{
    s8 *samples = (s8 *)(driver_wave.raw + offsetof(struct WaveData, data));

    memset(chan, 0, sizeof(*chan));
    driver_wave.header.size = 32;
    driver_wave.header.freq = 1 << 20;
    for (int i = 0; i < 32; i++)
        samples[i] = 64;

    chan->wav = &driver_wave.header;
    chan->currentPointer = samples;
    chan->count = 32;
    chan->statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    chan->envelopeVolume = 255;
    chan->envelopeVolumeRight = 255;
    chan->envelopeVolumeLeft = 255;
    chan->rightVolume = 255;
    chan->leftVolume = 255;
    chan->sustain = 255;
    chan->type = type;
    chan->frequency = 1 << 16;
}

// The envelope rebuilds the per-side volumes every frame from the master volume,
// so a driver test has to set one. At full master a sample of 64 arrives as
// ((255 * 255) >> 8) * 64 >> 8 = 63, and that is what these expect.
#define CONTRIBUTION 63

// Every channel up to maxChans is mixed, and nothing past it is touched.
static void test_driver_mixes_each_channel(void)
{
    s8 *frame;

    reset("the driver mixes every channel it is given");
    info.reverb = 0;
    info.maxChans = 2;
    info.divFreq = 1 << 4;
    info.masterVolume = 15;
    frame = info.pcmBuffer;

    arm_channel(&info.chans[0], TONEDATA_TYPE_FIX);
    arm_channel(&info.chans[1], TONEDATA_TYPE_FIX);
    // A third channel, beyond maxChans, which must not be touched.
    arm_channel(&info.chans[2], TONEDATA_TYPE_FIX);

    agb_m4a_mix_frame(&info, frame, SAMPLES);

    // Two channels at full volume, each contributing 64.
    CHECK(frame[0] == 2 * CONTRIBUTION, "the frame came out %d, not two channels' worth",
          frame[0]);
    CHECK(info.chans[2].count == 32, "a channel past maxChans was mixed");
    CHECK(info.chans[0].count == 32 - SAMPLES, "the first channel did not advance");
}

// A channel that is off is stepped but never mixed.
static void test_driver_skips_silent_channels(void)
{
    s8 *frame;

    reset("a channel that is off contributes nothing");
    info.reverb = 0;
    info.maxChans = 2;
    info.divFreq = 1 << 4;
    info.masterVolume = 15;
    frame = info.pcmBuffer;

    arm_channel(&info.chans[0], TONEDATA_TYPE_FIX);
    info.chans[0].statusFlags = 0; // not on
    arm_channel(&info.chans[1], TONEDATA_TYPE_FIX);

    agb_m4a_mix_frame(&info, frame, SAMPLES);

    CHECK(frame[0] == CONTRIBUTION, "the frame came out %d, not one channel's worth",
          frame[0]);
    CHECK(info.chans[0].count == 32, "a silent channel was advanced");
}

// The tone type picks the path. A reversed or compressed wave has no path yet
// and is skipped outright rather than mixed as though it were an ordinary one.
static void test_driver_dispatches_on_type(void)
{
    s8 *frame;

    reset("a reversed wave takes the reversed path");
    info.reverb = 0;
    info.maxChans = 1;
    info.divFreq = 1 << 4;
    info.masterVolume = 15;
    frame = info.pcmBuffer;

    arm_channel(&info.chans[0], 0x10); // TONEDATA_TYPE_REV

    agb_m4a_mix_frame(&info, frame, SAMPLES);

    CHECK(frame[0] == CONTRIBUTION, "a reversed wave was not mixed, the frame is %d",
          frame[0]);
    // Turning the play position round is what only the reversed path does.
    CHECK(info.chans[0].statusFlags & 0x20,
          "the channel was mixed but not by the reversed path");

    reset("a compressed wave is skipped");
    info.reverb = 0;
    info.maxChans = 1;
    info.divFreq = 1 << 4;
    info.masterVolume = 15;
    frame = info.pcmBuffer;
    arm_channel(&info.chans[0], 0x20); // TONEDATA_TYPE_CMP
    agb_m4a_mix_frame(&info, frame, SAMPLES);
    CHECK(frame[0] == 0, "a compressed wave was mixed anyway");
}

// A channel without the fixed-frequency bit is resampled instead, which walks
// the wave at a different rate. A constant wave cannot tell the two paths apart
// -- both produce the same number -- so this watches how far the wave advanced.
static void test_driver_pitched_path(void)
{
    s8 *frame;

    reset("a channel without the fixed bit is resampled");
    info.reverb = 0;
    info.maxChans = 1;
    info.divFreq = 1 << 4;
    info.masterVolume = 15;
    frame = info.pcmBuffer;

    arm_channel(&info.chans[0], 0); // neither FIX nor REV nor CMP

    agb_m4a_mix_frame(&info, frame, SAMPLES);

    // What matters here is which path ran, not its arithmetic -- that is pinned by
    // the resampler's own tests. Over these eight outputs it consumes two input
    // samples where the fixed path would have consumed eight.
    CHECK(info.chans[0].count == 30, "the wave advanced to %d, not 30",
          info.chans[0].count);
    CHECK(info.chans[0].count != 32 - SAMPLES, "the fixed path ran instead");
}

// The buffers are prepared before any channel reaches them, so a frame does not
// accumulate on top of what the last one left.
static void test_driver_prepares_first(void)
{
    s8 *frame;

    reset("the driver clears before it mixes");
    info.reverb = 0;
    info.maxChans = 1;
    info.divFreq = 1 << 4;
    info.masterVolume = 15;
    frame = info.pcmBuffer;
    memset(info.pcmBuffer, 0x40, sizeof(info.pcmBuffer));

    arm_channel(&info.chans[0], TONEDATA_TYPE_FIX);

    agb_m4a_mix_frame(&info, frame, SAMPLES);

    // Had the stale 0x40 survived, this would be 0x40 + CONTRIBUTION.
    CHECK(frame[0] == CONTRIBUTION, "the frame came out %d, so it was not cleared first",
          frame[0]);
}

// The channel loop is entered before the count is looked at, as the original
// does, so a header claiming no channels still mixes its first one.
static void test_driver_zero_max_chans(void)
{
    s8 *frame;

    reset("a maxChans of zero still mixes one channel");
    info.reverb = 0;
    info.maxChans = 0;
    info.divFreq = 1 << 4;
    info.masterVolume = 15;
    frame = info.pcmBuffer;

    arm_channel(&info.chans[0], TONEDATA_TYPE_FIX);

    agb_m4a_mix_frame(&info, frame, SAMPLES);

    CHECK(frame[0] == CONTRIBUTION, "the first channel was not mixed, the frame is %d",
          frame[0]);
    CHECK(info.chans[1].count == 0, "a second channel was mixed as well");
}

int main(void)
{
    test_frame_position();
    test_clear();
    test_clear_tail();
    test_reverb();
    test_reverb_negative_nudge();
    test_reverb_nudge_is_bit_seven();
    test_reverb_wraps_to_the_start();
    test_clear64();
    test_driver_mixes_each_channel();
    test_driver_skips_silent_channels();
    test_driver_dispatches_on_type();
    test_driver_pitched_path();
    test_driver_prepares_first();
    test_driver_zero_max_chans();

    return test_report("m4a frame setup");
}
