#ifndef GUARD_TESTS_HARNESS_H
#define GUARD_TESTS_HARNESS_H

#include <stdio.h>

// The PPU raises the scanline interrupts, so it reaches irq.c, which dispatches
// through the game's own handler table. Tests link the agb archive without the
// game, so they supply the table themselves -- and it is what lets a test
// install a handler and watch the PPU call it.
typedef void (*IntrFunc)(void);
extern IntrFunc gIntrTable[14];

// The slots are ordered by the BIOS vector's scan priority, not by IE bit.
#define INTR_SLOT_VCOUNT 0
#define INTR_SLOT_HBLANK 3
#define INTR_SLOT_VBLANK 4

extern int test_failures;
extern const char *test_case_name;

#define TEST_CASE(name) (test_case_name = (name))

#define CHECK(cond, ...)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            printf("FAIL %s:%d [%s] ", __FILE__, __LINE__, test_case_name); \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
            test_failures++;                                                \
        }                                                                   \
    } while (0)

int test_report(const char *suite);

#endif // GUARD_TESTS_HARNESS_H
