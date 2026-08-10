// The track interpreter's parameter opcodes.
//
// Each reads its operands from the track's command stream and stores them where
// the sequencer and mixer look. What is easy to get wrong is not the arithmetic
// but the bookkeeping: how many bytes an opcode eats, and which recomputation it
// asks for afterwards. Both are checked for every one.

#include <stdint.h>
#include <string.h>

#include "agb/m4a.h"
#include "agb/memmap.h"

#include "harness.h"

#define FLG_VOLCHG 0x03
#define FLG_PITCHG 0x0C
#define CENTRE 0x40

static struct MusicPlayerInfo player;
static struct MusicPlayerTrack track;
static struct ToneData tones[4];
static u8 stream[16];

static void reset(const char *name, const int *bytes, int n)
{
    TEST_CASE(name);
    memset(&player, 0, sizeof(player));
    memset(&track, 0, sizeof(track));
    memset(tones, 0, sizeof(tones));
    memset(stream, 0, sizeof(stream));

    for (int i = 0; i < n; i++)
        stream[i] = (u8)bytes[i];

    track.cmdPtr = stream;
    player.tone = tones;
}

// How many bytes the opcode consumed.
static int eaten(void)
{
    return (int)(track.cmdPtr - stream);
}

static void test_prio(void)
{
    const int s[1] = {7};

    reset("prio", s, 1);
    ply_prio(&player, &track);
    CHECK(track.priority == 7, "priority was %u, not 7", track.priority);
    CHECK(eaten() == 1, "prio ate %d bytes, not 1", eaten());
    CHECK(track.flags == 0, "prio asked for a recomputation it does not need");
}

// Tempo arrives halved and is scaled by the player's multiplier.
static void test_tempo(void)
{
    const int s[1] = {75};

    reset("tempo", s, 1);
    player.tempoU = 0x100; // a multiplier of one, in 8.8
    ply_tempo(&player, &track);

    CHECK(player.tempoD == 150, "tempoD was %u, not 150", player.tempoD);
    // (150 * 256) >> 8 = 150.
    CHECK(player.tempoI == 150, "tempoI was %u, not 150", player.tempoI);
    CHECK(eaten() == 1, "tempo ate %d bytes, not 1", eaten());

    reset("tempo with a half multiplier", s, 1);
    player.tempoU = 0x80;
    ply_tempo(&player, &track);
    CHECK(player.tempoD == 150, "tempoD should not be scaled, got %u", player.tempoD);
    CHECK(player.tempoI == 75, "tempoI was %u, not 75", player.tempoI);
}

static void test_keysh(void)
{
    const int s[1] = {0xF6}; // -10 as a signed byte

    reset("keysh", s, 1);
    ply_keysh(&player, &track);
    CHECK(track.keyShift == -10, "keyShift was %d, not -10", track.keyShift);
    CHECK(eaten() == 1, "keysh ate %d bytes, not 1", eaten());
    CHECK(track.flags == (FLG_VOLCHG | FLG_PITCHG), "keysh set flags %02X", track.flags);
}

// Selecting an instrument copies the whole tone entry into the track.
static void test_voice(void)
{
    const int s[1] = {2};
    struct WaveData wave;

    reset("voice", s, 1);
    tones[2].type = 9;
    tones[2].key = 60;
    tones[2].wav = &wave;
    tones[2].attack = 11;
    tones[2].decay = 12;
    tones[2].sustain = 13;
    tones[2].release = 14;

    ply_voice(&player, &track);

    CHECK(track.tone.type == 9, "type was %u, not 9", track.tone.type);
    CHECK(track.tone.key == 60, "key was %u, not 60", track.tone.key);
    CHECK(track.tone.wav == &wave, "the wave pointer did not come across");
    CHECK(track.tone.attack == 11 && track.tone.decay == 12, "the envelope did not come across");
    CHECK(track.tone.sustain == 13 && track.tone.release == 14,
          "the rest of the envelope did not come across");
    CHECK(eaten() == 1, "voice ate %d bytes, not 1", eaten());
}

static void test_vol(void)
{
    const int s[1] = {90};

    reset("vol", s, 1);
    ply_vol(&player, &track);
    CHECK(track.vol == 90, "vol was %u, not 90", track.vol);
    CHECK(track.flags == FLG_VOLCHG, "vol set flags %02X, not the volume one", track.flags);
    CHECK(eaten() == 1, "vol ate %d bytes, not 1", eaten());
}

// Pan, bend and tune arrive biased so that centre is a positive byte.
static void test_centred_operands(void)
{
    const int centre[1] = {CENTRE};
    const int left[1] = {CENTRE - 16};

    reset("pan at centre", centre, 1);
    ply_pan(&player, &track);
    CHECK(track.pan == 0, "centre pan came out %d, not 0", track.pan);
    CHECK(track.flags == FLG_VOLCHG, "pan set flags %02X", track.flags);

    reset("pan off centre", left, 1);
    ply_pan(&player, &track);
    CHECK(track.pan == -16, "pan came out %d, not -16", track.pan);

    reset("bend off centre", left, 1);
    ply_bend(&player, &track);
    CHECK(track.bend == -16, "bend came out %d, not -16", track.bend);
    CHECK(track.flags == FLG_PITCHG, "bend set flags %02X, not the pitch one", track.flags);

    reset("tune off centre", left, 1);
    ply_tune(&player, &track);
    CHECK(track.tune == -16, "tune came out %d, not -16", track.tune);
    CHECK(track.flags == FLG_PITCHG, "tune set flags %02X, not the pitch one", track.flags);
}

static void test_bendr(void)
{
    const int s[1] = {12};

    reset("bendr", s, 1);
    ply_bendr(&player, &track);
    CHECK(track.bendRange == 12, "bendRange was %u, not 12", track.bendRange);
    CHECK(track.flags == FLG_PITCHG, "bendr set flags %02X", track.flags);
}

static void test_lfodl(void)
{
    const int s[1] = {5};

    reset("lfodl", s, 1);
    ply_lfodl(&player, &track);
    CHECK(track.lfoDelay == 5, "lfoDelay was %u, not 5", track.lfoDelay);
    CHECK(track.flags == 0, "lfodl asked for a recomputation it does not need");
}

// Changing what the modulation drives invalidates both; setting it to what it
// already was does nothing at all.
static void test_modt(void)
{
    const int s[1] = {1};

    reset("modt changing", s, 1);
    track.modT = 0;
    ply_modt(&player, &track);
    CHECK(track.modT == 1, "modT was %u, not 1", track.modT);
    CHECK(track.flags == (FLG_VOLCHG | FLG_PITCHG), "modt set flags %02X", track.flags);

    reset("modt unchanged", s, 1);
    track.modT = 1;
    ply_modt(&player, &track);
    CHECK(track.modT == 1, "modT changed when it should not have");
    CHECK(track.flags == 0, "setting modT to what it already was asked for a recomputation");
    CHECK(eaten() == 1, "modt did not eat its operand when the value matched");
}

// Zero stops the modulation sweep, and which recomputation that needs depends on
// what the modulation was driving.
static void test_modulation_stop(void)
{
    const int zero[1] = {0};
    const int some[1] = {4};

    reset("lfos to zero clears the sweep", zero, 1);
    track.modM = 30;
    track.lfoSpeedC = 9;
    track.modT = 0; // driving the pitch
    ply_lfos(&player, &track);
    CHECK(track.modM == 0 && track.lfoSpeedC == 0, "the sweep was not cleared");
    CHECK(track.flags == FLG_PITCHG, "a pitch modulation asked for %02X", track.flags);

    reset("lfos to zero on a volume modulation", zero, 1);
    track.modT = 1; // driving the volume
    ply_lfos(&player, &track);
    CHECK(track.flags == FLG_VOLCHG, "a volume modulation asked for %02X", track.flags);

    reset("lfos to non-zero leaves the sweep", some, 1);
    track.modM = 30;
    track.lfoSpeedC = 9;
    ply_lfos(&player, &track);
    CHECK(track.lfoSpeed == 4, "lfoSpeed was %u, not 4", track.lfoSpeed);
    CHECK(track.modM == 30 && track.lfoSpeedC == 9, "a non-zero speed cleared the sweep");
    CHECK(track.flags == 0, "a non-zero speed asked for a recomputation");

    reset("mod to zero clears the sweep", zero, 1);
    track.modM = 30;
    track.modT = 0;
    ply_mod(&player, &track);
    CHECK(track.modM == 0, "the sweep was not cleared");
    CHECK(track.flags == FLG_PITCHG, "mod to zero asked for %02X", track.flags);

    reset("mod to non-zero leaves the sweep", some, 1);
    track.modM = 30;
    ply_mod(&player, &track);
    CHECK(track.mod == 4, "mod was %u, not 4", track.mod);
    CHECK(track.modM == 30, "a non-zero depth cleared the sweep");
}

// Two operands, and the second goes straight into a sound register.
static void test_port(void)
{
    const int s[2] = {4, 0x5A};

    reset("port writes a sound register", s, 2);
    memset(&agb_mem, 0, sizeof(agb_mem));
    track.cmdPtr = stream;

    ply_port(&player, &track);

    CHECK(agb_mem.io[REG_OFFSET_SOUND1CNT_L + 4] == 0x5A,
          "the register at offset 4 holds %02X, not 5A",
          agb_mem.io[REG_OFFSET_SOUND1CNT_L + 4]);
    CHECK(agb_mem.io[REG_OFFSET_SOUND1CNT_L] == 0,
          "port wrote the base register instead of the offset one");
    CHECK(eaten() == 2, "port ate %d bytes, not 2", eaten());
}

// ------------------------------------------------------------ control flow ---

static struct SoundChannel chans[3];

// Put a pointer into the command stream as four little-endian bytes, the way
// track data holds a jump target.
static void put_target(int at, const void *target)
{
    uintptr_t value = (uintptr_t)target;

    for (int i = 0; i < 4; i++)
        stream[at + i] = (u8)(value >> (8 * i));
}

static void link_chain(int count)
{
    memset(chans, 0, sizeof(chans));
    track.chan = &chans[0];
    for (int i = 0; i < count; i++)
    {
        chans[i].track = &track;
        chans[i].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
        chans[i].nextChannelPointer = i + 1 < count ? &chans[i + 1] : NULL;
        chans[i].prevChannelPointer = i > 0 ? &chans[i - 1] : NULL;
    }
}

// The chain's head lives in the track, so unlinking the first channel is a
// different case from unlinking one in the middle.
static void test_clear_chain(void)
{
    reset("clear chain, middle", NULL, 0);
    link_chain(3);
    RealClearChain(&chans[1]);
    CHECK(chans[0].nextChannelPointer == &chans[2], "the previous channel was not relinked");
    CHECK(chans[2].prevChannelPointer == &chans[0], "the next channel was not relinked");
    CHECK(chans[1].track == NULL, "the channel kept its track");
    CHECK(track.chan == &chans[0], "the head moved when it should not have");

    reset("clear chain, head", NULL, 0);
    link_chain(3);
    RealClearChain(&chans[0]);
    CHECK(track.chan == &chans[1], "the head was not moved on");
    CHECK(chans[1].prevChannelPointer == NULL, "the new head kept a previous pointer");

    reset("clear chain, tail", NULL, 0);
    link_chain(3);
    RealClearChain(&chans[2]);
    CHECK(chans[1].nextChannelPointer == NULL, "the new tail kept a forward pointer");
    CHECK(track.chan == &chans[0], "the head moved when it should not have");

    reset("clear chain, already free", NULL, 0);
    link_chain(1);
    chans[0].track = NULL;
    track.chan = &chans[0];
    RealClearChain(&chans[0]);
    CHECK(track.chan == &chans[0], "a channel with no track still touched the chain");
}

// End of track: every channel is released rather than cut off, and unlinked.
static void test_fine(void)
{
    reset("fine releases every channel", NULL, 0);
    link_chain(3);
    track.flags = 0xFF;
    chans[1].statusFlags = 0; // already silent, so it should not be told to stop

    ply_fine(&player, &track);

    CHECK(chans[0].statusFlags & SOUND_CHANNEL_SF_STOP, "channel 0 was not released");
    CHECK(chans[2].statusFlags & SOUND_CHANNEL_SF_STOP, "channel 2 was not released");
    CHECK(chans[1].statusFlags == 0, "a silent channel was told to stop");
    for (int i = 0; i < 3; i++)
        CHECK(chans[i].track == NULL, "channel %d kept its track", i);
    CHECK(track.chan == NULL, "the chain was not emptied");
    CHECK(track.flags == 0, "the track's flags were not cleared, they are %02X", track.flags);
}

// A jump target is four little-endian bytes, and it replaces the pointer rather
// than being stepped over.
static void test_goto(void)
{
    reset("goto", NULL, 0);
    put_target(0, &stream[9]);
    ply_goto(&player, &track);
    CHECK(track.cmdPtr == &stream[9], "goto did not land on its target");
}

// Calling a pattern remembers where to resume, three deep.
static void test_patt_and_pend(void)
{
    reset("patt pushes and jumps", NULL, 0);
    put_target(0, &stream[12]);
    ply_patt(&player, &track);

    CHECK(track.patternLevel == 1, "the level is %u, not 1", track.patternLevel);
    CHECK(track.patternStack[0] == &stream[4], "the return address is past the operand, not at it");
    CHECK(track.cmdPtr == &stream[12], "patt did not jump");

    ply_pend(&player, &track);
    CHECK(track.patternLevel == 0, "the level did not come back down");
    CHECK(track.cmdPtr == &stream[4], "pend did not resume after the call");

    reset("pend at the outermost level does nothing", NULL, 0);
    track.cmdPtr = &stream[6];
    track.patternLevel = 0;
    ply_pend(&player, &track);
    CHECK(track.cmdPtr == &stream[6], "pend moved the pointer with nothing to return to");
    CHECK(track.patternLevel == 0, "pend underflowed the level");

    reset("a fourth call ends the track instead of overflowing", NULL, 0);
    link_chain(1);
    track.patternLevel = 3;
    put_target(0, &stream[12]);
    ply_patt(&player, &track);
    CHECK(track.patternLevel == 3, "the level went past three");
    CHECK(chans[0].statusFlags & SOUND_CHANNEL_SF_STOP, "the track was not ended");
    CHECK(track.cmdPtr != &stream[12], "an overflowing call jumped anyway");
}

// Repeat: a count of zero loops for ever; otherwise the jump is taken until the
// count is reached, then the whole operand is stepped over.
static void test_rept(void)
{
    reset("rept with a count of zero loops for ever", NULL, 0);
    stream[0] = 0;
    put_target(1, &stream[10]);
    for (int i = 0; i < 3; i++)
    {
        track.cmdPtr = stream;
        ply_rept(&player, &track);
        CHECK(track.cmdPtr == &stream[10], "pass %d did not loop", i);
        CHECK(track.repN == 0, "an endless repeat counted passes");
    }

    reset("rept counts its passes", NULL, 0);
    stream[0] = 3;
    put_target(1, &stream[10]);

    track.cmdPtr = stream;
    ply_rept(&player, &track);
    CHECK(track.repN == 1 && track.cmdPtr == &stream[10], "the first pass did not loop");

    track.cmdPtr = stream;
    ply_rept(&player, &track);
    CHECK(track.repN == 2 && track.cmdPtr == &stream[10], "the second pass did not loop");

    // The third reaches the count: the counter resets and the operand is skipped.
    track.cmdPtr = stream;
    ply_rept(&player, &track);
    CHECK(track.repN == 0, "the counter was not reset, it is %u", track.repN);
    CHECK(track.cmdPtr == &stream[5], "did not step past the count and the address, at +%d",
          (int)(track.cmdPtr - stream));
}

int main(void)
{
    test_prio();
    test_tempo();
    test_keysh();
    test_voice();
    test_vol();
    test_centred_operands();
    test_bendr();
    test_lfodl();
    test_modt();
    test_modulation_stop();
    test_port();
    test_clear_chain();
    test_fine();
    test_goto();
    test_patt_and_pend();
    test_rept();

    return test_report("m4a track opcodes");
}
