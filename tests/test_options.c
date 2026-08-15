// Per-save port options: that they survive a round trip, that they are tied to
// the save they were set against, and that a run with no save still works
// rather than refusing to.

#include <stdio.h>
#include <string.h>

#include "host_options.h"

#include "harness.h"

#ifndef FRLG_TEST_OPTIONS_SAVE
#define FRLG_TEST_OPTIONS_SAVE "test_options.sav"
#endif

#define OTHER_SAVE FRLG_TEST_OPTIONS_SAVE ".other"

static void scrub(const char *save)
{
    char path[512];

    snprintf(path, sizeof(path), "%s.port.ini", save);
    remove(path);
}

static void test_round_trip(void)
{
    TEST_CASE("a value survives being written and read back");
    scrub(FRLG_TEST_OPTIONS_SAVE);

    host_options_open(FRLG_TEST_OPTIONS_SAVE);
    CHECK(host_option_get("lcd-grid", 7) == 7, "an unset key ignored its fallback");

    host_option_set("lcd-grid", 1);
    host_option_set("colour-mode", 2);
    CHECK(host_options_flush() == 0, "flush failed");

    host_options_open(FRLG_TEST_OPTIONS_SAVE);
    CHECK(host_option_get("lcd-grid", 0) == 1, "lcd-grid came back as %d",
          host_option_get("lcd-grid", 0));
    CHECK(host_option_get("colour-mode", 0) == 2, "colour-mode came back as %d",
          host_option_get("colour-mode", 0));
}

static void test_per_save(void)
{
    TEST_CASE("options belong to the save they were set against");
    scrub(FRLG_TEST_OPTIONS_SAVE);
    scrub(OTHER_SAVE);

    host_options_open(FRLG_TEST_OPTIONS_SAVE);
    host_option_set("lcd-grid", 1);
    host_options_flush();

    host_options_open(OTHER_SAVE);
    CHECK(host_option_get("lcd-grid", 0) == 0,
          "one save's option leaked into another");

    host_option_set("lcd-grid", 2);
    host_options_flush();

    host_options_open(FRLG_TEST_OPTIONS_SAVE);
    CHECK(host_option_get("lcd-grid", 0) == 1,
          "the first save's option was overwritten by the second");
}

static void test_no_save_is_not_an_error(void)
{
    TEST_CASE("no save: reads fall back, writes are held, flush says so");
    host_options_open(NULL);

    CHECK(host_option_get("lcd-grid", 3) == 3, "fallback was not returned");
    host_option_set("lcd-grid", 1);
    CHECK(host_option_get("lcd-grid", 0) == 1,
          "a write was lost when there was nowhere to flush it");
    CHECK(host_options_flush() == -1,
          "flush claimed to have written with no path");
}

static void test_reopen_forgets(void)
{
    TEST_CASE("opening a different save does not carry the old values along");
    scrub(FRLG_TEST_OPTIONS_SAVE);
    scrub(OTHER_SAVE);

    host_options_open(FRLG_TEST_OPTIONS_SAVE);
    host_option_set("only-here", 5);
    host_options_flush();

    host_options_open(OTHER_SAVE);
    CHECK(host_option_get("only-here", 0) == 0,
          "a key survived a change of save");
}

static void test_unchanged_does_not_write(void)
{
    TEST_CASE("flushing without a change writes nothing and succeeds");
    scrub(FRLG_TEST_OPTIONS_SAVE);

    host_options_open(FRLG_TEST_OPTIONS_SAVE);
    CHECK(host_options_flush() == 0, "a no-op flush reported failure");

    host_options_open(FRLG_TEST_OPTIONS_SAVE);
    host_option_set("grid", 1);
    host_options_flush();

    // Setting the same value again is not a change.
    host_options_open(FRLG_TEST_OPTIONS_SAVE);
    host_option_set("grid", 1);
    CHECK(host_options_flush() == 0, "re-setting the same value failed");
    CHECK(host_option_get("grid", 0) == 1, "the value was lost");
}

int main(void)
{
    test_round_trip();
    test_per_save();
    test_no_save_is_not_an_error();
    test_reopen_forgets();
    test_unchanged_does_not_write();
    scrub(FRLG_TEST_OPTIONS_SAVE);
    scrub(OTHER_SAVE);
    return test_report("options");
}
