// Frame driver.
//
// The game's main loop ends in a busy-wait on a flag that only the V-blank
// handler sets, so the interrupt has to *preempt* game code. A cooperative
// switch cannot escape that loop, which is why this is signal-driven rather
// than fiber-driven -- see docs/adr/0009-preemptive-interrupts.md.
//
// A signal handler runs on the interrupted context's own stack, which is
// exactly how a hardware interrupt behaves: game code is paused, not run
// concurrently. That is what keeps this free of the data races a thread would
// introduce.

#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "agb/frame.h"
#include "agb/irq.h"
#include "agb/memmap.h"

#define REG_OFF_KEYINPUT 0x130
#define KEYS_RELEASED 0x03FF

// The GBA's true refresh rate, not 60.
#define FRAME_PERIOD_US 16743

static sigjmp_buf agb_exit_point;
static volatile sig_atomic_t agb_frames;
static uint32_t agb_frame_limit;
static volatile sig_atomic_t agb_running;
static volatile sig_atomic_t agb_in_irq;

uint32_t agb_frame_count(void)
{
    return (uint32_t)agb_frames;
}

static void agb_reset_io(void)
{
    // Keys are active-low: a zeroed KEYINPUT reads as every button held, which
    // the game sees as the soft-reset combo before it draws a single frame.
    *(volatile uint16_t *)(agb_mem.io + REG_OFF_KEYINPUT) = KEYS_RELEASED;
}

static void agb_on_vblank(int sig)
{
    (void)sig;

    if (!agb_running || agb_in_irq)
        return;

    agb_frames++;

    if (agb_frame_limit && (uint32_t)agb_frames >= agb_frame_limit)
        siglongjmp(agb_exit_point, 1);

    agb_in_irq = 1;
    agb_irq_raise(AGB_IRQ_VBLANK);
    agb_in_irq = 0;
}

static void agb_timer_set(int on)
{
    struct itimerval it;

    memset(&it, 0, sizeof(it));
    if (on)
    {
        it.it_interval.tv_usec = FRAME_PERIOD_US;
        it.it_value.tv_usec = FRAME_PERIOD_US;
    }
    setitimer(ITIMER_REAL, &it, NULL);
}

uint32_t agb_frame_run(void (*entry)(void), uint32_t max_frames)
{
    struct sigaction sa;

    agb_frames = 0;
    agb_frame_limit = max_frames;
    agb_in_irq = 0;
    agb_reset_io();

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = agb_on_vblank;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);

    agb_running = 1;

    if (sigsetjmp(agb_exit_point, 1) == 0)
    {
        agb_timer_set(1);
        entry();
        fprintf(stderr, "agb: AgbMain returned after %u frames\n",
                (uint32_t)agb_frames);
    }

    agb_timer_set(0);
    agb_running = 0;
    return (uint32_t)agb_frames;
}

void agb_wait_vblank(void)
{
    sig_atomic_t start = agb_frames;

    if (!agb_running)
        return;

    while (agb_frames == start)
        ;
}

void agb_soft_reset(uint32_t flags)
{
    (void)flags;
    fprintf(stderr, "agb: soft reset at frame %u\n", (uint32_t)agb_frames);
    siglongjmp(agb_exit_point, 2);
}

// Unverified against hardware; the BIOS uses a table and its rounding differs.
// Replaced by tested vectors in phase 3.
int16_t agb_sin_lut(uint16_t angle)
{
    return (int16_t)lrint(sin((double)angle * 2.0 * M_PI / 65536.0) * 16384.0);
}
