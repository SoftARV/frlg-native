// Frame setup: which part of the PCM area a frame writes to, and how its two
// buffers are prepared before any channel is mixed in.
//
// Expected values are worked out by hand from the original ARM.

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

int main(void)
{
    test_frame_position();
    test_clear();
    test_clear_tail();
    test_reverb();
    test_reverb_negative_nudge();
    test_reverb_nudge_is_bit_seven();
    test_reverb_wraps_to_the_start();

    return test_report("m4a frame setup");
}
