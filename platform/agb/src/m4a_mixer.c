// The sound mixer, replacing upstream's m4a_1.s.
//
// Translated from the ARM rather than rewritten. The envelope below is a state
// machine whose transitions are expressed in the original as arithmetic on the
// status byte -- subtracting one to fall from attack to decay to sustain -- and
// that is kept, because the sequencer above reads the same byte and expects the
// numbering.

#include <stddef.h>
#include <string.h>

#include "agb/m4a.h"

// Attack, decay and release are per-frame multipliers in 8.8; the envelope is
// a byte, so each step truncates.
#define ENVELOPE_MAX 0xFF

// The wave header's flags are the high byte of its `status` field -- the
// original addresses them as a byte at offset 3 and the C struct has no name
// for them, so the test is written here rather than borrowed.
#define WAVE_DATA_FLAG_LOOP 0xC0
#define WAVE_DATA_FLAGS(w) (((w)->status >> 8) & 0xFF)

// Falls from attack to decay, or decay to sustain: the states are numbered so
// that the next one down is one less.
static void envelope_advance(uint8_t *flags)
{
    (*flags)--;
}

// Begin the note: point at the sample, clear the envelope, and take the loop
// flag from the wave rather than the track.
static void envelope_start(struct SoundChannel *chan, uint8_t *flags)
{
    struct WaveData *wav = chan->wav;

    *flags = SOUND_CHANNEL_SF_ENV_ATTACK;
    chan->statusFlags = *flags;
    chan->currentPointer = wav->data + chan->count;
    chan->count = wav->size - chan->count;
    // The original zeroes the envelope here and then rebuilds it from the
    // attack step below, so this store never survives to be read. Kept because
    // the translation is meant to be readable against the ARM, not tidier
    // than it -- no test can observe it, and none pretends to.
    chan->envelopeVolume = 0;
    chan->fw = 0;

    if (WAVE_DATA_FLAGS(wav) & WAVE_DATA_FLAG_LOOP)
    {
        *flags |= SOUND_CHANNEL_SF_LOOP;
        chan->statusFlags = *flags;
    }
}

// The tail after release: the note holds at the echo volume for a set number of
// frames rather than fading to nothing. Returns false when there is no echo to
// play, which ends the channel.
static bool envelope_begin_echo(struct SoundChannel *chan, uint8_t *flags, int *volume)
{
    *volume = chan->pseudoEchoVolume;

    if (*volume == 0)
    {
        chan->statusFlags = 0;
        return false;
    }

    *flags |= SOUND_CHANNEL_SF_IEC;
    chan->statusFlags = *flags;
    return true;
}

// Fold the master volume and the two side volumes into what the mixing loop
// reads per sample.
//
// The master volume is deliberate but reads oddly: the original indexes the
// SoundInfo it is handed with a SoundChannel offset. `release` sits at byte 7 of
// a channel and `masterVolume` at byte 7 of the header, so the load lands on the
// master volume. Kept as it is -- the arithmetic below depends on it.
static void envelope_apply(const struct SoundInfo *info, struct SoundChannel *chan, int volume)
{
    int scaled;

    chan->envelopeVolume = (uint8_t)volume;
    scaled = ((info->masterVolume + 1) * volume) >> 4;
    chan->envelopeVolumeRight = (uint8_t)((chan->rightVolume * scaled) >> 8);
    chan->envelopeVolumeLeft = (uint8_t)((chan->leftVolume * scaled) >> 8);
}

bool agb_m4a_envelope_step(struct SoundInfo *info, struct SoundChannel *chan)
{
    uint8_t flags = chan->statusFlags;
    int volume;

    if (!(flags & SOUND_CHANNEL_SF_ON))
        return false;

    if (flags & SOUND_CHANNEL_SF_START)
    {
        // Asked to start and stop in the same frame: the note never sounds.
        if (flags & SOUND_CHANNEL_SF_STOP)
        {
            chan->statusFlags = 0;
            return false;
        }

        envelope_start(chan, &flags);
        volume = chan->attack;
        if (volume >= ENVELOPE_MAX)
        {
            volume = ENVELOPE_MAX;
            envelope_advance(&flags);
            chan->statusFlags = flags;
        }
        envelope_apply(info, chan, volume);
        return true;
    }

    volume = chan->envelopeVolume;

    if (flags & SOUND_CHANNEL_SF_IEC)
    {
        // The original decrements the byte and then branches on the borrow, so
        // a length of one and a length of zero both end the channel here.
        int remaining = chan->pseudoEchoLength;

        chan->pseudoEchoLength = (uint8_t)(remaining - 1);
        if (remaining <= 1)
        {
            chan->statusFlags = 0;
            return false;
        }
    }
    else if (flags & SOUND_CHANNEL_SF_STOP)
    {
        volume = (volume * chan->release) >> 8;
        if (volume <= chan->pseudoEchoVolume && !envelope_begin_echo(chan, &flags, &volume))
            return false;
    }
    else
    {
        int stage = flags & SOUND_CHANNEL_SF_ENV;

        if (stage == SOUND_CHANNEL_SF_ENV_DECAY)
        {
            volume = (volume * chan->decay) >> 8;
            if (volume <= chan->sustain)
            {
                volume = chan->sustain;
                if (volume == 0)
                {
                    if (!envelope_begin_echo(chan, &flags, &volume))
                        return false;
                }
                else
                {
                    envelope_advance(&flags);
                    chan->statusFlags = flags;
                }
            }
        }
        else if (stage == SOUND_CHANNEL_SF_ENV_ATTACK)
        {
            volume += chan->attack;
            if (volume >= ENVELOPE_MAX)
            {
                volume = ENVELOPE_MAX;
                envelope_advance(&flags);
                chan->statusFlags = flags;
            }
        }
    }

    envelope_apply(info, chan, volume);
    return true;
}

// The fractional position is 9.23: twenty-three bits of fraction below the
// sample index.
#define FW_FRACTION 23

// Advancing the position clears the whole-sample bits it just consumed. The
// original masks exactly bits 23 to 29 rather than keeping the low twenty-three,
// so a step big enough to reach bit 30 would leave it behind; kept as it is.
#define FW_CONSUMED_MASK 0x3F800000u

// Where a looping wave restarts, and how many samples it has from there. A
// wave that does not loop reports a length of zero, which is what ends the
// channel when its count runs out.
static uint32_t loop_span(const struct SoundChannel *chan, const s8 **start)
{
    const struct WaveData *wav = chan->wav;

    if (!(chan->statusFlags & SOUND_CHANNEL_SF_LOOP))
        return 0;

    *start = wav->data + wav->loopStart;
    return wav->size - wav->loopStart;
}

// One sample's contribution to one side.
//
// The original keeps four output samples packed in a register and rotates them
// past an accumulator, masking so that one sample's low bits cannot bleed into
// its neighbour. What that computes, once unpacked, is a plain eight-bit
// accumulate of (volume * sample) >> 8 -- and it wraps rather than clamping,
// so a loud mix distorts the way the hardware does instead of flattening.
static s8 mix_sample(s8 accumulated, int volume, int sample)
{
    return (s8)(accumulated + ((volume * sample) >> 8));
}

bool agb_m4a_mix_fixed(struct SoundChannel *chan, s8 *right, s8 *left, int samples)
{
    const s8 *src = chan->currentPointer;
    const s8 *loop_start = NULL;
    uint32_t remaining = chan->count;
    uint32_t loop_length = loop_span(chan, &loop_start);
    int volume_right = chan->envelopeVolumeRight;
    int volume_left = chan->envelopeVolumeLeft;

    for (int i = 0; i < samples; i++)
    {
        int sample = *src++;

        right[i] = mix_sample(right[i], volume_right, sample);
        left[i] = mix_sample(left[i], volume_left, sample);

        // The count is spent after the sample is used, not before, so the last
        // sample of a wave still sounds -- and a frame filled exactly still
        // ends the channel.
        //
        // A channel handed in with a count of zero would wrap and read past the
        // wave. The original does the same, and the sequencer never produces
        // one, so the behaviour is kept rather than guarded.
        if (--remaining == 0)
        {
            if (loop_length == 0)
            {
                chan->statusFlags = 0;
                chan->currentPointer = (s8 *)src;
                chan->count = 0;
                return false;
            }

            src = loop_start;
            remaining = loop_length;
        }
    }

    chan->currentPointer = (s8 *)src;
    chan->count = remaining;
    return true;
}

// Find the way back into a looping wave after the position ran past its end,
// possibly several loop lengths past it.
//
// `remaining` comes in at or below zero -- how far the step overshot -- and goes
// out positive. The return is the offset into the loop to resume at.
static int32_t loop_rewind(int32_t *remaining, uint32_t loop_length)
{
    int32_t offset = -*remaining;

    for (;;)
    {
        *remaining += (int32_t)loop_length;
        if (*remaining > 0)
            return offset;
        offset -= (int32_t)loop_length;
    }
}

bool agb_m4a_mix_pitched(const struct SoundInfo *info, struct SoundChannel *chan,
                         s8 *right, s8 *left, int samples)
{
    const s8 *loop_start = NULL;
    uint32_t loop_length = loop_span(chan, &loop_start);
    int32_t remaining = (int32_t)chan->count;
    uint32_t phase = chan->fw;
    uint32_t step = (uint32_t)info->divFreq * (uint32_t)chan->frequency;
    int volume_right = chan->envelopeVolumeRight;
    int volume_left = chan->envelopeVolumeLeft;
    // `next` trails one ahead of `current`: the pointer kept in the channel
    // addresses the sample being played, and the one after it is the far end of
    // the interpolation.
    const s8 *next = chan->currentPointer;
    int current = *next++;
    int delta = *next - current;

    for (int i = 0; i < samples; i++)
    {
        // Interpolate the fraction of the way between this sample and the next.
        // The product fits a signed 32-bit multiply for every step the sequencer
        // produces, which is what the original relies on.
        int32_t between = ((int32_t)phase * (int32_t)delta) >> FW_FRACTION;

        right[i] = mix_sample(right[i], volume_right, current + between);
        left[i] = mix_sample(left[i], volume_left, current + between);

        phase += step;
        uint32_t whole = phase >> FW_FRACTION;

        if (whole == 0)
            continue;

        phase &= ~FW_CONSUMED_MASK;
        remaining -= (int32_t)whole;

        if (remaining <= 0)
        {
            if (loop_length == 0)
            {
                chan->statusFlags = 0;
                chan->fw = phase;
                chan->count = 0;
                chan->currentPointer = (s8 *)next - 1;
                return false;
            }

            next = loop_start + loop_rewind(&remaining, loop_length);
            current = *next;
        }
        else if (whole == 1)
        {
            // One sample on: the far end of the last interpolation is the near
            // end of the next, so it needs no reload.
            current += delta;
        }
        else
        {
            next += whole - 1;
            current = *next;
        }

        next++;
        delta = *next - current;
    }

    chan->fw = phase;
    chan->count = (uint32_t)remaining;
    // The pointer trails one behind, addressing the sample still being played.
    chan->currentPointer = (s8 *)next - 1;
    return true;
}

s8 *agb_m4a_frame_buffer(const struct SoundInfo *info)
{
    s8 *frame = (s8 *)info->pcmBuffer;
    int counter = info->pcmDmaCounter;

    // A counter of one or zero means the first frame of the area; anything
    // higher steps forward by however many frames the DMA has left to read.
    if (counter >= 2)
        frame += (info->pcmDmaPeriod - (counter - 1)) * info->pcmSamplesPerVBlank;

    return frame;
}

void agb_m4a_prepare_frame(const struct SoundInfo *info, s8 *frame, int samples)
{
    s8 *right = frame;
    s8 *left = frame + PCM_DMA_BUF_SIZE;
    int reverb = info->reverb;

    if (reverb == 0)
    {
        // The original clears with word stores and never handles the last one to
        // three bytes, so a sample count that is not a multiple of four leaves a
        // tail of the previous frame behind. Every rate the sequencer uses is a
        // multiple of four; the behaviour is kept rather than tidied.
        int whole = samples & ~3;

        memset(right, 0, (size_t)whole);
        memset(left, 0, (size_t)whole);
        return;
    }

    // Reverb sums both sides of this frame with both sides of another, which is
    // the frame ahead -- or the very start of the area, when the DMA counter says
    // this is the last frame in it.
    const s8 *next = info->pcmDmaCounter == 2 ? (const s8 *)info->pcmBuffer : frame + samples;

    for (int i = 0; i < samples; i++)
    {
        int sum = left[i] + right[i] + next[i + PCM_DMA_BUF_SIZE] + next[i];
        int value = (sum * reverb) >> 9;

        // The original tests bit seven of the result and adds one. For a negative
        // result that rounds it back towards zero, the shift above having
        // floored it -- but the test is on the bit rather than on the sign, so a
        // positive result that has run up into the sign bit is nudged too.
        if (value & 0x80)
            value++;

        left[i] = (s8)value;
        right[i] = (s8)value;
    }
}

// The sequencer's one arithmetic helper: the high half of a 64-bit product.
// `umull` gives both halves in a pair of registers and the original keeps the
// high one, which is a fixed-point multiply by a fraction -- MidiKeyToFreq uses
// it to interpolate between two entries of the frequency table, so every pitched
// note's frequency goes through it.
u32 umul3232H32(u32 multiplier, u32 multiplicand)
{
    return (u32)(((uint64_t)multiplier * (uint64_t)multiplicand) >> 32);
}

// ------------------------------------------------------------- reversed waves ---

// Set once, the first time a reversed channel is mixed, so the play position is
// turned round only once however many frames the note lasts.
#define SOUND_CHANNEL_SF_SPECIAL 0x20

// Turn the play position round: the same distance from the end of the wave that
// it currently is from the start. Upstream reaches this with pointer arithmetic
// that folds the wave's own address in twice; it comes to the same thing.
static void reverse_position(struct SoundChannel *chan)
{
    struct WaveData *wav = chan->wav;
    ptrdiff_t played = chan->currentPointer - wav->data;

    chan->currentPointer = wav->data + wav->size - played;
}

// Mix a channel whose wave is played backwards. The resampling is the same as
// the forward pitched path -- interpolate between neighbouring samples, carry
// the fraction across frames -- but the walk runs down the wave rather than up,
// and the sample pairs are read in the other order.
//
// Two things differ from the forward path beyond direction. A reversed wave
// **does not loop**: running out of samples ends the note rather than rewinding.
// And the fixed-frequency variant is handled here too, as a step of exactly one
// sample, rather than in a separate routine.
bool agb_m4a_mix_reversed(const struct SoundInfo *info, struct SoundChannel *chan,
                          s8 *right, s8 *left, int samples)
{
    int32_t remaining = (int32_t)chan->count;
    uint32_t phase = chan->fw;
    uint32_t step;
    int volume_right = chan->envelopeVolumeRight;
    int volume_left = chan->envelopeVolumeLeft;
    const s8 *p;
    int current;
    int delta;

    if (!(chan->statusFlags & SOUND_CHANNEL_SF_SPECIAL))
    {
        chan->statusFlags |= SOUND_CHANNEL_SF_SPECIAL;
        reverse_position(chan);
    }

    step = (chan->type & TONEDATA_TYPE_FIX) ? (1u << FW_FRACTION)
                                            : (uint32_t)info->divFreq * (uint32_t)chan->frequency;

    // The pointer addresses one past the sample being played, so the walk steps
    // back before reading, and the far end of the interpolation is the sample
    // before that.
    p = chan->currentPointer;
    current = *--p;
    delta = p[-1] - current;

    for (int i = 0; i < samples; i++)
    {
        int32_t between = ((int32_t)phase * (int32_t)delta) >> FW_FRACTION;

        right[i] = mix_sample(right[i], volume_right, current + between);
        left[i] = mix_sample(left[i], volume_left, current + between);

        phase += step;
        uint32_t whole = phase >> FW_FRACTION;

        if (whole == 0)
            continue;

        phase &= ~FW_CONSUMED_MASK;
        remaining -= (int32_t)whole;

        if (remaining <= 0)
        {
            // The original abandons the frame here without writing the position
            // back -- the channel is finished, so nothing reads it again.
            chan->statusFlags = 0;
            return false;
        }

        p -= whole;
        current = *p;
        delta = p[-1] - current;
    }

    chan->fw = phase;
    chan->count = (u32)remaining;
    chan->currentPointer = (s8 *)p + 1;
    return true;
}

// ------------------------------------------------------------- the mixer driver ---

// Compressed waves. The game does use them -- a channel of type 0x20 turns up
// during the intro -- and the block decoder is not written yet, so such a channel
// is skipped rather than mixed as though its wave were ordinary. That instrument
// is silent until SoundMainRAM_Unk2 exists.
#define TONEDATA_TYPE_REV 0x10
#define TONEDATA_TYPE_CMP 0x20

// Warned once per run, the way every other not-yet-written subsystem is. The
// generated stubs declare this the same way; there is no header for it.
int agb_deferred_named(const char *name);

// Mix every channel into this frame. Upstream reaches this by copying the
// routine's own machine code into IWRAM and jumping there, because IWRAM is
// faster than ROM; that is a hardware optimisation with no meaning here, so
// SoundMain calls this directly and the relocated copy is never made or used.
// It keeps our own name for that reason: `SoundMainRAM` names a block of code
// upstream copies about, and this is not that.
//
// The frame's buffers are prepared first, then each channel steps its envelope
// and is mixed by whichever path its tone type asks for.
void agb_m4a_mix_frame(struct SoundInfo *info, s8 *frame, int samples)
{
    struct SoundChannel *chan = info->chans;

    agb_m4a_prepare_frame(info, frame, samples);

    // Entered before the count is looked at, as the original does, so a header
    // claiming no channels still has its first one mixed.
    for (int left = info->maxChans;;)
    {
        // The original bails out of this loop once VCOUNT passes a deadline.
        // Not reproduced, and it cannot fire: gMaxLines is absolute zero in
        // every one of upstream's linker scripts. See ARCHITECTURE.md 6.7.
        if (agb_m4a_envelope_step(info, chan))
        {
            if (chan->type & TONEDATA_TYPE_CMP)
                agb_deferred_named("compressed wave");
            else if (chan->type & TONEDATA_TYPE_REV)
                agb_m4a_mix_reversed(info, chan, frame, frame + PCM_DMA_BUF_SIZE, samples);
            else if (chan->type & TONEDATA_TYPE_FIX)
                agb_m4a_mix_fixed(chan, frame, frame + PCM_DMA_BUF_SIZE, samples);
            else
                agb_m4a_mix_pitched(info, chan, frame, frame + PCM_DMA_BUF_SIZE, samples);
        }

        if (--left <= 0)
            break;
        chan++;
    }
}
