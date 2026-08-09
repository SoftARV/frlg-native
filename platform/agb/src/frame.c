#include <math.h>
#include <setjmp.h>
#include <stdio.h>

#include "agb/frame.h"
#include "agb/memmap.h"

static jmp_buf agb_exit_point;
static uint32_t agb_frames;
static uint32_t agb_frame_limit;
static int agb_running;

uint32_t agb_frame_count(void)
{
    return agb_frames;
}

#define REG_OFF_KEYINPUT 0x130
#define KEYS_RELEASED 0x03FF

static void agb_reset_io(void)
{
    // Keys are active-low: a zeroed KEYINPUT reads as every button held, which
    // the game sees as the soft-reset combo before it draws a single frame.
    *(volatile uint16_t *)(agb_mem.io + REG_OFF_KEYINPUT) = KEYS_RELEASED;
}

uint32_t agb_frame_run(void (*entry)(void), uint32_t max_frames)
{
    agb_frames = 0;
    agb_frame_limit = max_frames;
    agb_running = 1;

    agb_reset_io();

    if (setjmp(agb_exit_point) == 0)
    {
        entry();
        // AgbMain never returns on hardware; if it does, the port is wrong.
        fprintf(stderr, "agb: AgbMain returned after %u frames\n", agb_frames);
    }

    agb_running = 0;
    return agb_frames;
}

void agb_wait_vblank(void)
{
    if (!agb_running)
        return;

    agb_frames++;
    if (agb_frame_limit && agb_frames >= agb_frame_limit)
        longjmp(agb_exit_point, 1);
}

void agb_soft_reset(uint32_t flags)
{
    (void)flags;
    fprintf(stderr, "agb: soft reset at frame %u\n", agb_frames);
    longjmp(agb_exit_point, 2);
}

// Unverified against hardware; the BIOS uses a table and its rounding differs.
// Replaced by tested vectors in phase 3.
int16_t agb_sin_lut(uint16_t angle)
{
    return (int16_t)lrint(sin((double)angle * 2.0 * M_PI / 65536.0) * 16384.0);
}
