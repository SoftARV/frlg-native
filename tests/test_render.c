// The display-pipeline seam: that a stage runs, that a failing one is retired
// without taking the frame with it, and that a stage which is merely
// unavailable this frame is not punished for it.
//
// render.c is compiled into this test rather than linked from the host archive,
// so the two functions it calls downward -- present and log -- can be observed.

#include <string.h>

#include "host_render.h"

#include "harness.h"

#define W 4
#define H 2
#define N (W * H)

static uint32_t presented[N];
static int present_calls;
static bool presented_is_source;
static const uint32_t *source_ptr;
static char last_log[128];

void host_video_present(const uint32_t *pixels, int width, int height);
void host_log(const char *msg);

void host_video_present(const uint32_t *pixels, int width, int height)
{
    present_calls++;
    presented_is_source = (pixels == source_ptr);
    memcpy(presented, pixels, sizeof(uint32_t) * (size_t)(width * height));
}

void host_log(const char *msg)
{
    snprintf(last_log, sizeof(last_log), "%s", msg);
}

// A stage that adds one to every pixel, so its effect is visible and its order
// among several stages is countable.
static bool add_one(uint32_t *pixels, int width, int height, void *user)
{
    int i;

    (void)user;
    for (i = 0; i < width * height; i++)
        pixels[i] += 1;
    return true;
}

// Writes, then fails. The write is the point: a stage that gives up halfway
// must not leave half its work on the screen.
static bool scribble_then_fail(uint32_t *pixels, int width, int height,
                               void *user)
{
    int i;

    (void)user;
    for (i = 0; i < width * height; i++)
        pixels[i] = 0xDEADBEEF;
    return false;
}

static bool never_available(void *user)
{
    (void)user;
    return false;
}

static void reset(void)
{
    int i;

    for (i = 0; i < host_render_count(); i++)
        host_render_unregister(i);
    present_calls = 0;
    memset(presented, 0, sizeof(presented));
    last_log[0] = '\0';
}

static void fill(uint32_t *buf, uint32_t value)
{
    int i;

    for (i = 0; i < N; i++)
        buf[i] = value;
}

static void test_no_stages_presents_the_source(void)
{
    uint32_t frame[N];

    TEST_CASE("no stages: the caller's own buffer is presented");
    reset();
    fill(frame, 7);
    source_ptr = frame;

    host_render_present(frame, W, H);

    CHECK(present_calls == 1, "presented %d times", present_calls);
    CHECK(presented_is_source,
          "a copy was made when there was nothing to run");
    CHECK(presented[0] == 7, "pixel changed with no stage registered");
}

static void test_a_stage_runs(void)
{
    uint32_t frame[N];
    int id;

    TEST_CASE("a registered stage transforms the frame");
    reset();
    fill(frame, 7);
    source_ptr = frame;

    id = host_render_register(&(struct host_render_stage){
        .name = "add-one", .run = add_one});
    CHECK(id >= 0, "register returned %d", id);

    host_render_present(frame, W, H);

    CHECK(presented[0] == 8, "pixel is %u, wanted 8", presented[0]);
    CHECK(!presented_is_source, "the source buffer was written through");
    CHECK(frame[0] == 7, "the caller's buffer was modified: %u", frame[0]);
}

static void test_stages_compose(void)
{
    uint32_t frame[N];

    TEST_CASE("two stages both run");
    reset();
    fill(frame, 0);
    source_ptr = frame;

    host_render_register(&(struct host_render_stage){
        .name = "one", .run = add_one});
    host_render_register(&(struct host_render_stage){
        .name = "two", .run = add_one});

    host_render_present(frame, W, H);

    CHECK(presented[0] == 2, "pixel is %u, wanted 2", presented[0]);
}

static void test_failure_retires_only_itself(void)
{
    uint32_t frame[N];
    int bad, good;

    TEST_CASE("a failing stage is retired and its work discarded");
    reset();
    fill(frame, 5);
    source_ptr = frame;

    bad = host_render_register(&(struct host_render_stage){
        .name = "broken", .run = scribble_then_fail});
    good = host_render_register(&(struct host_render_stage){
        .name = "add-one", .run = add_one});

    host_render_present(frame, W, H);

    CHECK(presented[0] == 6,
          "pixel is %#x, wanted 6 -- the failed stage's writes survived",
          presented[0]);
    CHECK(host_render_is_retired(bad), "the failing stage was not retired");
    CHECK(!host_render_is_retired(good), "a working stage was retired too");
    CHECK(strstr(last_log, "broken") != NULL,
          "the failure was not attributed: '%s'", last_log);

    TEST_CASE("a retired stage stays out on the next frame");
    fill(frame, 5);
    host_render_present(frame, W, H);
    CHECK(presented[0] == 6, "pixel is %#x on the second frame", presented[0]);

    TEST_CASE("a retired stage cannot be switched back on from a menu");
    host_render_set_enabled(bad, true);
    CHECK(!host_render_is_enabled(bad), "a retired stage was re-enabled");
}

static void test_unavailable_is_not_retired(void)
{
    uint32_t frame[N];
    int id;

    TEST_CASE("unavailable this frame: skipped, not retired");
    reset();
    fill(frame, 3);
    source_ptr = frame;

    id = host_render_register(&(struct host_render_stage){
        .name = "gpu-only", .run = add_one, .available = never_available});

    host_render_present(frame, W, H);

    CHECK(presented[0] == 3, "the stage ran anyway: %u", presented[0]);
    CHECK(presented_is_source, "a copy was made for a stage that never ran");
    CHECK(!host_render_is_retired(id), "an unavailable stage was retired");
    CHECK(host_render_is_enabled(id), "an unavailable stage was disabled");
}

static void test_disabled_is_skipped(void)
{
    uint32_t frame[N];
    int id;

    TEST_CASE("a disabled stage does not run");
    reset();
    fill(frame, 1);
    source_ptr = frame;

    id = host_render_register(&(struct host_render_stage){
        .name = "add-one", .run = add_one});
    host_render_set_enabled(id, false);

    host_render_present(frame, W, H);

    CHECK(presented[0] == 1, "a disabled stage ran: %u", presented[0]);
    CHECK(host_render_name(id) != NULL,
          "a disabled stage vanished from the listing");
}

static void test_table_is_bounded(void)
{
    int i, id;

    TEST_CASE("registering past the table is refused, not overflowed");
    reset();
    for (i = 0; i < host_render_count(); i++)
    {
        id = host_render_register(&(struct host_render_stage){
            .name = "filler", .run = add_one});
        CHECK(id == i, "slot %d came back as %d", i, id);
    }
    id = host_render_register(&(struct host_render_stage){
        .name = "one too many", .run = add_one});
    CHECK(id == -1, "a full table accepted another stage: %d", id);

    TEST_CASE("a stage without a run function is refused");
    reset();
    CHECK(host_render_register(&(struct host_render_stage){
              .name = "no callback"}) == -1,
          "a stage with no run function was accepted");
}

int main(void)
{
    test_no_stages_presents_the_source();
    test_a_stage_runs();
    test_stages_compose();
    test_failure_retires_only_itself();
    test_unavailable_is_not_retired();
    test_disabled_is_skipped();
    test_table_is_bounded();
    return test_report("render");
}
