// Desktop entry point.
//
// SDL owns the main thread because macOS and iOS require windowing there, and
// the game runs on its own thread with SIGALRM routed to it -- the frame driver
// delivers V-blank by preempting game code (docs/adr/0009-preemptive-interrupts.md).
//
// The main thread is the hardware side: it pumps events, writes the key
// register and presents the framebuffer. It never runs game code, so it races
// with nothing; a real GBA updates its key register asynchronously too.

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <execinfo.h>
#endif

#include "agb/frame.h"
#include "agb/memmap.h"
#include "host.h"

#define SCREEN_W 240
#define SCREEN_H 160
#define REG_OFF_KEYINPUT 0x130

extern void AgbMain(void);

static volatile sig_atomic_t game_done;
static uint32_t frames_ran;
static uint32_t frame_limit;
static uint32_t framebuffer[SCREEN_W * SCREEN_H];

static void dump_backtrace(int sig)
{
    const char msg[] = "\nfrlg-native: game thread backtrace\n";
    ssize_t ignored = write(2, msg, sizeof(msg) - 1);
    (void)ignored;

#if defined(__GLIBC__)
    void *frames[24];
    int n = backtrace(frames, 24);
    backtrace_symbols_fd(frames, n, 2);
#endif
    if (sig != SIGUSR1)
        _exit(3);
}

static void *game_thread(void *arg)
{
    sigset_t allow;

    (void)arg;

    // A new thread inherits the creator's signal mask, and the main thread
    // blocked SIGALRM to keep the frame timer off itself. Without this the
    // signal is blocked everywhere and no frame ever advances.
    sigemptyset(&allow);
    sigaddset(&allow, SIGALRM);
    pthread_sigmask(SIG_UNBLOCK, &allow, NULL);

    signal(SIGUSR1, dump_backtrace);
    signal(SIGSEGV, dump_backtrace);
    signal(SIGBUS, dump_backtrace);

    frames_ran = agb_frame_run(AgbMain, frame_limit);
    game_done = 1;
    return NULL;
}

// The backdrop is palette entry 0, in BGR555. Until the PPU exists this is the
// only real pixel data the game produces, and it proves palette memory is live:
// AgbMain writes it before anything else.
static void fill_backdrop(void)
{
    uint16_t bgr = *(const volatile uint16_t *)agb_mem.pltt;
    uint32_t r = (bgr & 0x1F) << 3;
    uint32_t g = ((bgr >> 5) & 0x1F) << 3;
    uint32_t b = ((bgr >> 10) & 0x1F) << 3;
    uint32_t argb;

    r |= r >> 5;
    g |= g >> 5;
    b |= b >> 5;
    argb = (r << 16) | (g << 8) | b;

    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        framebuffer[i] = argb;
}

// Framebuffer capture, seeded here because the phase 3 golden-image harness
// needs exactly this. PPM keeps it dependency-free.
static void write_ppm(const char *path)
{
    FILE *fh = fopen(path, "wb");

    if (!fh)
    {
        perror("frlg-native: capture");
        return;
    }

    fprintf(fh, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
    {
        uint32_t p = framebuffer[i];
        unsigned char rgb[3] = {(p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF};
        fwrite(rgb, 1, 3, fh);
    }
    fclose(fh);
    fprintf(stderr, "frlg-native: wrote %s\n", path);
}

int main(int argc, char **argv)
{
    unsigned stall_secs = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 10;
    pthread_t game;
    sigset_t block;
    uint32_t last_frame = 0;
    unsigned stalled_ms = 0;

    frame_limit = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0;
    setvbuf(stdout, NULL, _IOLBF, 0);

    // Block the frame timer before anything else starts a thread. ITIMER_REAL
    // is delivered to any thread that has not blocked it, and threads inherit
    // the mask in force when they are created -- so blocking after SDL_Init
    // leaves SDL's own backend threads eligible, and the V-blank handler then
    // runs game code on one of them, concurrently with the game thread.
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &block, NULL);

    if (!host_video_open("frlg-native", SCREEN_W, SCREEN_H, 3))
        return 1;

    printf("frlg-native: starting, frame limit %u\n", frame_limit);
    if (pthread_create(&game, NULL, game_thread, NULL) != 0)
    {
        host_log("cannot start game thread");
        return 1;
    }

    while (!game_done)
    {
        struct timespec tick = {0, 4 * 1000 * 1000};
        uint32_t now = agb_frame_count();

        if (!host_pump_events())
            break;

        *(volatile uint16_t *)(agb_mem.io + REG_OFF_KEYINPUT) = host_input_keys();

        fill_backdrop();
        host_video_present(framebuffer, SCREEN_W, SCREEN_H);

        // Stall detection by frame progress rather than CPU time: it means the
        // same thing windowed or headless, and it can point at the stuck thread.
        if (now != last_frame)
        {
            last_frame = now;
            stalled_ms = 0;
        }
        else if (stall_secs && (stalled_ms += 4) > stall_secs * 1000)
        {
            fprintf(stderr, "frlg-native: no frame for %us at frame %u\n",
                    stall_secs, now);
            pthread_kill(game, SIGUSR1);
            nanosleep(&(struct timespec){0, 200 * 1000 * 1000}, NULL);
            _exit(3);
        }

        nanosleep(&tick, NULL);
    }

    pthread_join(game, NULL);

    const char *shot = getenv("FRLG_SHOT");
    if (shot)
    {
        fill_backdrop();
        write_ppm(shot);
    }

    host_video_close();

    printf("frlg-native: ran %u frames\n", frames_ran);
    return 0;
}
