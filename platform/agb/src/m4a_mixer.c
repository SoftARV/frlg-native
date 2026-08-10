// The sound mixer, replacing upstream's m4a_1.s.
//
// Translated from the ARM rather than rewritten. The envelope below is a state
// machine whose transitions are expressed in the original as arithmetic on the
// status byte -- subtracting one to fall from attack to decay to sustain -- and
// that is kept, because the sequencer above reads the same byte and expects the
// numbering.

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
