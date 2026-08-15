// The handheld's screen, approximated.
//
// The GBA's LCD was dim, and its subpixels bled into one another. Artists drew
// for that panel, compensating with more contrast and saturation than the
// picture needs on a display that reproduces exactly what it is given. Shown
// unaltered on a modern screen the result is harsher and more garish than
// anyone working on it ever saw.
//
// Two steps, in this order:
//
//   1. **Bleed.** A little of each channel is mixed into its neighbours. The
//      rows of the matrix each sum to one, so black stays black and white stays
//      white -- only what is between them moves. That is what keeps the
//      correction from looking like a tint.
//   2. **Response.** A gamma above one pulls the midtones down, which is the
//      dimness. It also fixes both endpoints, for the same reason.
//
// The coefficients are a first approximation, not a measurement: they are the
// shape of the thing -- some bleed, some dimming -- with numbers that look
// right rather than numbers derived from a panel. Somebody with a GBA and a
// colorimeter could do better, and the arithmetic would not change.
//
// Integer throughout. Not for speed -- 38,400 pixels is nothing -- but because
// a display stage that produced different bytes on different machines would
// make a screenshot mean less than it does now.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "host_filters.h"
#include "host_render.h"

// 8.8 fixed point. Rows sum to 256, which is what pins white in place.
#define ONE 256

static const int32_t mix[3][3] = {
    /* r' */ { 210, 33, 13 },
    /* g' */ { 23, 205, 28 },
    /* b' */ { 23, 33, 200 },
};

// out = 255 * (in/255)^GAMMA, with GAMMA above one so the middle darkens and
// 0 and 255 do not move.
#define GAMMA 1.18

static uint8_t response[256];
static bool ready;

static void build_response(void)
{
    int i;

    for (i = 0; i < 256; i++)
    {
        double v = pow((double)i / 255.0, GAMMA) * 255.0 + 0.5;

        response[i] = (uint8_t)(v > 255.0 ? 255.0 : v);
    }
    ready = true;
}

static uint8_t clamp8(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

static bool run(uint32_t *pixels, int width, int height, void *user)
{
    int32_t count = (int32_t)width * (int32_t)height;
    int32_t i;

    (void)user;

    if (!ready)
        build_response();

    for (i = 0; i < count; i++)
    {
        uint32_t p = pixels[i];
        int32_t r = (int32_t)((p >> 16) & 0xFF);
        int32_t g = (int32_t)((p >> 8) & 0xFF);
        int32_t b = (int32_t)(p & 0xFF);

        int32_t nr = (r * mix[0][0] + g * mix[0][1] + b * mix[0][2]) / ONE;
        int32_t ng = (r * mix[1][0] + g * mix[1][1] + b * mix[1][2]) / ONE;
        int32_t nb = (r * mix[2][0] + g * mix[2][1] + b * mix[2][2]) / ONE;

        pixels[i] = (p & 0xFF000000u)
                    | ((uint32_t)response[clamp8(nr)] << 16)
                    | ((uint32_t)response[clamp8(ng)] << 8)
                    | (uint32_t)response[clamp8(nb)];
    }
    return true;
}

int host_filter_screen_register(void)
{
    struct host_render_stage stage = {0};
    int id;

    stage.name = "handheld screen";
    stage.run = run;

    id = host_render_register(&stage);
    if (id >= 0)
        host_render_set_enabled(id, false);
    return id;
}
