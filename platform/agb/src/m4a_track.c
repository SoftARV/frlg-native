// The track interpreter's parameter opcodes, replacing their halves of
// upstream's m4a_1.s.
//
// These carry the game's own names rather than an `agb_` prefix, on purpose:
// they are the symbols the sequencer's jump table names, and this file is what
// supplies them. Same arrangement as the rest of the m4a_1 replacement.
//
// Every handler takes the player and the track, reads whatever operand bytes it
// needs from the track's command stream, and stores them where the mixer and the
// sequencer will look. Flags are how a handler says what it invalidated: the
// volume or the pitch has to be recomputed before the next note sounds.

#include <stdint.h>

#include "agb/m4a.h"
#include "agb/memmap.h"

// Pan, bend and tune arrive biased so that centre is a positive byte.
#define OPERAND_CENTRE C_V

// Reading the command stream always advances it. The original guards this read
// against straying into the BIOS ROM, which cannot happen here -- there is no
// BIOS ROM to stray into.
static u8 track_fetch(struct MusicPlayerTrack *track)
{
    return *track->cmdPtr++;
}

static void track_invalidate(struct MusicPlayerTrack *track, u8 what)
{
    track->flags |= what;
}

// Stop the modulation sweep where it is. Which of the two recomputations this
// needs depends on what the modulation was driving: type zero bends the pitch,
// anything else moves the volume.
void clear_modM(struct MusicPlayerTrack *track)
{
    track->modM = 0;
    track->lfoSpeedC = 0;
    track_invalidate(track, track->modT == 0 ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG);
}

void ply_prio(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    (void)player;
    track->priority = track_fetch(track);
}

// Tempo arrives halved, and the effective tempo is the requested one scaled by
// the player's own multiplier.
void ply_tempo(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    u16 requested = (u16)(track_fetch(track) * 2);

    player->tempoD = requested;
    player->tempoI = (u16)((requested * player->tempoU) >> 8);
}

void ply_keysh(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    (void)player;
    track->keyShift = (s8)track_fetch(track);
    // Both, because a key shift changes the pitch of a note already sounding
    // and the sequencer treats the pair as one invalidation.
    track_invalidate(track, MPT_FLG_VOLCHG | MPT_FLG_PITCHG);
}

// Select an instrument. The original copies the tone entry as three words,
// which is the whole of it -- so this is the same copy said once.
void ply_voice(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    u8 index = track_fetch(track);

    track->tone = player->tone[index];
}

void ply_vol(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    (void)player;
    track->vol = track_fetch(track);
    track_invalidate(track, MPT_FLG_VOLCHG);
}

void ply_pan(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    (void)player;
    track->pan = (s8)(track_fetch(track) - OPERAND_CENTRE);
    track_invalidate(track, MPT_FLG_VOLCHG);
}

void ply_bend(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    (void)player;
    track->bend = (s8)(track_fetch(track) - OPERAND_CENTRE);
    track_invalidate(track, MPT_FLG_PITCHG);
}

void ply_bendr(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    (void)player;
    track->bendRange = track_fetch(track);
    track_invalidate(track, MPT_FLG_PITCHG);
}

void ply_lfodl(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    (void)player;
    track->lfoDelay = track_fetch(track);
}

// Changing what the modulation drives invalidates both, but setting it to what
// it already was does nothing at all.
void ply_modt(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    u8 type = track_fetch(track);

    (void)player;
    if (track->modT == type)
        return;

    track->modT = type;
    track_invalidate(track, MPT_FLG_VOLCHG | MPT_FLG_PITCHG);
}

void ply_tune(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    (void)player;
    track->tune = (s8)(track_fetch(track) - OPERAND_CENTRE);
    track_invalidate(track, MPT_FLG_PITCHG);
}

// Setting either modulation control to zero stops the sweep rather than leaving
// it part-way through.
void ply_lfos(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    (void)player;
    track->lfoSpeed = track_fetch(track);
    if (track->lfoSpeed == 0)
        clear_modM(track);
}

void ply_mod(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    (void)player;
    track->mod = track_fetch(track);
    if (track->mod == 0)
        clear_modM(track);
}

// -------------------------------------------------------------- jump table ---

// The template lives in the game's own m4a_tables.c and is not declared in any
// header, so it is declared here.
extern void *const gMPlayJumpTableTemplate[];

#define MPLAY_JUMP_TABLE_ENTRIES 36

// Fill the sequencer's dispatch table from the template.
//
// On hardware this reads the template out of the BIOS ROM, and the original
// guards every entry against straying outside it. There is no BIOS ROM here and
// the template is an ordinary array, so the copy is just a copy.
//
// Not to be confused with the sequencer's own MusicPlayerJumpTableCopy, which
// asks the BIOS to do the same thing through a software interrupt. Nothing in
// this game calls that one -- which is why the build can drop its one
// instruction without supplying a replacement.
void MPlayJumpTableCopy(MPlayFunc *dest)
{
    for (int i = 0; i < MPLAY_JUMP_TABLE_ENTRIES; i++)
        dest[i] = (MPlayFunc)gMPlayJumpTableTemplate[i];
}

// ------------------------------------------------------------ control flow ---

// Unlink a channel from the chain its track keeps, and let go of the track. The
// chain's head lives in the track rather than in a channel, so losing the first
// one is a separate case from losing a middle one.
void RealClearChain(void *x)
{
    struct SoundChannel *chan = x;
    struct MusicPlayerTrack *track = chan->track;
    struct SoundChannel *next;
    struct SoundChannel *prev;

    if (track == NULL)
        return;

    next = chan->nextChannelPointer;
    prev = chan->prevChannelPointer;

    if (prev != NULL)
        prev->nextChannelPointer = next;
    else
        track->chan = next;

    if (next != NULL)
        next->prevChannelPointer = prev;

    chan->track = NULL;
}

// End of track. Every channel it still owns is told to stop -- released rather
// than cut off, so envelopes finish -- and then unlinked.
void ply_fine(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan = track->chan;

    (void)player;
    while (chan != NULL)
    {
        if (chan->statusFlags & SOUND_CHANNEL_SF_ON)
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;

        // Unlinking leaves the channel's own forward pointer alone, which is
        // what makes walking the chain while dismantling it work.
        RealClearChain(chan);
        chan = chan->nextChannelPointer;
    }

    track->flags = 0;
}

// Jump: the operand is a four-byte little-endian address, and it replaces the
// command pointer rather than being skipped over.
void ply_goto(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    const u8 *at = track->cmdPtr;

    (void)player;
    track->cmdPtr = (u8 *)(uintptr_t)((u32)at[0] | ((u32)at[1] << 8) | ((u32)at[2] << 16)
                                      | ((u32)at[3] << 24));
}

// Call a pattern: remember where to come back to, then jump. The stack is three
// deep, and a fourth call ends the track rather than overflowing it.
void ply_patt(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    u8 level = track->patternLevel;

    if (level >= 3)
    {
        ply_fine(player, track);
        return;
    }

    track->patternStack[level] = track->cmdPtr + 4;
    track->patternLevel = (u8)(level + 1);
    ply_goto(player, track);
}

// Return from a pattern. At the outermost level there is nowhere to return to,
// and the opcode does nothing at all.
void ply_pend(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    u8 level = track->patternLevel;

    (void)player;
    if (level == 0)
        return;

    level--;
    track->patternLevel = level;
    track->cmdPtr = track->patternStack[level];
}

// Repeat: a count of zero loops for ever, otherwise the jump is taken until the
// count is reached and then the whole operand is stepped over.
void ply_rept(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    u8 *operand = track->cmdPtr;
    u8 count = *operand;
    u8 taken;

    if (count == 0)
    {
        track->cmdPtr = operand + 1;
        ply_goto(player, track);
        return;
    }

    taken = (u8)(track->repN + 1);
    track->repN = taken;
    track->cmdPtr = operand + 1;

    if (taken < count)
    {
        ply_goto(player, track);
        return;
    }

    track->repN = 0;
    // Past the count and the four address bytes.
    track->cmdPtr = operand + 5;
}

// The sequencer keeps the sound header's address at a fixed spot in IWRAM. The
// prelude points the game's own SOUND_INFO_PTR at the same place, so both sides
// agree without either knowing about the other.
#define SOUND_INFO_SLOT 0x7FF0

static struct SoundInfo *sound_info(void)
{
    return *(struct SoundInfo **)(agb_mem.iwram + SOUND_INFO_SLOT);
}

// Silence a track at once. Unlike the end of a track, which releases its
// channels and lets their envelopes finish, this cuts them off: a stopped
// channel is available again immediately.
void TrackStop(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan;

    (void)player;
    if (!(track->flags & MPT_FLG_EXIST))
        return;

    for (chan = track->chan; chan != NULL; chan = chan->nextChannelPointer)
    {
        if (chan->statusFlags != 0)
        {
            // A compatible-sound channel is a hardware oscillator rather than a
            // mixed one, so it has to be told to stop rather than just dropped.
            // What the header wants is the channel's type, not the channel.
            u8 cgb = chan->type & TONEDATA_TYPE_CGB;

            if (cgb != 0)
                sound_info()->CgbOscOff(cgb);

            chan->statusFlags = 0;
        }

        chan->track = NULL;
    }

    track->chan = NULL;
}

// Split a note's velocity across the two sides according to where it is panned.
//
// The two sides are not quite symmetrical: the right takes 0x80 plus the pan and
// the left 0x7F minus it, so a centred note is one step louder on the right.
// That is the original's arithmetic, not a rounding artefact of this
// translation.
void ChnVolSetAsm(struct SoundChannel *chan, struct MusicPlayerTrack *track)
{
    int velocity = chan->velocity;
    int pan = (s8)chan->rhythmPan;
    int right = (track->volMR * ((0x80 + pan) * velocity)) >> 14;
    int left = (track->volML * ((0x7F - pan) * velocity)) >> 14;

    chan->rightVolume = (u8)(right > 0xFF ? 0xFF : right);
    chan->leftVolume = (u8)(left > 0xFF ? 0xFF : left);
}

// End a tied note. The key either arrives as an operand or is whatever the track
// last played, and only the first channel still holding that key is released --
// a tie ends one note, not every note sharing its pitch.
void ply_endtie(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    struct SoundChannel *chan;
    u8 operand = *track->cmdPtr;
    u8 key;

    (void)player;
    if (operand < 0x80)
    {
        // A key was given: it becomes the track's running key and is consumed.
        track->key = operand;
        track->cmdPtr++;
        key = operand;
    }
    else
    {
        // No key: the byte belongs to whatever comes next, and the tie ends the
        // note the track is already on.
        key = track->key;
    }

    for (chan = track->chan; chan != NULL; chan = chan->nextChannelPointer)
    {
        u8 flags = chan->statusFlags;

        if (!(flags & (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_ENV)))
            continue;
        if (flags & SOUND_CHANNEL_SF_STOP)
            continue;
        if (chan->midiKey != key)
            continue;

        chan->statusFlags = (u8)(flags | SOUND_CHANNEL_SF_STOP);
        return;
    }
}

// Two operands: an offset into the compatible-sound registers, then the byte to
// put there. The track data addresses those registers directly.
void ply_port(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    u8 offset = track_fetch(track);
    u8 value = track_fetch(track);

    (void)player;
    *(volatile u8 *)(agb_mem.io + REG_OFFSET_SOUND1CNT_L + offset) = value;
}

// ------------------------------------------------------------ starting a note ---

// Note lengths are not encoded directly; the opcode carries an index into this
// table. It lives in the game's own data, so we only borrow it.
extern const u8 gClockTable[];

// Defined by the sequencer, which has no header for it.
extern u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust);

// A rhythm or key-split instrument's `wav` field is not a waveform at all, it is
// the base of an array of real instruments to pick from.
#define SUBTONE(tone, index) ((const struct ToneData *)((const u8 *)(tone)->wav + (index) * 12))

// A key-split instrument keeps its split table where a plain one keeps its
// envelope. Upstream's assembler names that offset outright; the C struct has no
// field for it, so it is reached the same way.
#define KEY_SPLIT_TABLE(tone) (*(const u8 *const *)&(tone)->attack)

// Which instrument this note actually plays, and on which key. A plain
// instrument answers for itself; a key-split one redirects through its table,
// and a rhythm one additionally carries its own key and pan.
//
// Returns NULL when the redirect lands on another split or rhythm entry: one
// level of indirection is all the format allows, and the note is dropped.
static const struct ToneData *resolve_tone(struct MusicPlayerTrack *track, u8 *key,
                                           u32 *rhythm_pan)
{
    const struct ToneData *tone = &track->tone;
    const struct ToneData *entry;
    u8 index;

    *rhythm_pan = 0;
    *key = track->key;

    if (!(tone->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY)))
        return tone;

    index = (tone->type & TONEDATA_TYPE_SPL) ? KEY_SPLIT_TABLE(tone)[*key] : *key;
    entry = SUBTONE(tone, index);

    if (entry->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY))
        return NULL;

    if (tone->type & TONEDATA_TYPE_RHY)
    {
        // Only a rhythm entry that asks for a pan gets one, and it arrives
        // biased so that centre is a positive byte.
        if (entry->pan_sweep & 0x80)
            *rhythm_pan = (u32)((u8)(entry->pan_sweep - TONEDATA_P_S_PAN) << 1);

        *key = entry->key;
    }

    return entry;
}

// A compatible-sound note has exactly one channel it can play on, so the only
// question is whether what is already there may be taken. Anything finished or
// releasing may be; otherwise the newcomer has to be at least as important, and
// ties are settled by track address so that the decision is stable.
static struct SoundChannel *claim_cgb_channel(struct SoundInfo *info, u8 cgb_type,
                                              struct MusicPlayerTrack *track, u8 priority)
{
    struct SoundChannel *chan;

    if (info->cgbChans == NULL)
        return NULL;

    // The two channel kinds share a layout for everything this code touches, so
    // the shared parts are reached through the mixed-channel type.
    chan = (struct SoundChannel *)((u8 *)info->cgbChans + (cgb_type - 1) * 64);

    if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON))
        return chan;
    if (chan->statusFlags & SOUND_CHANNEL_SF_STOP)
        return chan;
    if (chan->priority < priority)
        return chan;
    if (chan->priority > priority)
        return NULL;

    return (uintptr_t)chan->track >= (uintptr_t)track ? chan : NULL;
}

// A mixed note takes the first idle channel it finds. Failing that it steals the
// least worthy one: a releasing channel is always a better victim than a
// sounding one, then the lowest priority, and finally the highest track address.
static struct SoundChannel *claim_mixed_channel(struct SoundInfo *info,
                                                struct MusicPlayerTrack *track, u8 priority)
{
    struct SoundChannel *chan = info->chans;
    struct SoundChannel *best = NULL;
    struct MusicPlayerTrack *best_track = track;
    u8 best_priority = priority;
    int releasing = 0;

    for (int left = info->maxChans; left > 0; left--, chan++)
    {
        int candidate;

        if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON))
            return chan;

        if (chan->statusFlags & SOUND_CHANNEL_SF_STOP)
        {
            // The first releasing channel displaces any sounding one already
            // held, however important it looked.
            if (!releasing)
            {
                releasing = 1;
                best_priority = chan->priority;
                best_track = chan->track;
                best = chan;
                continue;
            }
        }
        else if (releasing)
        {
            continue;
        }

        candidate = 0;
        if (chan->priority < best_priority)
        {
            best_priority = chan->priority;
            best_track = chan->track;
            candidate = 1;
        }
        else if (chan->priority == best_priority
                 && (uintptr_t)chan->track >= (uintptr_t)best_track)
        {
            best_track = chan->track;
            candidate = 1;
        }

        if (candidate)
            best = chan;
    }

    return best;
}

// Hand a channel over to a track: unhook it from wherever it was, put it at the
// head of this track's chain, and restart the modulation delay.
static void attach_channel(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track,
                           struct SoundChannel *chan)
{
    struct SoundChannel *head = track->chan;

    ClearChain(chan);

    chan->prevChannelPointer = NULL;
    chan->nextChannelPointer = head;
    if (head != NULL)
        head->prevChannelPointer = chan;

    track->chan = chan;
    chan->track = track;

    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0)
        clear_modM(track);

    TrkVolPitSet(player, track);
}

// Start a note. The opcode's operand is a length index; up to three more bytes
// may follow -- key, velocity, and extra length -- each optional, and each
// recognised only by being below 0x80. Anything at or above that is the next
// opcode, and a note that supplies none of them simply repeats the last one.
void ply_note(u32 clock, struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    struct SoundInfo *info = sound_info();
    const struct ToneData *tone;
    struct SoundChannel *chan;
    u32 rhythm_pan;
    u8 key;
    u8 priority;
    u8 cgb_type;
    int pitch;

    track->gateTime = gClockTable[clock];

    if (*track->cmdPtr < 0x80)
    {
        const u8 *p = track->cmdPtr;

        track->key = *p++;
        if (*p < 0x80)
        {
            track->velocity = *p++;
            if (*p < 0x80)
                track->gateTime = (u8)(track->gateTime + *p++);
        }
        track->cmdPtr = (u8 *)p;
    }

    tone = resolve_tone(track, &key, &rhythm_pan);
    if (tone == NULL)
        return;

    priority = (u8)(track->priority + player->priority);
    if (track->priority + player->priority > 0xFF)
        priority = 0xFF;

    cgb_type = tone->type & TONEDATA_TYPE_CGB;
    chan = cgb_type != 0 ? claim_cgb_channel(info, cgb_type, track, priority)
                         : claim_mixed_channel(info, track, priority);
    if (chan == NULL)
        return;

    attach_channel(player, track, chan);

    // The original copies these four bytes as one word, which lands the track's
    // running status in the channel's priority -- immediately overwritten below.
    chan->gateTime = track->gateTime;
    chan->midiKey = track->key;
    chan->velocity = track->velocity;
    chan->priority = priority;

    chan->key = key;
    chan->rhythmPan = (u8)rhythm_pan;
    chan->type = tone->type;
    chan->wav = tone->wav;
    chan->attack = tone->attack;
    chan->decay = tone->decay;
    chan->sustain = tone->sustain;
    chan->release = tone->release;
    chan->pseudoEchoVolume = track->pseudoEchoVolume;
    chan->pseudoEchoLength = track->pseudoEchoLength;

    ChnVolSetAsm(chan, track);

    // A key shifted below zero does not wrap, it bottoms out.
    pitch = chan->key + (s8)track->keyM;
    if (pitch < 0)
        pitch = 0;

    if (cgb_type != 0)
    {
        struct CgbChannel *cgb = (struct CgbChannel *)chan;
        u8 sweep = tone->pan_sweep;

        cgb->length = tone->length;
        // A pan request is not a sweep, and neither is an empty one.
        if ((sweep & 0x80) || !(sweep & 0x70))
            sweep = 8;
        cgb->sweep = sweep;

        chan->frequency = info->MidiKeyToCgbFreq(cgb_type, (u8)pitch, track->pitM);
    }
    else
    {
        chan->count = track->unk_3C;
        chan->frequency = MidiKeyToFreq(tone->wav, (u8)pitch, track->pitM);
    }

    chan->statusFlags = SOUND_CHANNEL_SF_START;
    track->flags &= 0xF0;
}

// ---------------------------------------------------------- the V-blank tick ---

// The DMA channels feeding the sound FIFOs. Ours is one register file, so these
// are offsets into it rather than addresses.
#define DMA1_CNT REG_OFFSET_DMA1CNT
#define DMA2_CNT (REG_OFFSET_DMA1CNT + 0x0C)

// A four-word immediate transfer, used to nudge a channel that was mid-repeat.
#define DMA_KICK (((u32)(DMA_ENABLE | DMA_START_NOW | DMA_32BIT | DMA_SRC_INC \
                         | DMA_DEST_FIXED) << 16) | 4)

// Disabled, and then re-armed in the FIFO mode the mixer feeds.
#define DMA_OFF ((u16)DMA_32BIT)
#define DMA_FIFO ((u16)(DMA_ENABLE | DMA_START_SPECIAL | DMA_32BIT | DMA_REPEAT))

static u32 io_read32(int offset)
{
    return *(volatile u32 *)(agb_mem.io + offset);
}

static void io_write32(int offset, u32 value)
{
    *(volatile u32 *)(agb_mem.io + offset) = value;
}

static void io_write16(int offset, u16 value)
{
    *(volatile u16 *)(agb_mem.io + offset) = value;
}

// Restart a FIFO channel that is part-way through a repeat, so it begins the
// next buffer cleanly rather than continuing the old one.
static void dma_restart_if_repeating(int cnt)
{
    if (io_read32(cnt) & ((u32)DMA_REPEAT << 16))
        io_write32(cnt, DMA_KICK);
}

// Called once per frame from the game's own V-blank handler. It counts down to
// the moment the sound DMA has to be handed the next buffer, and re-arms the two
// FIFO channels when it arrives.
//
// The re-arming has no audible effect here -- the host consumes the mixer's PCM
// buffer directly rather than through a sound FIFO -- but the registers are
// written anyway, because the game can read them back and the cost is nothing.
void m4aSoundVSync(void)
{
    struct SoundInfo *info = sound_info();
    int counter;

    // The header is locked while the mixer is inside it, which shows up as an
    // ident one past its resting value. Either is fine; anything else means the
    // header is not ours to touch.
    if ((u32)(info->ident - ID_NUMBER) > 1)
        return;

    // The original decrements the byte and branches on the signed result, so a
    // counter of zero falls through here as well as a counter of one.
    counter = info->pcmDmaCounter;
    info->pcmDmaCounter = (u8)(counter - 1);
    if (counter - 1 > 0)
        return;

    info->pcmDmaCounter = info->pcmDmaPeriod;

    dma_restart_if_repeating(DMA1_CNT);
    dma_restart_if_repeating(DMA2_CNT);

    io_write16(DMA1_CNT + 2, DMA_OFF);
    io_write16(DMA2_CNT + 2, DMA_OFF);
    io_write16(DMA1_CNT + 2, DMA_FIFO);
    io_write16(DMA2_CNT + 2, DMA_FIFO);
}
