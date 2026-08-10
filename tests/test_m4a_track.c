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

// -------------------------------------------------------------- jump table ---

// The real template lives in the game's m4a_tables.c, which unit tests do not
// link, so they supply their own -- the same arrangement as the interrupt table.
static void marker_a(void) {}
static void marker_b(void) {}

void *const gMPlayJumpTableTemplate[36] = {
    marker_a,
    marker_b,
    /* the rest stay null, which is enough to see the whole table copied */
};

static void test_jump_table_copy(void)
{
    MPlayFunc table[36];

    TEST_CASE("jump table copy");
    for (int i = 0; i < 36; i++)
        table[i] = (MPlayFunc)marker_b; // something the copy has to replace

    MPlayJumpTableCopy(table);

    CHECK(table[0] == (MPlayFunc)marker_a, "entry 0 did not come from the template");
    CHECK(table[1] == (MPlayFunc)marker_b, "entry 1 did not come from the template");
    for (int i = 2; i < 36; i++)
        CHECK(table[i] == NULL, "entry %d was not copied, it is %p", i, (void *)table[i]);
}

// ------------------------------------------------- stopping and note ends ---

static int cgb_off_calls;
static u8 cgb_off_arg;
static struct SoundInfo sound_header;

static void fake_cgb_osc_off(u8 which)
{
    cgb_off_calls++;
    cgb_off_arg = which;
}

// TrackStop reaches the sound header the way the game does, through a fixed slot
// in IWRAM, so a test has to put it there.
static void install_sound_header(void)
{
    memset(&sound_header, 0, sizeof(sound_header));
    sound_header.CgbOscOff = fake_cgb_osc_off;
    cgb_off_calls = 0;
    cgb_off_arg = 0xFF;
    *(struct SoundInfo **)(agb_mem.iwram + 0x7FF0) = &sound_header;
}

// Stopping cuts channels off outright, where ending a track releases them.
static void test_track_stop(void)
{
    reset("track stop cuts channels off", NULL, 0);
    install_sound_header();
    link_chain(2);
    track.flags = MPT_FLG_EXIST;

    TrackStop(&player, &track);

    CHECK(chans[0].statusFlags == 0, "channel 0 was not silenced, it is %02X",
          chans[0].statusFlags);
    CHECK(chans[1].statusFlags == 0, "channel 1 was not silenced");
    CHECK(chans[0].track == NULL && chans[1].track == NULL, "a channel kept its track");
    CHECK(track.chan == NULL, "the chain was not emptied");
    CHECK(cgb_off_calls == 0, "a mixed channel asked the hardware to stop an oscillator");

    reset("track stop on a track that does not exist", NULL, 0);
    install_sound_header();
    link_chain(1);
    track.flags = 0; // no EXIST bit
    TrackStop(&player, &track);
    CHECK(chans[0].statusFlags != 0, "a track with no EXIST flag was stopped anyway");
    CHECK(track.chan == &chans[0], "the chain was emptied anyway");

    reset("track stop turns off a compatible-sound oscillator", NULL, 0);
    install_sound_header();
    link_chain(1);
    track.flags = MPT_FLG_EXIST;
    chans[0].type = TONEDATA_TYPE_CGB & 0x03; // a CGB channel type
    TrackStop(&player, &track);
    CHECK(cgb_off_calls == 1, "the oscillator was told to stop %d times, not once", cgb_off_calls);
    CHECK(cgb_off_arg == (TONEDATA_TYPE_CGB & 0x03),
          "it was handed %02X rather than the channel type", cgb_off_arg);

    reset("track stop masks the type down to the oscillator", NULL, 0);
    install_sound_header();
    link_chain(1);
    track.flags = MPT_FLG_EXIST;
    // Bits above the oscillator field must not reach the header.
    chans[0].type = 0x08 | 0x03;
    TrackStop(&player, &track);
    CHECK(cgb_off_calls == 1, "the oscillator was not stopped");
    CHECK(cgb_off_arg == 0x03, "it was handed %02X rather than just the oscillator bits",
          cgb_off_arg);

    reset("track stop leaves an already silent channel alone", NULL, 0);
    install_sound_header();
    link_chain(1);
    track.flags = MPT_FLG_EXIST;
    chans[0].statusFlags = 0;
    chans[0].type = TONEDATA_TYPE_CGB & 0x03;
    TrackStop(&player, &track);
    CHECK(cgb_off_calls == 0, "a silent channel still stopped an oscillator");
    CHECK(chans[0].track == NULL, "a silent channel kept its track");
}

// Velocity is split across the two sides by the pan, and the two are not quite
// symmetrical.
static void test_chn_vol_set(void)
{
    reset("channel volume, centred", NULL, 0);
    chans[0].velocity = 127;
    chans[0].rhythmPan = 0;
    track.volMR = 128;
    track.volML = 128;

    ChnVolSetAsm(&chans[0], &track);

    // right: (128 * ((0x80 + 0) * 127)) >> 14 = (128 * 16256) >> 14 = 127
    // left:  (128 * ((0x7F - 0) * 127)) >> 14 = (128 * 16129) >> 14 = 126
    CHECK(chans[0].rightVolume == 127, "right was %u, not 127", chans[0].rightVolume);
    CHECK(chans[0].leftVolume == 126, "left was %u, not 126", chans[0].leftVolume);

    reset("channel volume, panned right", NULL, 0);
    chans[0].velocity = 127;
    chans[0].rhythmPan = 63;
    track.volMR = 128;
    track.volML = 128;
    ChnVolSetAsm(&chans[0], &track);
    // right: (128 * (191 * 127)) >> 14 = 189; left: (128 * (64 * 127)) >> 14 = 63
    CHECK(chans[0].rightVolume == 189, "right was %u, not 189", chans[0].rightVolume);
    CHECK(chans[0].leftVolume == 63, "left was %u, not 63", chans[0].leftVolume);

    reset("channel volume, panned left", NULL, 0);
    chans[0].velocity = 127;
    chans[0].rhythmPan = -64; // signed: a negative pan is to the left
    track.volMR = 128;
    track.volML = 128;
    ChnVolSetAsm(&chans[0], &track);
    // right: (128 * ((0x80 - 64) * 127)) >> 14 = 63
    // left:  (128 * ((0x7F + 64) * 127)) >> 14 = 189
    CHECK(chans[0].rightVolume == 63, "right was %u, not 63", chans[0].rightVolume);
    CHECK(chans[0].leftVolume == 189, "left was %u, not 189", chans[0].leftVolume);

    reset("channel volume clamps", NULL, 0);
    chans[0].velocity = 255;
    chans[0].rhythmPan = 0;
    track.volMR = 255;
    track.volML = 255;
    ChnVolSetAsm(&chans[0], &track);
    CHECK(chans[0].rightVolume == 0xFF, "right did not clamp, it is %u", chans[0].rightVolume);
    CHECK(chans[0].leftVolume == 0xFF, "left did not clamp, it is %u", chans[0].leftVolume);

    reset("channel volume, silent note", NULL, 0);
    chans[0].velocity = 0;
    track.volMR = 255;
    track.volML = 255;
    ChnVolSetAsm(&chans[0], &track);
    CHECK(chans[0].rightVolume == 0 && chans[0].leftVolume == 0,
          "a note with no velocity was audible");
}

// Ending a tie releases the first channel holding that key, and only that one.
static void test_endtie(void)
{
    reset("endtie with a key operand", NULL, 0);
    stream[0] = 60;
    link_chain(2);
    chans[0].midiKey = 55;
    chans[1].midiKey = 60;
    chans[0].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    chans[1].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    track.cmdPtr = stream;

    ply_endtie(&player, &track);

    CHECK(track.key == 60, "the running key was not updated, it is %u", track.key);
    CHECK(eaten() == 1, "endtie ate %d bytes, not 1", eaten());
    CHECK((chans[1].statusFlags & SOUND_CHANNEL_SF_STOP) != 0, "the matching note was not released");
    CHECK((chans[0].statusFlags & SOUND_CHANNEL_SF_STOP) == 0, "a note on another key was released");

    reset("endtie with no operand uses the running key", NULL, 0);
    stream[0] = 0x80; // not a key, so it belongs to whatever comes next
    link_chain(1);
    track.key = 44;
    chans[0].midiKey = 44;
    chans[0].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    track.cmdPtr = stream;

    ply_endtie(&player, &track);

    CHECK(eaten() == 0, "endtie consumed a byte that was not its operand");
    CHECK(track.key == 44, "the running key changed");
    CHECK((chans[0].statusFlags & SOUND_CHANNEL_SF_STOP) != 0, "the running note was not released");

    reset("endtie treats 0x7F as a key", NULL, 0);
    stream[0] = 0x7F; // the highest value that is still a key
    link_chain(1);
    chans[0].midiKey = 0x7F;
    chans[0].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    track.cmdPtr = stream;
    ply_endtie(&player, &track);
    CHECK(eaten() == 1, "0x7F was not taken as a key");
    CHECK(track.key == 0x7F, "the running key is %u, not 0x7F", track.key);

    reset("endtie releases only the first match", NULL, 0);
    stream[0] = 60;
    link_chain(3);
    for (int i = 0; i < 3; i++)
    {
        chans[i].midiKey = 60;
        chans[i].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    }
    track.cmdPtr = stream;
    ply_endtie(&player, &track);
    CHECK((chans[0].statusFlags & SOUND_CHANNEL_SF_STOP) != 0, "the first match was not released");
    CHECK((chans[1].statusFlags & SOUND_CHANNEL_SF_STOP) == 0, "a second match was released too");

    reset("endtie skips a note already stopping", NULL, 0);
    stream[0] = 60;
    link_chain(2);
    chans[0].midiKey = 60;
    chans[0].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN | SOUND_CHANNEL_SF_STOP;
    chans[1].midiKey = 60;
    chans[1].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    track.cmdPtr = stream;
    ply_endtie(&player, &track);
    CHECK((chans[1].statusFlags & SOUND_CHANNEL_SF_STOP) != 0,
          "it stopped at a note that was already ending instead of moving on");

    reset("endtie skips a silent channel", NULL, 0);
    stream[0] = 60;
    link_chain(2);
    chans[0].midiKey = 60;
    chans[0].statusFlags = 0; // neither starting nor in an envelope
    chans[1].midiKey = 60;
    chans[1].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    track.cmdPtr = stream;
    ply_endtie(&player, &track);
    CHECK((chans[1].statusFlags & SOUND_CHANNEL_SF_STOP) != 0, "a silent channel absorbed the tie");
}

// ------------------------------------------------------------ starting a note ---

// ply_note reaches into the sequencer for these four. None of it is built yet,
// so the test stands in for all of them -- and being spies, they also record
// that the note went through the steps in the right order.

const u8 gClockTable[] = {0, 3, 6, 12, 24, 48, 96};

static int clear_chain_calls;
static void *clear_chain_arg;
static int trk_vol_pit_calls;
static struct WaveData *freq_wav;
static u8 freq_key;
static u8 freq_fine;
static u8 cgb_freq_type;
static u8 cgb_freq_key;
static u8 cgb_freq_fine;

void ClearChain(void *x)
{
    clear_chain_calls++;
    clear_chain_arg = x;
    RealClearChain(x);
}

void TrkVolPitSet(struct MusicPlayerInfo *mplayInfo, struct MusicPlayerTrack *t)
{
    (void)mplayInfo;
    (void)t;
    trk_vol_pit_calls++;
}

u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust)
{
    freq_wav = wav;
    freq_key = key;
    freq_fine = fineAdjust;
    return 0xF00D;
}

static u32 fake_cgb_freq(u8 type, u8 key, u8 fineAdjust)
{
    cgb_freq_type = type;
    cgb_freq_key = key;
    cgb_freq_fine = fineAdjust;
    return 0xC0FFEE;
}

// The mixed-channel pool is embedded in the sound header rather than pointed
// at, so the note tests work on the header's own channels.
#define pool (sound_header.chans)

static struct CgbChannel cgb_chans[4];

// Ties are settled on track address, so testing them needs two tracks whose
// order in memory is known. An array guarantees that; two separate objects do
// not, and a test that only asserts when they happen to fall the right way is no
// test at all.
static struct MusicPlayerTrack ordered[2];
static u8 ordered_stream[2] = {0x80, 0x80};

#define LOWER (&ordered[0])
#define HIGHER (&ordered[1])

static void arm_track(struct MusicPlayerTrack *t, u8 priority)
{
    memset(t, 0, sizeof(*t));
    t->cmdPtr = ordered_stream;
    t->priority = priority;
    t->key = 60;
}

// A key split's table is held where a plain instrument keeps its envelope, so it
// is written the same way the interpreter reads it.
static void set_key_split_table(struct ToneData *tone, const u8 *table)
{
    *(const u8 **)&tone->attack = table;
}

// A note needs rather more standing around it than a parameter opcode: a sound
// header holding the channel pool, an instrument, and somewhere to put the note.
static void note_reset(const char *name, const int *bytes, int n)
{
    reset(name, bytes, n);
    install_sound_header();
    memset(cgb_chans, 0, sizeof(cgb_chans));

    sound_header.maxChans = 3;
    sound_header.cgbChans = cgb_chans;
    sound_header.MidiKeyToCgbFreq = fake_cgb_freq;

    clear_chain_calls = 0;
    clear_chain_arg = NULL;
    trk_vol_pit_calls = 0;
    freq_wav = NULL;
    freq_key = 0xFF;
    freq_fine = 0xFF;
    cgb_freq_type = 0xFF;
    cgb_freq_key = 0xFF;
    cgb_freq_fine = 0xFF;

    track.key = 60;
    track.velocity = 100;
}

// Up to three operand bytes may follow, each optional and each recognised only
// by being below 0x80. A note that supplies none repeats the previous one.
static void test_note_operands(void)
{
    const int none[1] = {0x80}; // the next opcode, not an operand

    note_reset("note with no operands", none, 1);
    ply_note(3, &player, &track);
    CHECK(track.gateTime == 12, "gate time came from the wrong clock entry: %d",
          track.gateTime);
    CHECK(track.key == 60, "the key changed without an operand");
    CHECK(track.velocity == 100, "the velocity changed without an operand");
    CHECK(eaten() == 0, "%d operand bytes were eaten, not none", eaten());

    const int key_only[2] = {55, 0x80};
    note_reset("note with a key", key_only, 2);
    ply_note(1, &player, &track);
    CHECK(track.key == 55, "the key operand did not land, it is %d", track.key);
    CHECK(track.velocity == 100, "the velocity changed without an operand");
    CHECK(track.gateTime == 3, "gate time came out %d", track.gateTime);
    CHECK(eaten() == 1, "%d bytes were eaten, not one", eaten());

    const int key_vel[3] = {55, 20, 0x80};
    note_reset("note with a key and a velocity", key_vel, 3);
    ply_note(1, &player, &track);
    CHECK(track.velocity == 20, "the velocity operand did not land, it is %d",
          track.velocity);
    CHECK(track.gateTime == 3, "gate time changed without a third operand");
    CHECK(eaten() == 2, "%d bytes were eaten, not two", eaten());

    const int all_three[4] = {55, 20, 5, 0x80};
    note_reset("note with an added gate time", all_three, 4);
    ply_note(3, &player, &track);
    CHECK(track.gateTime == 12 + 5, "gate time came out %d, not the sum", track.gateTime);
    CHECK(eaten() == 3, "%d bytes were eaten, not three", eaten());
}

// The three operands are positional: a velocity cannot be given without a key,
// and stopping early must not consume the byte that ended the run.
static void test_note_operands_stop_early(void)
{
    const int stops[3] = {55, 0x90, 20};

    note_reset("an operand run stops at the first high byte", stops, 3);
    ply_note(1, &player, &track);
    CHECK(track.key == 55, "the key did not land");
    CHECK(track.velocity == 100, "the velocity was taken from beyond the run");
    CHECK(eaten() == 1, "%d bytes were eaten; the high byte must be left", eaten());
}

// Gate time is a byte and the addition is not widened, but it cannot actually
// overflow: the largest clock entry is 96 and an operand is at most 127, so the
// sum tops out at 223. The largest reachable sum is pinned here instead -- if the
// clock table ever grows, this is what will notice.
static void test_note_gate_time_maximum(void)
{
    const int s[4] = {55, 20, 127, 0x80};

    note_reset("the largest reachable gate time", s, 4);
    ply_note(6, &player, &track); // the last entry of the test's table, 96
    CHECK(track.gateTime == 96 + 127, "gate time came out %d, not 223", track.gateTime);
    CHECK(eaten() == 3, "%d bytes were eaten, not three", eaten());
}

// A plain instrument answers for itself, and everything about it reaches the
// channel.
static void test_note_plain_instrument(void)
{
    const int s[1] = {0x80};
    static struct WaveData wave;

    note_reset("a plain instrument sets the channel up", s, 1);
    track.tone.type = 0;
    track.tone.wav = &wave;
    track.tone.attack = 1;
    track.tone.decay = 2;
    track.tone.sustain = 3;
    track.tone.release = 4;
    track.pseudoEchoVolume = 50;
    track.pseudoEchoLength = 60;
    track.unk_3C = 0x1234;
    track.flags = 0xFF;

    ply_note(2, &player, &track);

    CHECK(track.chan == &pool[0], "the note did not take the first idle channel");
    CHECK(pool[0].wav == &wave, "the waveform did not reach the channel");
    CHECK(pool[0].attack == 1 && pool[0].decay == 2 && pool[0].sustain == 3
              && pool[0].release == 4,
          "the envelope did not reach the channel");
    CHECK(pool[0].pseudoEchoVolume == 50 && pool[0].pseudoEchoLength == 60,
          "the pseudo-echo pair did not reach the channel");
    CHECK(pool[0].gateTime == 6, "the gate time did not reach the channel");
    CHECK(pool[0].midiKey == 60, "the midi key did not reach the channel");
    CHECK(pool[0].velocity == 100, "the velocity did not reach the channel");
    CHECK(pool[0].key == 60, "the played key came out %d", pool[0].key);
    CHECK(pool[0].count == 0x1234, "the channel's count did not come from the track");
    CHECK(pool[0].frequency == 0xF00D, "the frequency did not come from the sequencer");
    CHECK(pool[0].statusFlags == SOUND_CHANNEL_SF_START,
          "the channel was not started, its flags are %02X", pool[0].statusFlags);
    CHECK(track.flags == 0xF0, "the track's low flags were not cleared, they are %02X",
          track.flags);
    CHECK(freq_wav == &wave, "the sequencer was handed the wrong waveform");
    CHECK(trk_vol_pit_calls == 1, "the volume was recomputed %d times, not once",
          trk_vol_pit_calls);
    CHECK(clear_chain_calls == 1 && clear_chain_arg == &pool[0],
          "the channel was not unhooked before being taken");
    CHECK(pool[0].track == &track, "the channel was not given its track");
}

// A key-split instrument redirects through a table, and the entries it indexes
// are twelve bytes apart. The table and the entries are two separate pointers,
// held at two different offsets of the same instrument.
static void test_note_key_split(void)
{
    const int s[1] = {0x80};
    static u8 table[128];
    static u8 block[4 * 12];
    struct ToneData *entries = (struct ToneData *)block;
    static struct WaveData wave;

    note_reset("a key split redirects through its table", s, 1);
    memset(table, 0, sizeof(table));
    memset(block, 0, sizeof(block));
    // Not the identity, so a lookup that skipped the table shows up; and the
    // entries carry distinct envelopes, so a wrong stride shows up too.
    table[60] = 2;
    entries[1].attack = 11;
    entries[2].attack = 22;
    entries[2].decay = 33;
    entries[2].wav = &wave;
    entries[2].key = 77; // a split entry's own key is not used

    track.tone.type = TONEDATA_TYPE_SPL;
    track.tone.wav = (struct WaveData *)block;
    set_key_split_table(&track.tone, table);

    ply_note(1, &player, &track);

    CHECK(pool[0].attack == 22 && pool[0].decay == 33,
          "the wrong split entry was chosen: attack %d, decay %d", pool[0].attack,
          pool[0].decay);
    CHECK(pool[0].wav == &wave, "the entry's waveform did not reach the channel");
    CHECK(pool[0].key == 60, "a key split must keep the track's key, not the entry's");

    note_reset("a key split with an identity entry", s, 1);
    memset(table, 0, sizeof(table));
    memset(block, 0, sizeof(block));
    table[60] = 1;
    entries[1].attack = 11;
    track.tone.type = TONEDATA_TYPE_SPL;
    track.tone.wav = (struct WaveData *)block;
    set_key_split_table(&track.tone, table);
    ply_note(1, &player, &track);
    CHECK(pool[0].attack == 11, "the entry one along was not reached, attack is %d",
          pool[0].attack);
}

// A rhythm instrument indexes by key, then plays the entry's own key -- and may
// carry a pan with it.
static void test_note_rhythm(void)
{
    const int s[1] = {0x80};
    static u8 block[4 * 12];
    struct ToneData *entries = (struct ToneData *)block;
    static struct WaveData wave;

    note_reset("a rhythm entry brings its own key", s, 1);
    memset(block, 0, sizeof(block));
    entries[2].type = 0;
    entries[2].key = 77;
    entries[2].wav = &wave;
    entries[2].pan_sweep = 0; // no pan
    track.key = 2;
    track.tone.type = TONEDATA_TYPE_RHY;
    track.tone.wav = (struct WaveData *)block;

    ply_note(1, &player, &track);

    CHECK(pool[0].key == 77, "the entry's own key was not used, the channel has %d",
          pool[0].key);
    CHECK(pool[0].midiKey == 2, "the track's key should still reach midiKey, it is %d",
          pool[0].midiKey);
    CHECK(pool[0].rhythmPan == 0, "a pan appeared without one being asked for");
    CHECK(pool[0].wav == &wave, "the entry's waveform did not reach the channel");

    note_reset("a rhythm entry with a pan", s, 1);
    memset(block, 0, sizeof(block));
    entries[2].type = 0;
    entries[2].key = 77;
    entries[2].pan_sweep = 0xC0 + 5; // biased, bit seven set
    track.key = 2;
    track.tone.type = TONEDATA_TYPE_RHY;
    track.tone.wav = (struct WaveData *)block;

    ply_note(1, &player, &track);

    CHECK(pool[0].rhythmPan == 10, "the pan came out %d, not the debiased double",
          pool[0].rhythmPan);
}

// One level of redirection is all the format allows.
static void test_note_nested_redirect_is_dropped(void)
{
    const int s[1] = {0x80};
    static u8 block[4 * 12];
    struct ToneData *entries = (struct ToneData *)block;

    note_reset("a rhythm entry that is itself a rhythm is dropped", s, 1);
    memset(block, 0, sizeof(block));
    entries[2].type = TONEDATA_TYPE_RHY;
    track.key = 2;
    track.tone.type = TONEDATA_TYPE_RHY;
    track.tone.wav = (struct WaveData *)block;

    ply_note(1, &player, &track);

    CHECK(track.chan == NULL, "a nested redirect still claimed a channel");
    CHECK(clear_chain_calls == 0, "a dropped note still unhooked a channel");
    // The operands are read before the instrument is resolved, so the gate time
    // still moved.
    CHECK(track.gateTime == 3, "the gate time should still have been set");

    note_reset("a rhythm entry that is a key split is dropped", s, 1);
    memset(block, 0, sizeof(block));
    entries[2].type = TONEDATA_TYPE_SPL;
    track.key = 2;
    track.tone.type = TONEDATA_TYPE_RHY;
    track.tone.wav = (struct WaveData *)block;
    ply_note(1, &player, &track);
    CHECK(track.chan == NULL, "a nested key split still claimed a channel");
}

// The note's priority is the track's plus the player's, and it saturates.
static void test_note_priority(void)
{
    const int s[1] = {0x80};

    note_reset("note priority sums track and player", s, 1);
    track.priority = 30;
    player.priority = 12;
    ply_note(1, &player, &track);
    CHECK(pool[0].priority == 42, "priority came out %d, not the sum",
          pool[0].priority);

    note_reset("note priority saturates", s, 1);
    track.priority = 200;
    player.priority = 100;
    ply_note(1, &player, &track);
    CHECK(pool[0].priority == 0xFF, "priority came out %d rather than saturating",
          pool[0].priority);
}

// An idle channel is taken outright, before any of the stealing rules apply.
static void test_note_takes_an_idle_channel(void)
{
    const int s[1] = {0x80};

    note_reset("an idle channel wins over a better victim", s, 1);
    // Channel 0 is sounding and would never be chosen as a victim; channel 1 is
    // idle. The search must stop at 1 rather than looking for the best steal.
    pool[0].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    pool[0].priority = 0;
    pool[1].statusFlags = 0;
    pool[2].statusFlags = SOUND_CHANNEL_SF_STOP;

    track.priority = 10;
    ply_note(1, &player, &track);

    CHECK(track.chan == &pool[1], "the idle channel was not the one taken");
}

// With everything busy the least worthy channel is stolen: lowest priority.
static void test_note_steals_lowest_priority(void)
{
    const int s[1] = {0x80};

    note_reset("the lowest priority channel is stolen", s, 1);
    for (int i = 0; i < 3; i++)
    {
        pool[i].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
        pool[i].track = &track;
    }
    pool[0].priority = 9;
    pool[1].priority = 3;
    pool[2].priority = 7;

    track.priority = 20;
    ply_note(1, &player, &track);

    CHECK(track.chan == &pool[1], "the wrong channel was stolen");
}

// A releasing channel is a better victim than any sounding one, however much
// less important the sounding one looks.
static void test_note_prefers_a_releasing_channel(void)
{
    const int s[1] = {0x80};

    note_reset("a releasing channel is stolen before a sounding one", s, 1);
    for (int i = 0; i < 3; i++)
        pool[i].track = &track;
    // Channel 0 is sounding at the lowest priority there is, so the priority
    // rule alone would pick it. Channel 2 is releasing at the highest.
    pool[0].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    pool[0].priority = 1;
    pool[1].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    pool[1].priority = 2;
    pool[2].statusFlags = SOUND_CHANNEL_SF_STOP;
    pool[2].priority = 250;

    track.priority = 20;
    ply_note(1, &player, &track);

    CHECK(track.chan == &pool[2], "a sounding channel was stolen over a releasing one");
}

// Once a releasing channel is in hand, later sounding ones are not even weighed;
// but a second releasing one still competes on priority.
static void test_note_releasing_channels_compete(void)
{
    const int s[1] = {0x80};

    note_reset("two releasing channels are compared on priority", s, 1);
    for (int i = 0; i < 3; i++)
        pool[i].track = &track;
    pool[0].statusFlags = SOUND_CHANNEL_SF_STOP;
    pool[0].priority = 100;
    pool[1].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    pool[1].priority = 1; // must be ignored: it is only sounding
    pool[2].statusFlags = SOUND_CHANNEL_SF_STOP;
    pool[2].priority = 50;

    track.priority = 200;
    ply_note(1, &player, &track);

    CHECK(track.chan == &pool[2], "the quieter releasing channel was not preferred");
}

// Equal priorities are settled by track address, so the choice is stable rather
// than depending on which channel happened to be looked at first.
static void test_note_ties_break_on_track(void)
{
    const int s[1] = {0x80};

    note_reset("an equal-priority tie goes to the higher track address", s, 1);
    arm_track(LOWER, 5);
    for (int i = 0; i < 3; i++)
    {
        pool[i].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
        pool[i].priority = 5;
    }
    // Only the middle channel's track outranks the newcomer's, so it is the one
    // that has to be given up.
    pool[0].track = LOWER;
    pool[1].track = HIGHER;
    pool[2].track = LOWER;

    ply_note(1, &player, LOWER);

    CHECK(LOWER->chan == &pool[1], "the tie was not broken towards the higher address");

    // Channels tied on both priority and track: the search keeps moving, so the
    // last of them is the one taken rather than the first.
    note_reset("an exact tie goes to the last channel, not the first", s, 1);
    arm_track(LOWER, 5);
    for (int i = 0; i < 3; i++)
    {
        pool[i].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
        pool[i].priority = 5;
        pool[i].track = HIGHER;
    }

    ply_note(1, &player, LOWER);

    CHECK(LOWER->chan == &pool[2], "the last of an exact tie was not the one taken");
}

// The search starts out holding the newcomer's own priority and track, which is
// what stops it from stealing a channel that outranks it on either count.
static void test_note_will_not_steal_from_below(void)
{
    const int s[1] = {0x80};

    note_reset("an equal-priority channel below the newcomer keeps it", s, 1);
    arm_track(HIGHER, 5);
    // Equal priority, but the track holding it sits below the newcomer's, so the
    // tie-break goes against the newcomer and nothing is taken.
    pool[0].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    pool[0].priority = 5;
    pool[0].track = LOWER;
    pool[1].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    pool[1].priority = 5;
    pool[1].track = LOWER;
    pool[2].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    pool[2].priority = 5;
    pool[2].track = LOWER;

    ply_note(1, &player, HIGHER);

    CHECK(HIGHER->chan == NULL, "a channel below the newcomer was stolen anyway");
    CHECK(clear_chain_calls == 0, "a dropped note still unhooked a channel");
}

// Every channel busy and more important than the newcomer: the note is dropped.
static void test_note_dropped_when_nothing_is_stealable(void)
{
    const int s[1] = {0x80};

    note_reset("a note with nothing to steal is dropped", s, 1);
    for (int i = 0; i < 3; i++)
    {
        pool[i].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
        pool[i].priority = 200;
        pool[i].track = &track;
    }

    track.priority = 10;
    player.priority = 0;
    ply_note(1, &player, &track);

    CHECK(track.chan == NULL, "a channel was stolen that should have been safe");
    CHECK(clear_chain_calls == 0, "a dropped note still unhooked a channel");
}

// A compatible-sound note has one channel it can play on, chosen by type.
static void test_note_cgb_channel_choice(void)
{
    const int s[1] = {0x80};

    note_reset("a compatible-sound note picks its channel by type", s, 1);
    track.tone.type = 3; // within TONEDATA_TYPE_CGB
    track.tone.length = 42;

    ply_note(1, &player, &track);

    CHECK(track.chan == (struct SoundChannel *)&cgb_chans[2],
          "the note did not land on the third oscillator");
    CHECK(cgb_chans[2].length == 42, "the sound length did not reach the channel");
    CHECK(cgb_chans[2].frequency == 0xC0FFEE,
          "the frequency did not come from the compatible-sound table");
    CHECK(cgb_freq_type == 3, "the table was handed type %d", cgb_freq_type);
    CHECK(pool[0].statusFlags == 0, "a mixed channel was disturbed");

    note_reset("a compatible-sound note with no channel pool is dropped", s, 1);
    sound_header.cgbChans = NULL;
    track.tone.type = 1;
    ply_note(1, &player, &track);
    CHECK(track.chan == NULL, "a note was placed with no pool to place it in");
}

// The same stealing rules, but with only one candidate to apply them to.
static void test_note_cgb_stealing(void)
{
    const int s[1] = {0x80};

    note_reset("a busier compatible-sound channel is not taken", s, 1);
    cgb_chans[0].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    cgb_chans[0].priority = 200;
    cgb_chans[0].track = &track;
    track.tone.type = 1;
    track.priority = 10;
    ply_note(1, &player, &track);
    CHECK(track.chan == NULL, "a more important note was cut off");

    note_reset("a releasing compatible-sound channel is taken", s, 1);
    cgb_chans[0].statusFlags = SOUND_CHANNEL_SF_STOP;
    cgb_chans[0].priority = 200;
    track.tone.type = 1;
    track.priority = 10;
    ply_note(1, &player, &track);
    CHECK(track.chan == (struct SoundChannel *)&cgb_chans[0],
          "a releasing channel was not reused");

    note_reset("a less important compatible-sound channel is taken", s, 1);
    cgb_chans[0].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    cgb_chans[0].priority = 5;
    track.tone.type = 1;
    track.priority = 10;
    player.priority = 0;
    ply_note(1, &player, &track);
    CHECK(track.chan == (struct SoundChannel *)&cgb_chans[0],
          "a channel of lower priority was not taken");

    note_reset("an equal-priority compatible-sound tie goes on track address", s, 1);
    arm_track(HIGHER, 10);
    HIGHER->tone.type = 1;
    cgb_chans[0].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    cgb_chans[0].priority = 10;
    // A track below the newcomer's keeps the channel.
    cgb_chans[0].track = LOWER;
    ply_note(1, &player, HIGHER);
    CHECK(HIGHER->chan == NULL, "a lower-addressed track lost its channel");

    note_reset("an equal-priority tie at the same track is taken", s, 1);
    arm_track(LOWER, 10);
    LOWER->tone.type = 1;
    cgb_chans[0].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    cgb_chans[0].priority = 10;
    cgb_chans[0].track = HIGHER;
    ply_note(1, &player, LOWER);
    CHECK(LOWER->chan == (struct SoundChannel *)&cgb_chans[0],
          "a higher-addressed track kept its channel");
}

// The key shift lands on the played key, and does not wrap when it goes under.
static void test_note_key_shift(void)
{
    const int s[1] = {0x80};

    note_reset("the key shift moves the played key", s, 1);
    track.key = 60;
    track.keyM = 5;
    track.pitM = 7;
    ply_note(1, &player, &track);
    CHECK(freq_key == 65, "the shifted key came out %d", freq_key);
    CHECK(freq_fine == 7, "the fine adjustment did not reach the sequencer");
    CHECK(pool[0].key == 60, "the channel's key should be the unshifted one, it is %d",
          pool[0].key);

    note_reset("a key shifted below zero bottoms out", s, 1);
    track.key = 3;
    track.keyM = (u8)-10;
    ply_note(1, &player, &track);
    CHECK(freq_key == 0, "the shifted key came out %d rather than bottoming out", freq_key);
}

// A pan request is not a sweep, and an empty sweep field is not one either.
static void test_note_cgb_sweep(void)
{
    const int s[1] = {0x80};

    note_reset("a real sweep reaches the channel", s, 1);
    track.tone.type = 1;
    track.tone.pan_sweep = 0x30; // within the sweep field, bit seven clear
    ply_note(1, &player, &track);
    CHECK(cgb_chans[0].sweep == 0x30, "the sweep came out %02X", cgb_chans[0].sweep);

    note_reset("a pan request is not treated as a sweep", s, 1);
    track.tone.type = 1;
    track.tone.pan_sweep = 0x80 | 0x30;
    ply_note(1, &player, &track);
    CHECK(cgb_chans[0].sweep == 8, "a pan request became sweep %02X",
          cgb_chans[0].sweep);

    note_reset("an empty sweep field falls back", s, 1);
    track.tone.type = 1;
    track.tone.pan_sweep = 0x0F; // nothing in the sweep field
    ply_note(1, &player, &track);
    CHECK(cgb_chans[0].sweep == 8, "an empty sweep came out %02X", cgb_chans[0].sweep);
}

// The channel goes to the head of the track's chain, and the modulation delay
// restarts with it.
static void test_note_chains_and_delay(void)
{
    const int s[1] = {0x80};

    note_reset("a second note chains ahead of the first", s, 1);
    pool[0].statusFlags = SOUND_CHANNEL_SF_ENV_SUSTAIN;
    pool[0].track = &track;
    track.chan = &pool[0];

    ply_note(1, &player, &track);

    CHECK(track.chan == &pool[1], "the new channel is not at the head");
    CHECK(pool[1].nextChannelPointer == &pool[0], "the old head was not linked behind");
    CHECK(pool[0].prevChannelPointer == &pool[1], "the old head does not point back");
    CHECK(pool[1].prevChannelPointer == NULL, "the head has something before it");

    note_reset("a modulation delay restarts and clears the sweep", s, 1);
    track.lfoDelay = 4;
    track.modM = 20;
    track.lfoSpeedC = 9;
    track.modT = 0;
    ply_note(1, &player, &track);
    CHECK(track.lfoDelayC == 4, "the delay counter did not reload, it is %d",
          track.lfoDelayC);
    CHECK(track.modM == 0 && track.lfoSpeedC == 0, "the modulation sweep was not cleared");

    note_reset("no modulation delay leaves the sweep alone", s, 1);
    track.lfoDelay = 0;
    track.modM = 20;
    ply_note(1, &player, &track);
    CHECK(track.modM == 20, "the sweep was cleared with no delay set");
}

// ---------------------------------------------------------- the V-blank tick ---

#define DMA1_CNT REG_OFFSET_DMA1CNT
#define DMA2_CNT (REG_OFFSET_DMA1CNT + 0x0C)

static u32 io32(int offset)
{
    return *(volatile u32 *)(agb_mem.io + offset);
}

static void reset_vsync(const char *name, int counter, int period)
{
    TEST_CASE(name);
    memset(&agb_mem, 0, sizeof(agb_mem));
    memset(&sound_header, 0, sizeof(sound_header));
    sound_header.ident = ID_NUMBER;
    sound_header.pcmDmaCounter = (u8)counter;
    sound_header.pcmDmaPeriod = (u8)period;
    *(struct SoundInfo **)(agb_mem.iwram + 0x7FF0) = &sound_header;
}

// Most frames just count down.
static void test_vsync_counts_down(void)
{
    reset_vsync("vsync counts down", 4, 4);
    m4aSoundVSync();
    CHECK(sound_header.pcmDmaCounter == 3, "the counter is %u, not 3",
          sound_header.pcmDmaCounter);
    CHECK(io32(DMA1_CNT) == 0, "a channel was re-armed on an ordinary frame");
}

// On the frame the counter runs out, the period is reloaded and both FIFO
// channels are re-armed.
static void test_vsync_reloads(void)
{
    reset_vsync("vsync reloads and re-arms", 1, 7);
    m4aSoundVSync();

    CHECK(sound_header.pcmDmaCounter == 7, "the counter reloaded to %u, not 7",
          sound_header.pcmDmaCounter);
    CHECK((io32(DMA1_CNT) >> 16) == (u32)(DMA_ENABLE | DMA_START_SPECIAL | DMA_32BIT | DMA_REPEAT),
          "channel 1 was not left in FIFO mode, control is %04X",
          (unsigned)(io32(DMA1_CNT) >> 16));
    CHECK((io32(DMA2_CNT) >> 16) == (u32)(DMA_ENABLE | DMA_START_SPECIAL | DMA_32BIT | DMA_REPEAT),
          "channel 2 was not left in FIFO mode");
}

// A counter of zero falls through as well: the original decrements the byte and
// branches on the signed result.
static void test_vsync_zero_counter(void)
{
    reset_vsync("vsync with a counter of zero", 0, 5);
    m4aSoundVSync();
    CHECK(sound_header.pcmDmaCounter == 5, "a counter of zero did not reload, it is %u",
          sound_header.pcmDmaCounter);
}

// A channel part-way through a repeat is kicked with an immediate transfer
// before being re-armed.
static void test_vsync_restarts_a_repeating_channel(void)
{
    reset_vsync("vsync kicks a repeating channel", 1, 4);
    // Leave channel 1 mid-repeat and channel 2 idle.
    *(volatile u32 *)(agb_mem.io + DMA1_CNT) = (u32)DMA_REPEAT << 16;
    m4aSoundVSync();
    // The kick writes a four-word count, which the re-arm does not overwrite.
    CHECK((io32(DMA1_CNT) & 0xFFFF) == 4, "the kicked channel has a count of %u, not 4",
          (unsigned)(io32(DMA1_CNT) & 0xFFFF));
    CHECK((io32(DMA2_CNT) & 0xFFFF) == 0, "the idle channel was kicked as well");

    // And the same the other way round, so neither channel is being handled by
    // accident through the other.
    reset_vsync("vsync kicks the second channel too", 1, 4);
    *(volatile u32 *)(agb_mem.io + DMA2_CNT) = (u32)DMA_REPEAT << 16;
    m4aSoundVSync();
    CHECK((io32(DMA2_CNT) & 0xFFFF) == 4, "channel 2 was not kicked, its count is %u",
          (unsigned)(io32(DMA2_CNT) & 0xFFFF));
    CHECK((io32(DMA1_CNT) & 0xFFFF) == 0, "channel 1 was kicked as well");
}

// The header is only ours to touch while its ident is at rest or one past it.
static void test_vsync_ident_guard(void)
{
    reset_vsync("vsync accepts a locked header", 1, 4);
    sound_header.ident = ID_NUMBER + 1;
    m4aSoundVSync();
    CHECK(sound_header.pcmDmaCounter == 4, "a locked header was refused");

    reset_vsync("vsync refuses a foreign header", 1, 4);
    sound_header.ident = ID_NUMBER + 2;
    m4aSoundVSync();
    CHECK(sound_header.pcmDmaCounter == 1, "a header two past its ident was touched");

    reset_vsync("vsync refuses an uninitialised header", 1, 4);
    sound_header.ident = 0;
    m4aSoundVSync();
    CHECK(sound_header.pcmDmaCounter == 1, "an uninitialised header was touched");
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
    test_jump_table_copy();
    test_track_stop();
    test_chn_vol_set();
    test_endtie();
    test_note_operands();
    test_note_operands_stop_early();
    test_note_gate_time_maximum();
    test_note_plain_instrument();
    test_note_key_split();
    test_note_rhythm();
    test_note_nested_redirect_is_dropped();
    test_note_priority();
    test_note_takes_an_idle_channel();
    test_note_steals_lowest_priority();
    test_note_prefers_a_releasing_channel();
    test_note_releasing_channels_compete();
    test_note_ties_break_on_track();
    test_note_will_not_steal_from_below();
    test_note_dropped_when_nothing_is_stealable();
    test_note_cgb_channel_choice();
    test_note_cgb_stealing();
    test_note_key_shift();
    test_note_cgb_sweep();
    test_note_chains_and_delay();
    test_vsync_counts_down();
    test_vsync_reloads();
    test_vsync_zero_counter();
    test_vsync_restarts_a_repeating_channel();
    test_vsync_ident_guard();

    return test_report("m4a track opcodes");
}
