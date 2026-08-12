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
#include "agb/audio.h"
#include "agb/cart.h"
#include "agb/flash.h"
#include "agb/memmap.h"
#include "agb/ppu.h"
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

// The PPU composes into its own buffer on the game thread; the main thread
// copies it out to present. A torn copy costs one frame of tearing and never
// blocks the game, which is the right trade for a display path.
static void copy_frame(void)
{
    memcpy(framebuffer, agb_ppu_framebuffer(),
           sizeof(uint32_t) * (size_t)(SCREEN_W * SCREEN_H));
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

// Called from the game thread every frame with the buffer the mixer has just
// filled. The device opens on the first frame and reopens if the game changes
// the mixer's rate, which m4aSoundMode can do at any time.
// What the mixer produced, whether or not anything is playing it. Audio leaves
// no trace if it is wrong -- a mixer that runs but produces nothing looks exactly
// like a host that is not listening -- so this is measured on every run and
// reported at the end. It is the only way a silent mixer fails a test.
static FILE *audio_dump;
static uint32_t audio_frames;
static uint32_t audio_loud_frames;
static int audio_peak;

static void audio_sink(const int8_t *right, const int8_t *left, int samples, int rate)
{
    static int opened_rate;
    bool loud = false;

    // Measured before the device is considered, so a build with no audio output
    // at all still reports what the mixer did.
    audio_frames++;
    for (int i = 0; i < samples; i++)
    {
        int a = right[i] < 0 ? -right[i] : right[i];
        int b = left[i] < 0 ? -left[i] : left[i];

        if (a > audio_peak)
            audio_peak = a;
        if (b > audio_peak)
            audio_peak = b;
        if (a != 0 || b != 0)
            loud = true;
    }
    if (loud)
        audio_loud_frames++;

    if (rate != opened_rate)
    {
        opened_rate = host_audio_open(rate) ? rate : 0;
        if (opened_rate != 0)
            printf("frlg-native: audio at %d Hz\n", opened_rate);
    }

    // FRLG_PCM dumps what the mixer produced, in the same raw signed 16-bit
    // stereo mgba-audio writes, so the two can be compared directly. Audio is
    // the one subsystem a screenshot cannot check.
    if (audio_dump != NULL)
    {
        for (int i = 0; i < samples; i++)
        {
            short pair[2] = {(short)(left[i] * 256), (short)(right[i] * 256)};

            fwrite(pair, sizeof(pair), 1, audio_dump);
        }
    }

    if (opened_rate != 0)
        host_audio_submit(right, left, samples);
}

// Input traces.
//
// FRLG_INPUT_RECORD writes what the keyboard did, a line per frame on which it
// changed; FRLG_INPUT replays one. A replayed run is deterministic because the
// frame driver asks for the keys on the game's own thread at the same point in
// every frame, rather than the presenting thread writing the register whenever
// it gets round to it.
//
// The format is a frame number and a key mask, active-low like the register, so
// a trace can be read, diffed and hand-edited.
#define TRACE_MAX 65536

static struct
{
    uint32_t frame;
    uint16_t keys;
} trace[TRACE_MAX];

static unsigned trace_count;
static unsigned trace_pos;
static uint16_t trace_keys = HOST_KEYS_RELEASED;
static FILE *trace_out;
static uint16_t trace_last_written = HOST_KEYS_RELEASED;

// Called by the frame driver, on the game thread, once a frame.
static uint16_t replay_keys(uint32_t frame)
{
    while (trace_pos < trace_count && trace[trace_pos].frame <= frame)
        trace_keys = trace[trace_pos++].keys;

    return trace_keys;
}

static uint16_t record_keys(uint32_t frame)
{
    uint16_t keys = host_input_keys();

    if (keys != trace_last_written)
    {
        fprintf(trace_out, "%u %04X\n", frame, keys);
        trace_last_written = keys;
    }
    return keys;
}

static void load_trace(void)
{
    const char *replay = getenv("FRLG_INPUT");
    const char *record = getenv("FRLG_INPUT_RECORD");
    FILE *fh;
    char line[128];

    if (record != NULL)
    {
        trace_out = fopen(record, "w");
        if (trace_out == NULL)
        {
            fprintf(stderr, "frlg-native: cannot record to %s\n", record);
            return;
        }
        setvbuf(trace_out, NULL, _IOLBF, 0);
        fprintf(trace_out, "# frame keys (active low)\n");
        printf("frlg-native: recording input to %s\n", record);
        agb_frame_set_key_source(record_keys);
        return;
    }

    if (replay == NULL)
        return;

    fh = fopen(replay, "r");
    if (fh == NULL)
    {
        fprintf(stderr, "frlg-native: cannot read %s\n", replay);
        return;
    }

    while (trace_count < TRACE_MAX && fgets(line, sizeof(line), fh) != NULL)
    {
        unsigned frame, keys;

        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%u %x", &frame, &keys) != 2)
            continue;
        trace[trace_count].frame = frame;
        trace[trace_count].keys = (uint16_t)keys;
        trace_count++;
    }
    fclose(fh);

    printf("frlg-native: replaying %u input events from %s\n", trace_count, replay);
    agb_frame_set_key_source(replay_keys);
}

// The save chip is backed by a file, in the layout emulators use, so a save
// moves between this port, mGBA and a cartridge unchanged. Phase 7 gives it a
// proper home next to the player's own data; until then it sits where the port
// was started, or wherever FRLG_SAV says.
static void load_save(void)
{
    const char *path = getenv("FRLG_SAV");

    if (path == NULL)
        path = "frlg-native.sav";

    if (agb_flash_open(path))
        printf("frlg-native: save file %s\n", path);
    else
        fprintf(stderr, "frlg-native: no save file; nothing will be kept\n");
}

// The game's own data lives in the player's ROM, not in this binary
// (docs/adr/0006-rom-supplied-data.md). Until the phase 7 importer exists, the
// path comes from the environment, falling back to the build the port is bound
// against -- the two have to describe each other, so this is not a free choice.
static void load_cart(void)
{
    const char *path = getenv("FRLG_ROM");
    int err;

    if (path == NULL)
        path = FRLG_DEFAULT_ROM;

    err = agb_cart_load(path);
    if (err == 0)
    {
        printf("frlg-native: cart loaded from %s\n", path);
        return;
    }

    // Not fatal: the game runs without it, badly. Everything read out of
    // data/*.s reads as zeros, which is its own kind of wrong -- see
    // docs/spikes/0003-empty-cart-region.md.
    fprintf(stderr, "frlg-native: no cart image (%s): %s\n", path,
            err == -2 ? "wrong size" : "cannot read");
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

    load_cart();
    load_save();
    load_trace();

    {
        const char *dump = getenv("FRLG_PCM");

        if (dump != NULL)
            audio_dump = fopen(dump, "wb");
    }
    agb_m4a_set_audio_sink(audio_sink);

    // A capture has to be reproducible to be worth comparing, and a wall-clock
    // frame timer is not: a frame whose work overruns the tick misses a V-blank
    // and the run slips one frame behind a less loaded one. FRLG_LOCKSTEP drives
    // frames from the game's own idle point instead. Not for playing -- there is
    // nothing left pacing the game to real time.
    {
        const char *lockstep = getenv("FRLG_LOCKSTEP");

        if (lockstep != NULL)
        {
            int paced = strcmp(lockstep, "pace") == 0;

            agb_frame_set_lockstep(1);
            agb_frame_set_pace(paced);
            printf("frlg-native: lockstep clock%s\n",
                   paced ? ", paced to real time" : ", frames advance with the game");
        }
    }

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

        // Left to the frame driver when a trace is driving the keys, so that
        // the two do not write the register against each other.
        if (trace_out == NULL && trace_count == 0)
            *(volatile uint16_t *)(agb_mem.io + REG_OFF_KEYINPUT) = host_input_keys();

        copy_frame();
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
        copy_frame();
        write_ppm(shot);
    }

    if (audio_dump != NULL)
        fclose(audio_dump);
    agb_flash_close();
    if (trace_out != NULL)
        fclose(trace_out);
    host_audio_close();
    host_video_close();

    printf("frlg-native: ran %u frames\n", frames_ran);
    if (agb_frame_watchdog_ticks() != 0)
        printf("frlg-native: %u frames advanced by the stall watchdog\n",
               agb_frame_watchdog_ticks());
    printf("frlg-native: audio %u frames, %u non-silent, peak %d\n",
           audio_frames, audio_loud_frames, audio_peak);
    return 0;
}
