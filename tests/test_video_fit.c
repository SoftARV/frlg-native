#include "harness.h"

#include "host_video_fit.h"

#define NATIVE_W 240
#define NATIVE_H 160

static int near(float a, float b)
{
    float d = a - b;

    return d < 0.05f && d > -0.05f;
}

// The shape is the thing the fit must not change, so it is checked on its own
// rather than inferred from the width and height agreeing with a number.
static void check_shape(struct host_video_fit f, const char *what)
{
    CHECK(near(f.w * (float)NATIVE_H, f.h * (float)NATIVE_W),
          "%s: %gx%g is not the shape of %dx%d", what, (double)f.w, (double)f.h,
          NATIVE_W, NATIVE_H);
}

int main(void)
{
    TEST_CASE("a window of the picture's own shape is filled completely");
    {
        struct host_video_fit f
            = host_video_fit_rect(1200, 800, NATIVE_W, NATIVE_H);

        CHECK(near(f.w, 1200.0f) && near(f.h, 800.0f), "got %gx%g, want 1200x800",
              (double)f.w, (double)f.h);
        CHECK(near(f.x, 0.0f) && near(f.y, 0.0f), "got %g,%g, want no bars",
              (double)f.x, (double)f.y);
        check_shape(f, "exact multiple");
    }

    TEST_CASE("a size between two whole multiples still fills the short axis");
    {
        // 1000x700 is 4.17 of the picture across and 4.375 down: the width is
        // what runs out, so the width is what the picture takes all of.
        struct host_video_fit f
            = host_video_fit_rect(1000, 700, NATIVE_W, NATIVE_H);

        CHECK(near(f.w, 1000.0f), "want the full width, got %g", (double)f.w);
        CHECK(near(f.h, 666.67f), "want 666.67 tall, got %g", (double)f.h);
        CHECK(near(f.x, 0.0f), "want no side bars, got %g", (double)f.x);
        CHECK(near(f.y, 16.67f), "want 16.67 of bar, got %g", (double)f.y);
        check_shape(f, "between multiples");

        // The whole-multiple answer was 4, which is 960x640 and leaves 40 of
        // the window across and 60 down unused. This is the point of the
        // change, so it is the thing asserted.
        CHECK(f.w > 960.0f, "no larger than whole multiples would give: %g",
              (double)f.w);
    }

    TEST_CASE("a wide window bars the sides, not the top");
    {
        // 16:9 is wider than the picture, which is the ordinary case for a
        // maximised window or fullscreen.
        struct host_video_fit f
            = host_video_fit_rect(1920, 1080, NATIVE_W, NATIVE_H);

        CHECK(near(f.h, 1080.0f), "want the full height, got %g", (double)f.h);
        CHECK(near(f.w, 1620.0f), "want 1620 across, got %g", (double)f.w);
        CHECK(near(f.x, 150.0f), "want 150 of bar a side, got %g", (double)f.x);
        CHECK(near(f.y, 0.0f), "want no bar above, got %g", (double)f.y);
        check_shape(f, "16:9");
    }

    TEST_CASE("a tall window bars the top and bottom");
    {
        struct host_video_fit f
            = host_video_fit_rect(1000, 1000, NATIVE_W, NATIVE_H);

        CHECK(near(f.w, 1000.0f), "want the full width, got %g", (double)f.w);
        CHECK(near(f.h, 666.67f), "want 666.67 tall, got %g", (double)f.h);
        CHECK(near(f.y, 166.67f), "want 166.67 of bar, got %g", (double)f.y);
        check_shape(f, "square window");
    }

    TEST_CASE("a window smaller than the picture shrinks it rather than clipping");
    {
        struct host_video_fit f = host_video_fit_rect(120, 80, NATIVE_W, NATIVE_H);

        CHECK(near(f.w, 120.0f) && near(f.h, 80.0f), "got %gx%g, want 120x80",
              (double)f.w, (double)f.h);
        check_shape(f, "half size");
    }

    TEST_CASE("the picture is centred, so the bars are the same on both sides");
    {
        struct host_video_fit f
            = host_video_fit_rect(1913, 1077, NATIVE_W, NATIVE_H);

        CHECK(near(f.x * 2.0f + f.w, 1913.0f), "%g + %g + %g is not 1913",
              (double)f.x, (double)f.w, (double)f.x);
        CHECK(near(f.y * 2.0f + f.h, 1077.0f), "%g + %g + %g is not 1077",
              (double)f.y, (double)f.h, (double)f.y);
        check_shape(f, "odd window");
    }

    TEST_CASE("no area anywhere means no rectangle");
    {
        struct host_video_fit z[] = {
            host_video_fit_rect(0, 800, NATIVE_W, NATIVE_H),
            host_video_fit_rect(1200, 0, NATIVE_W, NATIVE_H),
            host_video_fit_rect(1200, 800, 0, NATIVE_H),
            host_video_fit_rect(1200, 800, NATIVE_W, 0),
            host_video_fit_rect(-1200, -800, NATIVE_W, NATIVE_H),
        };
        int i;

        for (i = 0; i < (int)(sizeof(z) / sizeof(z[0])); i++)
            CHECK(z[i].w == 0.0f && z[i].h == 0.0f && z[i].x == 0.0f
                      && z[i].y == 0.0f,
                  "case %d gave %g,%g %gx%g", i, (double)z[i].x, (double)z[i].y,
                  (double)z[i].w, (double)z[i].h);
    }

    TEST_CASE("a viewport that is not the native one keeps its own shape");
    {
        // FRLG_VIEW composes at other sizes, and the fit is told the source
        // rather than assuming it.
        struct host_video_fit f = host_video_fit_rect(1000, 1000, 256, 256);

        CHECK(near(f.w, 1000.0f) && near(f.h, 1000.0f), "got %gx%g, want square",
              (double)f.w, (double)f.h);
    }

    return test_report("video_fit");
}
