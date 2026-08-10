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
    test_vsync_counts_down();
    test_vsync_reloads();
    test_vsync_zero_counter();
    test_vsync_restarts_a_repeating_channel();
    test_vsync_ident_guard();

    return test_report("m4a track opcodes");
}
