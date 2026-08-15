// The screen filter, checked against the two things its design claims: that it
// moves the middle and not the ends, and that it produces the same bytes every
// time it is asked.

#include <string.h>

#include "host_filters.h"
#include "host_render.h"

#include "harness.h"

#define W 4
#define H 2
#define N (W * H)

static uint32_t presented[N];

void host_video_present(const uint32_t *pixels, int width, int height);
void host_log(const char *msg);

void host_video_present(const uint32_t *pixels, int width, int height)
{
    memcpy(presented, pixels, sizeof(uint32_t) * (size_t)(width * height));
}

void host_log(const char *msg)
{
    (void)msg;
}

static uint32_t through_the_filter(uint32_t colour)
{
    uint32_t frame[N];
    int i, id;

    for (i = 0; i < host_render_count(); i++)
        host_render_unregister(i);

    id = host_filter_screen_register();
    host_render_set_enabled(id, true);

    for (i = 0; i < N; i++)
        frame[i] = colour;
    host_render_present(frame, W, H);
    return presented[0];
}

#define R(p) (((p) >> 16) & 0xFF)
#define G(p) (((p) >> 8) & 0xFF)
#define B(p) ((p) & 0xFF)

static void test_registered_off(void)
{
    int id, i;

    TEST_CASE("registers switched off, because vanilla is the default");
    for (i = 0; i < host_render_count(); i++)
        host_render_unregister(i);

    id = host_filter_screen_register();
    CHECK(id >= 0, "the filter did not register");
    CHECK(!host_render_is_enabled(id), "it registered switched on");
    CHECK(host_render_name(id) != NULL, "it registered without a name");
}

static void test_the_ends_do_not_move(void)
{
    uint32_t black = through_the_filter(0xFF000000u);
    uint32_t white;

    TEST_CASE("black stays black");
    CHECK(R(black) == 0 && G(black) == 0 && B(black) == 0,
          "black became %02x%02x%02x", R(black), G(black), B(black));

    white = through_the_filter(0xFFFFFFFFu);
    TEST_CASE("white stays white");
    // The matrix rows sum to one and the gamma fixes both endpoints, so this is
    // exact rather than approximate. If it drifts, one of the two has changed.
    CHECK(R(white) == 255 && G(white) == 255 && B(white) == 255,
          "white became %02x%02x%02x", R(white), G(white), B(white));
}

static void test_the_middle_darkens(void)
{
    uint32_t grey = through_the_filter(0xFF808080u);

    TEST_CASE("a midtone comes back darker");
    CHECK(R(grey) < 0x80 && G(grey) < 0x80 && B(grey) < 0x80,
          "mid grey became %02x%02x%02x, no darker", R(grey), G(grey), B(grey));

    // Neutral in, neutral out: the rows differ, but a grey has equal channels
    // to mix, so nothing should come out tinted.
    CHECK(R(grey) == G(grey) && G(grey) == B(grey),
          "a neutral grey came back tinted: %02x%02x%02x",
          R(grey), G(grey), B(grey));
}

static void test_channels_bleed(void)
{
    uint32_t red = through_the_filter(0xFFFF0000u);

    TEST_CASE("a pure channel picks up its neighbours");
    CHECK(R(red) > 0, "red lost its own channel entirely");
    CHECK(G(red) > 0 || B(red) > 0,
          "pure red stayed pure: %02x%02x%02x -- nothing bled",
          R(red), G(red), B(red));
    CHECK(R(red) > G(red) && R(red) > B(red),
          "red stopped being mostly red: %02x%02x%02x",
          R(red), G(red), B(red));
}

static void test_alpha_is_left_alone(void)
{
    uint32_t out = through_the_filter(0xAB123456u);

    TEST_CASE("the high byte is not the filter's business");
    CHECK((out >> 24) == 0xAB, "the high byte became %02x", (unsigned)(out >> 24));
}

static void test_deterministic(void)
{
    uint32_t first = through_the_filter(0xFF3C6890u);
    uint32_t again = through_the_filter(0xFF3C6890u);

    TEST_CASE("the same pixel gives the same bytes twice");
    CHECK(first == again, "%08x then %08x", first, again);
}

int main(void)
{
    test_registered_off();
    test_the_ends_do_not_move();
    test_the_middle_darkens();
    test_channels_bleed();
    test_alpha_is_left_alone();
    test_deterministic();
    return test_report("filter_screen");
}
