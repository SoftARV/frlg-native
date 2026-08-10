// The mixer's envelope: attack, decay, sustain, release and the pseudo-echo
// tail, plus the volume fold the mixing loop reads.
//
// Expected values are worked out by hand from the original ARM rather than
// computed here, so the test cannot agree with the translation by sharing its
// arithmetic.

#include <string.h>

#include "agb/m4a.h"

#include "harness.h"

#define SF_START 0x80
#define SF_STOP 0x40
#define SF_LOOP 0x10
#define SF_IEC 0x04
#define SF_ENV_ATTACK 0x03
#define SF_ENV_DECAY 0x02
#define SF_ENV_SUSTAIN 0x01
#define SF_ENV_RELEASE 0x00

// The loop flag lives in the high byte of the wave header's status field.
#define WAVE_LOOP (0xC0 << 8)

static struct SoundInfo info;
static struct SoundChannel chan;
static struct WaveData wave;
static s8 samples[64];

static void reset(const char *name)
{
    TEST_CASE(name);
    memset(&info, 0, sizeof(info));
    memset(&chan, 0, sizeof(chan));
    memset(&wave, 0, sizeof(wave));
    memset(samples, 0, sizeof(samples));

    info.masterVolume = 15; // the maximum, so the fold below is a clean identity
    wave.size = 64;
    chan.wav = &wave;
    chan.rightVolume = 255;
    chan.leftVolume = 255;
}

// A channel nobody has started is not mixed.
static void test_inactive(void)
{
    reset("a silent channel is not mixed");
    chan.statusFlags = 0;
    CHECK(!agb_m4a_envelope_step(&info, &chan), "an off channel asked to be mixed");
}

static void test_start(void)
{
    reset("starting a note");
    chan.statusFlags = SF_START;
    chan.attack = 16;
    chan.count = 8; // an offset into the sample, as the sequencer leaves it
    CHECK(agb_m4a_envelope_step(&info, &chan), "a started note was not mixed");

    CHECK(chan.statusFlags == SF_ENV_ATTACK, "expected the attack state, got %02X",
          chan.statusFlags);
    CHECK(chan.currentPointer == wave.data + 8, "the sample pointer skipped the wrong distance");
    CHECK(chan.count == 64 - 8, "count is not the samples remaining, but %u",
          (unsigned)chan.count);
    CHECK(chan.fw == 0, "the filter state was not cleared");
    // The first frame applies one attack step rather than sounding at zero.
    CHECK(chan.envelopeVolume == 16, "the first attack step gave %u, not 16",
          chan.envelopeVolume);

    reset("starting reuses a channel from scratch");
    // The sequencer hands back channels that were already sounding, so the
    // envelope has to restart rather than carry on from where it stopped.
    chan.statusFlags = SF_START;
    chan.attack = 16;
    chan.envelopeVolume = 200;
    agb_m4a_envelope_step(&info, &chan);
    CHECK(chan.envelopeVolume == 16, "the envelope carried over, giving %u instead of 16",
          chan.envelopeVolume);

    reset("a looping wave sets the loop flag");
    chan.statusFlags = SF_START;
    chan.attack = 16;
    wave.status = WAVE_LOOP;
    agb_m4a_envelope_step(&info, &chan);
    CHECK((chan.statusFlags & SF_LOOP) != 0, "the loop flag did not come from the wave");

    reset("start and stop together never sounds");
    chan.statusFlags = SF_START | SF_STOP;
    CHECK(!agb_m4a_envelope_step(&info, &chan), "a note stopped on its first frame was mixed");
    CHECK(chan.statusFlags == 0, "the channel was not released");
}

// Attack adds a fixed step per frame and saturates into decay.
static void test_attack(void)
{
    reset("attack ramps and falls into decay");
    chan.statusFlags = SF_ENV_ATTACK;
    chan.attack = 100;
    chan.envelopeVolume = 0;

    agb_m4a_envelope_step(&info, &chan);
    CHECK(chan.envelopeVolume == 100, "first step gave %u, not 100", chan.envelopeVolume);
    CHECK(chan.statusFlags == SF_ENV_ATTACK, "left attack too early");

    agb_m4a_envelope_step(&info, &chan);
    CHECK(chan.envelopeVolume == 200, "second step gave %u, not 200", chan.envelopeVolume);

    // Landing exactly on the maximum still counts as saturating.
    reset("attack landing exactly on the maximum");
    chan.statusFlags = SF_ENV_ATTACK;
    chan.attack = 100;
    chan.envelopeVolume = 155;
    agb_m4a_envelope_step(&info, &chan);
    CHECK(chan.envelopeVolume == 255, "expected 255, got %u", chan.envelopeVolume);
    CHECK(chan.statusFlags == SF_ENV_DECAY, "an exact landing did not leave attack, state %02X",
          chan.statusFlags);

    reset("attack ramps and falls into decay");
    chan.statusFlags = SF_ENV_ATTACK;
    chan.attack = 100;
    chan.envelopeVolume = 200;
    // 300 saturates at 255 and the state falls one place, to decay.
    agb_m4a_envelope_step(&info, &chan);
    CHECK(chan.envelopeVolume == 255, "attack did not saturate, gave %u", chan.envelopeVolume);
    CHECK(chan.statusFlags == SF_ENV_DECAY, "expected decay after saturation, got %02X",
          chan.statusFlags);
}

// Decay multiplies rather than subtracts, and holds at the sustain level.
static void test_decay_to_sustain(void)
{
    reset("decay falls to sustain");
    chan.statusFlags = SF_ENV_DECAY;
    chan.envelopeVolume = 255;
    chan.decay = 128;  // half per frame
    chan.sustain = 100;

    agb_m4a_envelope_step(&info, &chan);
    CHECK(chan.envelopeVolume == 127, "(255*128)>>8 should be 127, got %u", chan.envelopeVolume);
    CHECK(chan.statusFlags == SF_ENV_DECAY, "left decay while still above sustain");

    // (127*128)>>8 = 63, which is at or below sustain, so it clamps and moves on.
    agb_m4a_envelope_step(&info, &chan);
    CHECK(chan.envelopeVolume == 100, "did not clamp to the sustain level, got %u",
          chan.envelopeVolume);
    CHECK(chan.statusFlags == SF_ENV_SUSTAIN, "expected sustain, got %02X", chan.statusFlags);
}

// Sustain holds: no state, no change, frame after frame.
static void test_sustain_holds(void)
{
    reset("sustain holds");
    chan.statusFlags = SF_ENV_SUSTAIN;
    chan.envelopeVolume = 100;
    chan.decay = 1;
    chan.attack = 50;

    for (int i = 0; i < 4; i++)
        agb_m4a_envelope_step(&info, &chan);

    CHECK(chan.envelopeVolume == 100, "sustain drifted to %u", chan.envelopeVolume);
    CHECK(chan.statusFlags == SF_ENV_SUSTAIN, "sustain changed state to %02X", chan.statusFlags);
}

// Release multiplies down until it reaches the echo volume, then the tail runs
// for a fixed number of frames.
static void test_release_into_echo(void)
{
    reset("release falls into the echo tail");
    chan.statusFlags = SF_ENV_SUSTAIN | SF_STOP;
    chan.envelopeVolume = 255;
    chan.release = 128;
    chan.pseudoEchoVolume = 60;
    chan.pseudoEchoLength = 3;

    agb_m4a_envelope_step(&info, &chan);
    CHECK(chan.envelopeVolume == 127, "release step gave %u, not 127", chan.envelopeVolume);
    CHECK((chan.statusFlags & SF_IEC) == 0, "entered the echo while still above its volume");

    // (127*128)>>8 = 63, still above 60.
    agb_m4a_envelope_step(&info, &chan);
    CHECK(chan.envelopeVolume == 63, "second release step gave %u, not 63", chan.envelopeVolume);

    // (63*128)>>8 = 31, at or below 60: hold at the echo volume instead.
    agb_m4a_envelope_step(&info, &chan);
    CHECK(chan.envelopeVolume == 60, "did not hold at the echo volume, got %u",
          chan.envelopeVolume);
    CHECK((chan.statusFlags & SF_IEC) != 0, "the echo flag was not set");

    // Now the tail counts down, and the channel ends when it runs out.
    CHECK(agb_m4a_envelope_step(&info, &chan), "the echo ended a frame early");
    CHECK(chan.pseudoEchoLength == 2, "the tail did not count down, at %u",
          chan.pseudoEchoLength);
    CHECK(agb_m4a_envelope_step(&info, &chan), "the echo ended a frame early");
    CHECK(!agb_m4a_envelope_step(&info, &chan), "the echo outlasted its length");
    CHECK(chan.statusFlags == 0, "the channel was not released at the end of the echo");
}

// With no echo to play, release ends the channel outright.
static void test_release_without_echo(void)
{
    reset("release with no echo ends the note");
    chan.statusFlags = SF_ENV_SUSTAIN | SF_STOP;
    chan.envelopeVolume = 1;
    chan.release = 128;
    chan.pseudoEchoVolume = 0;

    // (1*128)>>8 is 0, which is not above the echo volume, so the tail is asked
    // for -- and there is none. A volume still above it would keep sounding.
    CHECK(!agb_m4a_envelope_step(&info, &chan), "a note with no echo kept sounding");
    CHECK(chan.statusFlags == 0, "the channel was not released");
}

// A sustain level of zero has nowhere to hold, so decay goes straight to the
// echo rather than sitting at silence.
static void test_decay_to_zero_sustain(void)
{
    reset("decay to a zero sustain enters the echo");
    chan.statusFlags = SF_ENV_DECAY;
    chan.envelopeVolume = 1; // (1*128)>>8 is 0, which is not above the sustain
    chan.decay = 128;
    chan.sustain = 0;
    chan.pseudoEchoVolume = 40;
    chan.pseudoEchoLength = 2;

    CHECK(agb_m4a_envelope_step(&info, &chan), "the note ended instead of echoing");
    CHECK(chan.envelopeVolume == 40, "did not take the echo volume, got %u", chan.envelopeVolume);
    CHECK((chan.statusFlags & SF_IEC) != 0, "the echo flag was not set");

    reset("decay to a zero sustain with no echo ends the note");
    chan.statusFlags = SF_ENV_DECAY;
    chan.envelopeVolume = 1;
    chan.decay = 128;
    chan.sustain = 0;
    chan.pseudoEchoVolume = 0;
    CHECK(!agb_m4a_envelope_step(&info, &chan), "a note with neither sustain nor echo kept going");
}

// An echo length of one, or of zero, ends the channel on the next frame: the
// original decrements the byte and branches on the borrow.
static void test_echo_length_edges(void)
{
    reset("an echo length of one ends immediately");
    chan.statusFlags = SF_ENV_SUSTAIN | SF_IEC;
    chan.envelopeVolume = 40;
    chan.pseudoEchoLength = 1;
    CHECK(!agb_m4a_envelope_step(&info, &chan), "an echo of one frame ran on");

    reset("an echo length of zero ends immediately");
    chan.statusFlags = SF_ENV_SUSTAIN | SF_IEC;
    chan.envelopeVolume = 40;
    chan.pseudoEchoLength = 0;
    CHECK(!agb_m4a_envelope_step(&info, &chan), "an echo of zero frames wrapped instead of ending");
}

// The master volume is folded in through a byte the original reaches by
// indexing the sound header with a channel offset. It scales everything.
static void test_volume_fold(void)
{
    reset("volume fold at full master volume");
    chan.statusFlags = SF_ENV_SUSTAIN;
    chan.envelopeVolume = 255;
    info.masterVolume = 15;
    chan.rightVolume = 128;
    chan.leftVolume = 64;
    agb_m4a_envelope_step(&info, &chan);

    // scaled = ((15+1)*255)>>4 = 255; right = (128*255)>>8 = 127; left = (64*255)>>8 = 63.
    CHECK(chan.envelopeVolumeRight == 127, "right came out %u, not 127", chan.envelopeVolumeRight);
    CHECK(chan.envelopeVolumeLeft == 63, "left came out %u, not 63", chan.envelopeVolumeLeft);

    reset("volume fold at the lowest master volume");
    chan.statusFlags = SF_ENV_SUSTAIN;
    chan.envelopeVolume = 255;
    info.masterVolume = 0;
    chan.rightVolume = 128;
    chan.leftVolume = 64;
    agb_m4a_envelope_step(&info, &chan);

    // scaled = ((0+1)*255)>>4 = 15; right = (128*15)>>8 = 7; left = (64*15)>>8 = 3.
    CHECK(chan.envelopeVolumeRight == 7, "right came out %u, not 7", chan.envelopeVolumeRight);
    CHECK(chan.envelopeVolumeLeft == 3, "left came out %u, not 3", chan.envelopeVolumeLeft);
}

// The threshold is strict: a volume still above the sustain or echo level keeps
// the note going rather than clamping early.
static void test_threshold_is_strict(void)
{
    reset("decay above sustain keeps decaying");
    chan.statusFlags = SF_ENV_DECAY;
    chan.envelopeVolume = 4;
    chan.decay = 128;
    chan.sustain = 0;
    chan.pseudoEchoVolume = 40;
    agb_m4a_envelope_step(&info, &chan);
    CHECK(chan.envelopeVolume == 2, "(4*128)>>8 should be 2, got %u", chan.envelopeVolume);
    CHECK(chan.statusFlags == SF_ENV_DECAY, "clamped while still above the sustain");

    reset("release above the echo volume keeps releasing");
    chan.statusFlags = SF_ENV_SUSTAIN | SF_STOP;
    chan.envelopeVolume = 4;
    chan.release = 128;
    chan.pseudoEchoVolume = 0;
    CHECK(agb_m4a_envelope_step(&info, &chan), "the note ended while still above the echo");
    CHECK(chan.envelopeVolume == 2, "(4*128)>>8 should be 2, got %u", chan.envelopeVolume);
    CHECK((chan.statusFlags & SF_IEC) == 0, "entered the echo early");
}

int main(void)
{
    test_inactive();
    test_start();
    test_attack();
    test_decay_to_sustain();
    test_sustain_holds();
    test_release_into_echo();
    test_release_without_echo();
    test_decay_to_zero_sustain();
    test_echo_length_edges();
    test_threshold_is_strict();
    test_volume_fold();

    return test_report("m4a envelope");
}
