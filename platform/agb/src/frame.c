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

#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "agb/dma.h"
#include "agb/frame.h"
#include "agb/irq.h"
#include "agb/ppu.h"
#include "agb/memmap.h"

#define REG_OFF_KEYINPUT 0x130
#define REG_OFF_VCOUNT 0x006
#define SCREEN_LINES 160
#define KEYS_RELEASED 0x03FF

// The GBA's true refresh rate, not 60.
#define FRAME_PERIOD_US 16743

static sigjmp_buf agb_exit_point;
static volatile sig_atomic_t agb_frames;
static uint32_t agb_frame_limit;
static volatile sig_atomic_t agb_running;
static volatile sig_atomic_t agb_in_irq;
static int agb_lockstep;
static int agb_paced;
static volatile sig_atomic_t agb_stopping;
static volatile sig_atomic_t agb_watchdog_ticks;

// How long the watchdog gives the game to reach its idle point before advancing
// a frame itself. Three frame periods is far longer than any frame's work takes
// on a host CPU, and short enough that a spin costs a barely visible pause.
#define WATCHDOG_PERIODS 3

// The game's main loop never returns, so a port that wants to shut down has to
// ask: the next frame boundary leaves through the same non-local jump the frame
// limit uses. Without it, closing the window left the game thread running and the
// join behind it never finished, which is a hang the desktop offers to kill.
void agb_frame_stop(void)
{
    agb_stopping = 1;
}

// A fault in game code leaves the port with a live process and a dead game. The
// handler that catches it can do almost nothing safely -- so it does almost
// nothing, and leaves through the same non-local jump the frame limit and the
// stop request use. What comes out the other side is an ordinary return from
// agb_frame_run, on a stack that was never damaged, where a port can write a
// report and tell somebody.
//
// Nothing resumes afterwards: the game's own state is whatever the fault left.
// The jump is to end the run, not to survive it.
void agb_frame_abort(void)
{
    if (agb_running)
        siglongjmp(agb_exit_point, 4);
}

void agb_frame_set_lockstep(int on)
{
    agb_lockstep = on;
}

void agb_frame_set_pace(int on)
{
    agb_paced = on;
}

// Lockstep runs as fast as the host will go, which is right for a capture and
// unplayable for a person. Pacing waits out what is left of the frame period
// after the work, so the game keeps time without the frame boundary depending on
// how long the work took -- which is the property a recorded trace needs.
static void agb_pace_frame(void)
{
    static struct timespec due;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (due.tv_sec == 0 || now.tv_sec - due.tv_sec > 1)
        due = now;

    due.tv_nsec += FRAME_PERIOD_US * 1000L;
    if (due.tv_nsec >= 1000000000L)
    {
        due.tv_nsec -= 1000000000L;
        due.tv_sec++;
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL) == EINTR)
        ;
}

uint32_t agb_frame_count(void)
{
    return (uint32_t)agb_frames;
}

static void agb_reset_io(void)
{
    // Keys are active-low: a zeroed KEYINPUT reads as every button held, which
    // the game sees as the soft-reset combo before it draws a single frame.
    *(volatile uint16_t *)(agb_mem.io + REG_OFF_KEYINPUT) = KEYS_RELEASED;

    agb_io_affine_identity();
}

static uint16_t (*agb_key_source)(uint32_t frame);

void agb_frame_set_key_source(uint16_t (*source)(uint32_t frame))
{
    agb_key_source = source;
}

// Lockstep advances frames from the game's idle point, but not every wait the
// game does is that point: DoMapLoadLoop runs its state machine to completion
// with a bare spin, and one of its steps waits for the DMA3 manager, which only
// drains in the V-blank handler. On hardware the interrupt preempts the spin.
// Rather than find and rewrite every such loop -- there are a dozen candidates
// and only some can block -- the timer stays armed as a watchdog and advances
// the frame the game is waiting for. See ADR 0014.
//
// It costs nothing in determinism where it matters: a loop that spins is a loop
// doing nothing, so it does not matter when the frame lands. Firings are counted
// and reported, because one during real work would mean otherwise.
static void agb_watchdog_arm(void)
{
    struct itimerval it;

    memset(&it, 0, sizeof(it));
    it.it_value.tv_usec = FRAME_PERIOD_US * WATCHDOG_PERIODS;
    setitimer(ITIMER_REAL, &it, NULL);
}

uint32_t agb_frame_watchdog_ticks(void)
{
    return (uint32_t)agb_watchdog_ticks;
}

static void agb_frame_advance(void)
{
    agb_frames++;

    if (agb_frame_limit && (uint32_t)agb_frames >= agb_frame_limit)
        siglongjmp(agb_exit_point, 1);

    if (agb_stopping)
        siglongjmp(agb_exit_point, 3);

    agb_in_irq = 1;

    // Before the handler, so the keys the game samples this frame are the ones
    // the trace names for it, whatever the presenting thread is doing.
    if (agb_key_source != NULL)
        *(volatile uint16_t *)(agb_mem.io + REG_OFF_KEYINPUT) =
            agb_key_source((uint32_t)agb_frames);

    // VCOUNT reads inside the handler must see the V-blank period; the game
    // samples it around the sound mixer.
    *(volatile uint16_t *)(agb_mem.io + REG_OFF_VCOUNT) = SCREEN_LINES;

    // Same order as H-blank: the channels waiting for V-blank run before the
    // handler that may re-arm them.
    agb_dma_trigger(AGB_DMA_START_VBLANK);

    agb_irq_raise(AGB_IRQ_VBLANK);

    // Composed after the handler, so the register writes and DMA copies it
    // just performed are on screen this frame rather than the next.
    agb_ppu_render_frame();

    *(volatile uint16_t *)(agb_mem.io + REG_OFF_VCOUNT) = 0;
    agb_in_irq = 0;

    if (agb_lockstep)
        agb_watchdog_arm();
}

static void agb_on_vblank(int sig)
{
    (void)sig;

    if (!agb_running || agb_in_irq)
        return;

    if (agb_lockstep)
        agb_watchdog_ticks++;

    agb_frame_advance();
}

// The game's main loop ends in a spin on a flag only the V-blank handler sets,
// which is why a real-time run needs a signal to preempt it (ADR 0009). In
// lockstep that spin is the one place the game is provably doing nothing, so the
// frame can be advanced from inside it instead of from a timer -- and a run then
// depends on nothing but the game's own state. See ADR 0013.
void agb_frame_idle(void)
{
    if (!agb_lockstep || !agb_running || agb_in_irq)
        return;

    if (agb_paced)
        agb_pace_frame();
    agb_frame_advance();
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
    agb_stopping = 0;
    agb_reset_io();

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = agb_on_vblank;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);

    agb_running = 1;

    if (sigsetjmp(agb_exit_point, 1) == 0)
    {
        // In lockstep the timer is a watchdog rather than the clock: it is
        // re-armed a few frame periods out after every frame, so it only fires
        // where the game is spinning somewhere that never reaches the idle hook.
        if (agb_lockstep)
            agb_watchdog_arm();
        else
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

    if (agb_lockstep)
    {
        agb_frame_idle();
        return;
    }

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
