// Shared state for the test suites, and the interrupt table they lend the PPU.
//
// The table belongs to the game, which the tests do not link. Supplying it here
// is what lets a test install a handler and watch the PPU call it per scanline.

#include "harness.h"

IntrFunc gIntrTable[14];

int test_failures;
const char *test_case_name = "";

int test_report(const char *suite)
{
    printf("%s: %s (%d failures)\n", suite, test_failures ? "FAILED" : "ok", test_failures);
    return test_failures != 0;
}
