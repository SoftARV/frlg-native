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

// Two operands: an offset into the compatible-sound registers, then the byte to
// put there. The track data addresses those registers directly.
void ply_port(struct MusicPlayerInfo *player, struct MusicPlayerTrack *track)
{
    u8 offset = track_fetch(track);
    u8 value = track_fetch(track);

    (void)player;
    *(volatile u8 *)(agb_mem.io + REG_OFFSET_SOUND1CNT_L + offset) = value;
}
