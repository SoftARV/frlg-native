// The GBA's four hardware sound channels: two squares, a programmable wave and
// noise. The m4a engine drives them by writing registers rather than by mixing,
// so without this they are simply silent -- which sounds like a tune with parts
// missing rather than like no sound at all.
//
// The channels are the Game Boy's, carried forward unchanged, so the timings
// here are the ones documented for that hardware: a frame sequencer at 512 Hz
// driving length at 256 Hz, sweep at 128 Hz and the envelopes at 64 Hz.
//
// Sampling happens at the software mixer's own output rate rather than at the
// hardware's, because the result is added to the same buffer. That is a
// resampling of a square wave by point sampling, which is not what the hardware
// does; it is what the host would have to do anyway, one stage earlier.

#include <string.h>

#include "agb/memmap.h"
#include "agb/psg.h"

#include "gba/io_reg.h"

// Phase is kept in 32-bit fixed point with the point in the middle, which leaves
// room for the wave channel's rate without overflowing at our sample rates.
#define PHASE_BITS 16
#define PHASE_ONE (1u << PHASE_BITS)

// The GBA counts its sound channels from a 2^17 Hz clock divided by a period
// register that counts up to 2048.
#define SQUARE_CLOCK 131072
#define WAVE_CLOCK 2097152
#define NOISE_CLOCK 524288

#define FRAME_SEQUENCER_HZ 512

struct envelope
{
    int volume;      // 0..15
    int step;        // frames of the sequencer between steps, 0 = held
    int counter;
    int rising;
};

struct square
{
    uint32_t phase;
    int duty;        // 0..3
    int length;      // in 256 Hz ticks, 0 = expired
    int use_length;
    int enabled;
    struct envelope env;
    // Channel one only.
    int sweep_shift;
    int sweep_time;
    int sweep_down;
    int sweep_counter;
    int period;      // the frequency register, which sweep rewrites
};

struct wave
{
    uint32_t phase;
    int length;
    int use_length;
    int enabled;
    int volume_shift; // 4 = silent, 0 = full, 1 = half, 2 = quarter
    int period;
};

struct noise
{
    uint32_t phase;
    uint32_t lfsr;
    int narrow;
    int length;
    int use_length;
    int enabled;
    struct envelope env;
    int period_shift;
    int divisor;
};

static struct square square1, square2;
static struct wave wave3;
static struct noise noise4;
static int sequencer_step;
static uint32_t sequencer_phase;

// The duty cycles, in eighths of a period spent high.
static const int duty_eighths[4] = {1, 2, 4, 6};

// Voices are generated at four times their natural scale so that the duty
// correction below stays exact in integers; the final mix divides it out.
#define VOICE_SCALE 4

// The noise generator's divisor codes: zero means a half rather than a zero.
static const int noise_divisor[8] = {8, 16, 32, 48, 64, 80, 96, 112};

static uint16_t reg(int offset)
{
    uint16_t value;

    memcpy(&value, agb_mem.io + offset, sizeof(value));
    return value;
}

static void reg_write(int offset, uint16_t value)
{
    memcpy(agb_mem.io + offset, &value, sizeof(value));
}

void agb_psg_reset(void)
{
    memset(&square1, 0, sizeof(square1));
    memset(&square2, 0, sizeof(square2));
    memset(&wave3, 0, sizeof(wave3));
    memset(&noise4, 0, sizeof(noise4));
    noise4.lfsr = 0x7FFF;
    sequencer_step = 0;
    sequencer_phase = 0;
}

static void envelope_load(struct envelope *env, uint16_t control)
{
    env->volume = (control >> 12) & 0xF;
    env->rising = (control >> 11) & 1;
    env->step = (control >> 8) & 7;
    env->counter = env->step;
}

static void envelope_tick(struct envelope *env)
{
    if (env->step == 0)
        return;

    if (--env->counter > 0)
        return;

    env->counter = env->step;
    if (env->rising && env->volume < 15)
        env->volume++;
    else if (!env->rising && env->volume > 0)
        env->volume--;
}

// The trigger bit is write-only on the hardware: setting it starts the channel
// and reading it back gives zero. The register file here is plain memory, so the
// bit is cleared once it has been acted on, which comes to the same thing.
static int take_trigger(int offset)
{
    uint16_t value = reg(offset);

    if (!(value & 0x8000))
        return 0;

    reg_write(offset, (uint16_t)(value & ~0x8000));
    return 1;
}

static void square_update(struct square *ch, int cnt_l, int cnt_x, int sweeps)
{
    uint16_t control = reg(cnt_l);
    uint16_t freq = reg(cnt_x);

    ch->duty = (control >> 6) & 3;
    ch->period = freq & 0x7FF;
    ch->use_length = (freq >> 14) & 1;

    if (sweeps)
    {
        uint16_t sweep = reg(REG_OFFSET_SOUND1CNT_L);

        ch->sweep_shift = sweep & 7;
        ch->sweep_down = (sweep >> 3) & 1;
        ch->sweep_time = (sweep >> 4) & 7;
    }

    if (take_trigger(cnt_x))
    {
        ch->enabled = 1;
        ch->length = 64 - (control & 0x3F);
        ch->sweep_counter = ch->sweep_time;
        envelope_load(&ch->env, control);
    }
}

static void wave_update(void)
{
    uint16_t enable = reg(REG_OFFSET_SOUND3CNT_L);
    uint16_t control = reg(REG_OFFSET_SOUND3CNT_H);
    uint16_t freq = reg(REG_OFFSET_SOUND3CNT_X);
    static const int shift_for[4] = {4, 0, 1, 2};

    wave3.period = freq & 0x7FF;
    wave3.use_length = (freq >> 14) & 1;
    wave3.volume_shift = shift_for[(control >> 13) & 3];

    // The channel is held off entirely by its own enable bit.
    if (!(enable & 0x0080))
        wave3.enabled = 0;

    if (take_trigger(REG_OFFSET_SOUND3CNT_X))
    {
        wave3.enabled = (enable & 0x0080) != 0;
        wave3.length = 256 - (control & 0xFF);
        wave3.phase = 0;
    }
}

static void noise_update(void)
{
    uint16_t control = reg(REG_OFFSET_SOUND4CNT_L);
    uint16_t freq = reg(REG_OFFSET_SOUND4CNT_H);

    noise4.divisor = noise_divisor[freq & 7];
    noise4.period_shift = (freq >> 4) & 0xF;
    noise4.narrow = (freq >> 3) & 1;
    noise4.use_length = (freq >> 14) & 1;

    if (take_trigger(REG_OFFSET_SOUND4CNT_H))
    {
        noise4.enabled = 1;
        noise4.length = 64 - (control & 0x3F);
        noise4.lfsr = 0x7FFF;
        envelope_load(&noise4.env, control);
    }
}

static void length_tick(int *length, int use_length, int *enabled)
{
    if (!use_length || *length <= 0)
        return;

    if (--*length == 0)
        *enabled = 0;
}

// Sweep rewrites the frequency register itself, which is why a swept channel
// drifts even though the game never touches it again.
static void sweep_tick(struct square *ch)
{
    int step;

    if (ch->sweep_time == 0 || ch->sweep_shift == 0)
        return;

    if (--ch->sweep_counter > 0)
        return;

    ch->sweep_counter = ch->sweep_time;
    step = ch->period >> ch->sweep_shift;
    ch->period += ch->sweep_down ? -step : step;

    if (ch->period > 2047)
    {
        ch->period = 2047;
        ch->enabled = 0;
    }
    else if (ch->period < 0)
    {
        ch->period = 0;
    }
}

static void sequencer_tick(void)
{
    sequencer_step = (sequencer_step + 1) & 7;

    if ((sequencer_step & 1) == 0)
    {
        length_tick(&square1.length, square1.use_length, &square1.enabled);
        length_tick(&square2.length, square2.use_length, &square2.enabled);
        length_tick(&wave3.length, wave3.use_length, &wave3.enabled);
        length_tick(&noise4.length, noise4.use_length, &noise4.enabled);
    }

    if (sequencer_step == 2 || sequencer_step == 6)
        sweep_tick(&square1);

    if (sequencer_step == 7)
    {
        envelope_tick(&square1.env);
        envelope_tick(&square2.env);
        envelope_tick(&noise4.env);
    }
}

// Each channel's output is a four-bit swing about zero, the way its DAC is.
static int square_sample(struct square *ch, int rate)
{
    uint32_t step;
    int high;

    if (!ch->enabled || ch->period >= 2048)
        return 0;

    step = (uint32_t)(((uint64_t)SQUARE_CLOCK * PHASE_ONE)
                      / ((uint32_t)(2048 - ch->period) * (uint32_t)rate));
    ch->phase = (ch->phase + step) & (PHASE_ONE - 1);

    // A channel's DAC puts out 0 to 15 and the hardware couples the result
    // through a capacitor, so no duty carries a standing offset. Swinging
    // symmetrically about zero instead would: at one eighth duty the mean sits
    // at three quarters of the volume, and since the duty changes from note to
    // note that offset moves, which is a thump rather than a tone. Weighting
    // each side by the time spent on the other keeps the mean at zero and the
    // swing unchanged.
    high = duty_eighths[ch->duty];
    if (ch->phase < (uint32_t)high * (PHASE_ONE / 8))
        return ch->env.volume * (8 - high);

    return -ch->env.volume * high;
}

static int wave_sample(int rate)
{
    uint32_t step;
    unsigned index;
    unsigned byte;
    int nibble;

    if (!wave3.enabled || wave3.volume_shift >= 4 || wave3.period >= 2048)
        return 0;

    // The wave channel steps through its 32 entries at the sample rate below,
    // so the tone it produces is that over thirty-two.
    step = (uint32_t)(((uint64_t)WAVE_CLOCK * PHASE_ONE)
                      / ((uint32_t)(2048 - wave3.period) * (uint32_t)rate * 32u));
    wave3.phase = (wave3.phase + step) & (PHASE_ONE - 1);

    index = (wave3.phase * 32u) >> PHASE_BITS;
    byte = agb_mem.io[REG_OFFSET_WAVE_RAM0 + (index >> 1)];
    nibble = (index & 1) ? (int)(byte & 0xF) : (int)(byte >> 4);

    // Centred the same way, on the assumption the table is symmetric about its
    // midpoint -- which the hardware's capacitor would take care of regardless.
    return ((nibble - 8) * VOICE_SCALE) >> wave3.volume_shift;
}

static int noise_sample(int rate)
{
    uint32_t step;
    uint32_t clock;

    if (!noise4.enabled || noise4.period_shift > 13)
        return 0;

    clock = (uint32_t)((NOISE_CLOCK * 8u) / (uint32_t)noise4.divisor)
            >> noise4.period_shift;
    step = (uint32_t)(((uint64_t)clock * PHASE_ONE) / (uint32_t)rate);

    // The noise clock runs far above the rate this is mixed at -- up to 262 kHz
    // against 13 -- so a sample is not a moment of the shift register, it is
    // however many states it passed through since the last one. Taking the last
    // of them keeps every one of those at full amplitude, which is a channel a
    // quarter too loud and harsher than the hardware's: measured against the
    // reference at 1.24 where the other three sat at 1.00.
    //
    // Averaging over the states, weighted by how long each lasted, is what the
    // speaker does. Below the mix rate there is at most one state per sample and
    // this reduces to what it replaces.
    {
        int64_t acc = 0;
        uint32_t weight = 0;
        uint32_t left = step;

        do
        {
            uint32_t room = PHASE_ONE - noise4.phase;
            uint32_t take = left < room ? left : room;
            int level = (noise4.lfsr & 1) ? -noise4.env.volume * VOICE_SCALE
                                          : noise4.env.volume * VOICE_SCALE;

            // A step shorter than one output sample still contributes the state
            // it is in for the whole of it.
            if (take == 0)
                take = room;

            acc += (int64_t)level * take;
            weight += take;
            noise4.phase += take;
            left -= take < left ? take : left;

            if (noise4.phase >= PHASE_ONE)
            {
                uint32_t bit = (noise4.lfsr ^ (noise4.lfsr >> 1)) & 1;

                noise4.phase -= PHASE_ONE;
                noise4.lfsr = (noise4.lfsr >> 1) | (bit << 14);
                if (noise4.narrow)
                    noise4.lfsr = (noise4.lfsr & ~0x40u) | (bit << 6);
            }
        } while (left > 0);

        return weight ? (int)(acc / weight) : 0;
    }
}

static int8_t add_clamped(int8_t existing, int addition)
{
    int sum = existing + addition;

    // The software mixer's own accumulate wraps, deliberately, because the
    // hardware's does. This one is the final mix into the DAC, which clamps.
    if (sum > 127)
        sum = 127;
    else if (sum < -128)
        sum = -128;

    return (int8_t)sum;
}

void agb_psg_mix(int8_t *right, int8_t *left, int samples, int rate)
{
    uint16_t master = reg(REG_OFFSET_SOUNDCNT_L);
    uint16_t mixing = reg(REG_OFFSET_SOUNDCNT_H);
    int volume_right = master & 7;
    int volume_left = (master >> 4) & 7;
    unsigned enable_right = (master >> 8) & 0xF;
    unsigned enable_left = (master >> 12) & 0xF;
    // Bits 0-1 scale the whole PSG side against direct sound: a quarter, a half,
    // full, and a fourth setting the hardware leaves undefined.
    static const int ratio_num[4] = {1, 2, 4, 4};
    int ratio = ratio_num[mixing & 3];
    uint32_t sequencer_step_phase;

    if (rate <= 0 || !(reg(REG_OFFSET_SOUNDCNT_X) & 0x0080))
        return;

    square_update(&square1, REG_OFFSET_SOUND1CNT_H, REG_OFFSET_SOUND1CNT_X, 1);
    square_update(&square2, REG_OFFSET_SOUND2CNT_L, REG_OFFSET_SOUND2CNT_H, 0);
    wave_update();
    noise_update();

    sequencer_step_phase = (uint32_t)(((uint64_t)FRAME_SEQUENCER_HZ * PHASE_ONE)
                                      / (uint32_t)rate);

    for (int i = 0; i < samples; i++)
    {
        int voice[4];
        int sum_right = 0;
        int sum_left = 0;

        sequencer_phase += sequencer_step_phase;
        while (sequencer_phase >= PHASE_ONE)
        {
            sequencer_phase -= PHASE_ONE;
            sequencer_tick();
        }

        voice[0] = square_sample(&square1, rate);
        voice[1] = square_sample(&square2, rate);
        voice[2] = wave_sample(rate);
        voice[3] = noise_sample(rate);

        for (int v = 0; v < 4; v++)
        {
            if (enable_right & (1u << v))
                sum_right += voice[v];
            if (enable_left & (1u << v))
                sum_left += voice[v];
        }

        // Per-side master volume is one of eight steps, and the ratio above
        // scales the whole side against the software mixer's output.
        sum_right = sum_right * (volume_right + 1) * ratio / (32 * VOICE_SCALE);
        sum_left = sum_left * (volume_left + 1) * ratio / (32 * VOICE_SCALE);

        right[i] = add_clamped(right[i], sum_right);
        left[i] = add_clamped(left[i], sum_left);
    }
}
