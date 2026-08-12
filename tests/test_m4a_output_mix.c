// The output stage the two direct-sound buffers pass through.
//
// The mixer produces two buffers, and they are not a finished stereo pair: on
// hardware each feeds one of the two direct-sound FIFOs, and SOUNDCNT_H says at
// what volume each FIFO reaches each side. The game changes that register when
// the sound option is applied, so a port that ignores it is right during the
// intro and wrong from the first menu onwards.

#include <string.h>

#include "agb/m4a.h"
#include "agb/memmap.h"

#include "harness.h"

#define SOUNDCNT_H 0x82

#define A_MIX_FULL 0x0004
#define B_MIX_FULL 0x0008
#define A_RIGHT 0x0100
#define A_LEFT 0x0200
#define B_RIGHT 0x1000
#define B_LEFT 0x2000

// What m4aSoundInit leaves behind: both at full, hard panned.
#define INIT_STATE (A_MIX_FULL | B_MIX_FULL | A_RIGHT | B_LEFT | 0x0002)
// What applying the sound option leaves behind, measured from the reference at
// the same frame: both FIFOs on both sides at half, PSG at full.
#define MONO_STATE 0x3302

static void set_mixing(uint16_t value)
{
    *(volatile uint16_t *)(agb_mem.io + SOUNDCNT_H) = value;
}

static void reset(const char *name)
{
    TEST_CASE(name);
    memset(&agb_mem, 0, sizeof(agb_mem));
}

static void test_full_and_panned_passes_through(void)
{
    s8 a[4] = {100, -100, 50, 0};
    s8 b[4] = {20, 30, -40, 7};

    reset("full volume, hard panned, is what it was");
    set_mixing(INIT_STATE);
    agb_m4a_apply_output_mix(a, b, 4);

    CHECK(a[0] == 100 && a[1] == -100 && a[2] == 50 && a[3] == 0,
          "the right side changed: %d %d %d %d", a[0], a[1], a[2], a[3]);
    CHECK(b[0] == 20 && b[1] == 30 && b[2] == -40 && b[3] == 7,
          "the left side changed: %d %d %d %d", b[0], b[1], b[2], b[3]);
}

// The state the game actually plays in. Both halves reach both sides at half
// volume, which is a mono downmix at unity gain -- and half the level the port
// used to hand over.
static void test_mono_halves_and_sums(void)
{
    s8 a[3] = {100, -60, 0};
    s8 b[3] = {40, -20, 80};

    reset("mono puts both halves on both sides, halved");
    set_mixing(MONO_STATE);
    agb_m4a_apply_output_mix(a, b, 3);

    CHECK(a[0] == 50 + 20, "right[0] should be (100 + 40)/2, got %d", a[0]);
    CHECK(a[1] == -30 + -10, "right[1] should be (-60 + -20)/2, got %d", a[1]);
    CHECK(a[2] == 0 + 40, "right[2] should be (0 + 80)/2, got %d", a[2]);
    CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2],
          "the two sides differ in mono: %d/%d %d/%d %d/%d",
          a[0], b[0], a[1], b[1], a[2], b[2]);
}

// The same music, at half volume, is the whole point: the PSG channels keep
// their own ratio, so getting this wrong moves the balance between them.
static void test_half_is_half(void)
{
    s8 a[2] = {80, -80};
    s8 b[2] = {0, 0};

    reset("half volume halves one source");
    set_mixing(B_MIX_FULL | A_RIGHT | B_LEFT | 0x0002);
    agb_m4a_apply_output_mix(a, b, 2);

    CHECK(a[0] == 40 && a[1] == -40, "A was not halved: %d %d", a[0], a[1]);
}

static void test_a_side_with_no_route_is_silent(void)
{
    s8 a[2] = {120, -120};
    s8 b[2] = {10, 20};

    reset("a side nothing is routed to is silent");
    set_mixing(A_MIX_FULL | B_MIX_FULL | A_RIGHT | 0x0002);
    agb_m4a_apply_output_mix(a, b, 2);

    CHECK(a[0] == 120 && a[1] == -120, "the routed side changed: %d %d", a[0], a[1]);
    CHECK(b[0] == 0 && b[1] == 0, "the unrouted side is not silent: %d %d", b[0], b[1]);
}

// Two sources at full volume on one side can exceed what a byte holds, and the
// hardware's mixer saturates rather than wrapping.
static void test_the_sum_saturates(void)
{
    s8 a[2] = {120, -120};
    s8 b[2] = {100, -100};

    reset("a sum past the ends is clamped, not wrapped");
    set_mixing(A_MIX_FULL | B_MIX_FULL | A_RIGHT | B_RIGHT | 0x0002);
    agb_m4a_apply_output_mix(a, b, 2);

    CHECK(a[0] == 127, "expected 127, got %d", a[0]);
    CHECK(a[1] == -128, "expected -128, got %d", a[1]);
}

int main(void)
{
    test_full_and_panned_passes_through();
    test_mono_halves_and_sums();
    test_half_is_half();
    test_a_side_with_no_route_is_silent();
    test_the_sum_saturates();

    return test_report("m4a output mix");
}
