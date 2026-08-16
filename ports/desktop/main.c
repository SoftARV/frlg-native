// Desktop entry point.
//
// SDL owns the main thread because macOS and iOS require windowing there, and
// the game runs on its own thread with SIGALRM routed to it -- the frame driver
// delivers V-blank by preempting game code (docs/adr/0009-preemptive-interrupts.md).
//
// The main thread is the hardware side: it pumps events, writes the key
// register and presents the framebuffer. It never runs game code, so it races
// with nothing; a real GBA updates its key register asynchronously too.

// pthread_timedjoin_np is a GNU extension, and this port is the desktop one:
// OS-specific code is allowed here and nowhere below it.
#define _GNU_SOURCE

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
#include "host_filters.h"
#include "host_options.h"
#include "host_render.h"

// platform/game/port_view.c: whether the field is what is on screen.
bool agb_port_field_active(void);
#include "host_session.h"

#include "crash.h"

// Where a report goes. The template asks for exactly what the bundle holds.
#define FRLG_ISSUE_URL "https://github.com/SoftARV/frlg-native/issues/new?template=crash-or-glitch.yml"

// The viewport, not the hardware: what the renderer is currently composing.
// The buffer is sized for the largest it can be asked for.
#define SCREEN_W agb_ppu_width()
#define SCREEN_H agb_ppu_height()
#define REG_OFF_KEYINPUT 0x130

extern void AgbMain(void);

static volatile sig_atomic_t game_done;
static uint32_t frames_ran;
static uint32_t frame_limit;
static uint32_t framebuffer[AGB_PPU_MAX_W * AGB_PPU_MAX_H];

// SIGUSR1 asks a running game where it is, which is how a hang is inspected
// without stopping it. Faults are caught in crash.c, which has to do rather
// more than print.
static void dump_backtrace(int sig)
{
    const char msg[] = "\nfrlg-native: game thread backtrace\n";
    ssize_t ignored = write(2, msg, sizeof(msg) - 1);

    (void)sig;
    (void)ignored;

#if defined(__GLIBC__)
    void *frames[24];
    int n = backtrace(frames, 24);
    backtrace_symbols_fd(frames, n, 2);
#endif
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
    crash_install();

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

// FRLG_SHOT_RANGE=FIRST:LAST:STEP:DIR writes every frame in a range, which is
// how an animation is inspected: one run instead of one run per frame.
static unsigned shot_first, shot_last, shot_step = 1;
static const char *shot_dir;

// FRLG_SHOT_FIELD=1 skips every frame that is not the overworld. Sampling a
// recording at a fixed step lands on menus, fades and the quest log more often
// than on the field, and a comparison of two builds that agree on a menu says
// nothing about the field. Four separate investigations were read off frames
// that turned out to be the quest log before this existed.
static void shot_range(uint32_t frame)
{
    char path[512];

    if (!shot_dir || frame < shot_first || frame > shot_last)
        return;
    if (shot_step > 1 && (frame - shot_first) % shot_step != 0)
        return;
    if (getenv("FRLG_SHOT_FIELD") != NULL && !agb_port_field_active())
        return;
    copy_frame();
    snprintf(path, sizeof(path), "%s/f%u.ppm", shot_dir, frame);
    write_ppm(path);
}

// Called by the frame driver, on the game thread, once a frame.
static uint16_t replay_keys(uint32_t frame)
{
    while (trace_pos < trace_count && trace[trace_pos].frame <= frame)
        trace_keys = trace[trace_pos++].keys;

    shot_range(frame);
    crash_test_tick(frame);
    return trace_keys;
}

static uint16_t record_keys(uint32_t frame)
{
    uint16_t keys = host_input_keys();

    crash_test_tick(frame);

    if (keys != trace_last_written)
    {
        fprintf(trace_out, "%u %04X\n", frame, keys);
        trace_last_written = keys;
    }
    return keys;
}

static char session_trace[512];

static void load_trace(void)
{
    const char *replay = getenv("FRLG_INPUT");
    const char *record = getenv("FRLG_INPUT_RECORD");
    FILE *fh;
    char line[128];

    // Replay wins: a run driven by a trace is reproducing something rather than
    // producing something, and recording its own inputs back would only produce
    // a copy of the file it is reading.
    if (replay == NULL && record == NULL)
        record = host_session_file("input.trace", session_trace, sizeof(session_trace));

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

    // Before the game can touch it. A trace only replays against the state its
    // run began with, and playing changes that state -- so the copy has to be
    // taken now or it is not the right copy. This is the step a tester is asked
    // to remember by hand today, and the one they forget.
    host_session_keep(path, "start.sav");

    // Port options belong to the save, not the install: a display mode chosen
    // while playing one file is not chosen for another. They live beside the
    // save rather than in it, because SaveBlock2 is the cartridge's.
    host_options_open(path);
    {
        const char *want = getenv("FRLG_SCREEN_FILTER");
        int id = host_filter_screen_register();

        // The environment still wins, as everywhere else: a recorded run must
        // not depend on what somebody had switched on.
        host_render_set_enabled(id, want != NULL
                                    ? atoi(want) != 0
                                    : host_option_get("screen-filter", 0) != 0);

        // The rest of what the port's own options screen writes. Applied here
        // because the save is what they belong to, and this is where it is
        // known -- before that there is no profile to have chosen anything.
        {
            int zoom = host_option_get("zoom", 0);

            if (zoom > 0)
                host_video_set_zoom(zoom);
            if (host_option_get("fullscreen", 0))
                host_video_set_fullscreen(true);
        }
    }

    if (agb_flash_open(path))
        printf("frlg-native: save file %s\n", path);
    else
        fprintf(stderr, "frlg-native: no save file; nothing will be kept\n");
}

// The game's own data lives in the player's ROM, not in this binary
// (docs/adr/0006-rom-supplied-data.md). Until the phase 7 importer exists, the
// path comes from the environment, falling back to the build the port is bound
// against -- the two have to describe each other, so this is not a free choice.
// Hashes the decompilation records for the revisions it can build. Names, not
// game data -- they exist so a rejection can say what the player actually
// handed over instead of only that it was wrong.
static const struct
{
    const char *sha1;
    const char *name;
} known_roms[] = {
    {"41cb23d8dccc8ebd7c649cd8fbb58eeace6e2fdc", "Pokemon FireRed (rev 0)"},
    {"dd5945db9b930750cb39d00c84da8571feebf417", "Pokemon FireRed (rev 1)"},
    {"baa452d0b24629dd7782cfc07a8984085dde1311", "Pokemon FireRed (Switch)"},
    {"574fa542ffebb14be69902d1d36f1ec0a4afd71e", "Pokemon LeafGreen (rev 0)"},
    {"7862c67bdecbe21d1d69ce082ce34327e1c6ed5e", "Pokemon LeafGreen (rev 1)"},
    {"62b9fc77549dbc67032eb6cbd0ea6ad3b825690f", "Pokemon LeafGreen (Switch)"},
};

static const char *name_of_rom(const char *sha1)
{
    for (unsigned i = 0; i < sizeof(known_roms) / sizeof(known_roms[0]); i++)
        if (strcmp(known_roms[i].sha1, sha1) == 0)
            return known_roms[i].name;
    return NULL;
}

// Where this install keeps the imported game. Keyed by what the manifest
// describes, because a build for another title or revision is another game and
// must not read this one's image.
static const char *cache_path(char *buf, size_t len)
{
    char root[512];

    if (host_data_dir(root, sizeof(root)) != 0)
        return NULL;
    snprintf(buf, len, "%s/cache", root);
    host_make_dir(buf);
    snprintf(buf, len, "%s/cache/%.8s.cart", root, FRLG_ROM_SHA1);
    return buf;
}

// Import is a first-boot step, not a per-launch one: the relocated image is
// kept, so the player's ROM is needed once and never again. It is not a copy of
// their ROM -- every pointer in it has been rewritten to this build's own
// addresses, so it will not run anywhere else (ADR 0006).
// The launcher asks the game about itself rather than being told twice: one
// binary is one title, and it is the only thing that knows which ROM its
// manifest describes or whether that ROM has been imported yet. Plain
// key=value, which a shell and a Vala GSubprocess read equally well.
static int command_describe(void)
{
    char cache[512];
    const char *cached = cache_path(cache, sizeof(cache));

    printf("title=%s\n", FRLG_ROM_TITLE);
    printf("sha1=%s\n", FRLG_ROM_SHA1);
    printf("cache=%s\n", cached != NULL ? cached : "");
    printf("imported=%s\n",
           (cached != NULL && agb_cart_cache_load(cached, FRLG_ROM_SHA1) == 0) ? "yes" : "no");
    return 0;
}

// Import without playing, so a launcher can do it with a progress spinner up
// and know whether it worked before it offers to start anything.
static int command_import(const char *path)
{
    char cache[512];
    char saw[AGB_SHA1_TEXT];
    const char *cached = cache_path(cache, sizeof(cache));
    int err = agb_cart_import(path, FRLG_ROM_SHA1, saw);

    if (err != AGB_CART_OK)
    {
        const char *name = err == AGB_CART_WRONG_GAME ? name_of_rom(saw) : NULL;

        printf("ok=no\n");
        if (err == AGB_CART_WRONG_SIZE)
            printf("error=that file is not the right size for a Game Boy Advance ROM\n");
        else if (err == AGB_CART_UNREADABLE)
            printf("error=that file could not be read\n");
        else if (name != NULL)
            printf("error=that file is %s; this needs %s\n", name, FRLG_ROM_TITLE);
        else
            printf("error=that file is not %s\n", FRLG_ROM_TITLE);
        printf("sha1=%s\n", saw);
        return 3;
    }

    if (cached == NULL || agb_cart_cache_save(cached, FRLG_ROM_SHA1) != 0)
    {
        printf("ok=no\nerror=the imported game could not be saved\n");
        return 4;
    }

    printf("ok=yes\ntitle=%s\ncache=%s\n", FRLG_ROM_TITLE, cached);
    return 0;
}

// What a save says about itself, for a launcher listing them.
//
// The offsets are mirrored from the game's own headers rather than reached
// through them: pulling global.h into port code would drag the game's whole
// include world across a layer it is not supposed to cross. Same arrangement as
// agb/m4a.h, and the same risk -- if upstream moves these, this reports
// nonsense rather than failing, so they are cited precisely.
//
//   vendor/pokefirered/include/save.h        the sector layout
//   vendor/pokefirered/include/global.h:327  struct SaveBlock2
//   vendor/pokefirered/include/global.h:759  struct SaveBlock1
//   vendor/pokefirered/include/constants/flags.h:1324  SYS_FLAGS = 0x800
#define SAVE_SECTOR_DATA 3968
#define SAVE_SECTOR_SIZE 4096
#define SAVE_SECTOR_COUNT 32
#define SAVE_SIGNATURE 0x08012025u

#define SB2_PLAYER_NAME 0x000
#define SB2_PLAY_HOURS 0x00E
#define SB2_PLAY_MINUTES 0x010
#define SB1_FLAGS 0xEE0
#define FLAG_BADGE01 (0x800 + 0x20)

// The game keeps two save slots and rotates the sectors inside them, so the
// copy to read is the one with the highest counter, per sector id.
static int newest_sector(const uint8_t *save, unsigned id, const uint8_t **data)
{
    uint32_t best_counter = 0;
    int found = 0;

    for (unsigned i = 0; i < SAVE_SECTOR_COUNT; i++)
    {
        const uint8_t *sector = save + (size_t)i * SAVE_SECTOR_SIZE;
        uint16_t sector_id;
        uint16_t stored_sum;
        uint32_t signature, counter, sum = 0;

        memcpy(&sector_id, sector + SAVE_SECTOR_SIZE - 12, sizeof(sector_id));
        memcpy(&stored_sum, sector + SAVE_SECTOR_SIZE - 10, sizeof(stored_sum));
        memcpy(&signature, sector + SAVE_SECTOR_SIZE - 8, sizeof(signature));
        memcpy(&counter, sector + SAVE_SECTOR_SIZE - 4, sizeof(counter));

        if (signature != SAVE_SIGNATURE || sector_id != id)
            continue;
        if (found && counter <= best_counter)
            continue;

        // The game's own checksum: 32-bit words summed, folded to 16 bits.
        // Reading past a corrupt sector would report a plausible wrong answer,
        // which is worse than reporting nothing.
        for (unsigned w = 0; w < SAVE_SECTOR_DATA / 4; w++)
        {
            uint32_t word;

            memcpy(&word, sector + w * 4, sizeof(word));
            sum += word;
        }
        if ((uint16_t)((sum >> 16) + sum) != stored_sum)
            continue;

        best_counter = counter;
        *data = sector;
        found = 1;
    }
    return found;
}

// The game's own text encoding, not ASCII: a name is stored in it and reads as
// nonsense if handed over raw. Only the run of characters a name can contain is
// translated, from vendor/pokefirered/charmap.txt -- space at 00, digits from
// A1, capitals from BB, lower case from D5, and FF ending the string.
#define PLAYER_NAME_MAX 7

static void decode_name(const uint8_t *encoded, char *out)
{
    unsigned n = 0;

    for (; n < PLAYER_NAME_MAX; n++)
    {
        uint8_t c = encoded[n];

        if (c == 0xFF)
            break;
        if (c == 0x00)
            out[n] = ' ';
        else if (c >= 0xA1 && c <= 0xAA)
            out[n] = (char)('0' + (c - 0xA1));
        else if (c >= 0xBB && c <= 0xD4)
            out[n] = (char)('A' + (c - 0xBB));
        else if (c >= 0xD5 && c <= 0xEE)
            out[n] = (char)('a' + (c - 0xD5));
        else
            out[n] = '?';   // A name can hold characters this does not cover.
    }
    out[n] = '\0';
}

// The seven the game itself keeps, packed into two bytes of SaveBlock2:
//
//   0x013  u8  optionsButtonMode
//   0x014  u16 textSpeed:3, windowFrame:5, sound:1, battleStyle:1,
//              battleSceneOff:1, regionMapZoom:1
//
// Bit positions rather than a struct, for the reason the offsets above are
// mirrored rather than included -- and little-endian, LSB first, which is what
// both the ROM build and this one do.
#define SB2_BUTTON_MODE 0x013
#define SB2_OPTION_BITS 0x014

struct option_field
{
    const char *name;
    unsigned shift;       // in the 16-bit word; ignored when byte_at is set
    unsigned width;
    unsigned byte_at;     // non-zero when the option is a byte of its own
    const char *const *words;
};

static const char *const speed_words[] = {"slow", "mid", "fast", NULL};
static const char *const sound_words[] = {"mono", "stereo", NULL};
static const char *const style_words[] = {"shift", "set", NULL};
static const char *const scene_words[] = {"on", "off", NULL};
static const char *const button_words[] = {"help", "lr", "l-equals-a", NULL};
static const char *const zoom_words[] = {"out", "in", NULL};

static const struct option_field options[] = {
    {"text-speed",   0, 3, 0,               speed_words},
    {"frame",        3, 5, 0,               NULL},
    {"sound",        8, 1, 0,               sound_words},
    {"battle-style", 9, 1, 0,               style_words},
    {"battle-scene", 10, 1, 0,              scene_words},
    {"map-zoom",     11, 1, 0,              zoom_words},
    {"button-mode",  0, 8, SB2_BUTTON_MODE, button_words},
};

#define OPTION_COUNT (sizeof(options) / sizeof(options[0]))

static unsigned option_read(const uint8_t *sb2, const struct option_field *f)
{
    uint16_t word;

    if (f->byte_at != 0)
        return sb2[f->byte_at];
    memcpy(&word, sb2 + SB2_OPTION_BITS, sizeof(word));
    return (word >> f->shift) & ((1u << f->width) - 1u);
}

static int command_options(const char *path)
{
    static uint8_t save[SAVE_SECTOR_COUNT * SAVE_SECTOR_SIZE];
    const uint8_t *sb2 = NULL;
    FILE *fh = fopen(path, "rb");
    size_t read;

    if (fh == NULL)
    {
        printf("ok=no\nerror=no save yet\n");
        return 3;
    }
    read = fread(save, 1, sizeof(save), fh);
    fclose(fh);
    if (read != sizeof(save) || !newest_sector(save, 0, &sb2))
    {
        printf("ok=no\nerror=nothing saved yet\n");
        return 3;
    }

    printf("ok=yes\n");
    for (unsigned i = 0; i < OPTION_COUNT; i++)
    {
        unsigned value = option_read(sb2, &options[i]);

        if (options[i].words != NULL && options[i].words[value] != NULL)
            printf("%s=%s\n", options[i].name, options[i].words[value]);
        else
            printf("%s=%u\n", options[i].name, value);
    }
    return 0;
}

// Writing one option back.
//
// Only the newest copy of SaveBlock2 is touched: the game reads the slot with
// the highest counter, and the older slot is the backup that exists precisely so
// that a bad write is survivable. Its checksum is recomputed, since a sector the
// game cannot verify is a sector it discards -- which would silently undo this
// and, worse, look like the save going bad on its own.
static int command_set_option(const char *path, const char *assignment)
{
    static uint8_t save[SAVE_SECTOR_COUNT * SAVE_SECTOR_SIZE];
    const uint8_t *found = NULL;
    uint8_t *sb2;
    const struct option_field *field = NULL;
    const char *equals = strchr(assignment, '=');
    char name[32];
    unsigned value = 0;
    FILE *fh;
    size_t read;
    char temp[600];

    if (equals == NULL || (size_t)(equals - assignment) >= sizeof(name))
    {
        printf("ok=no\nerror=expected name=value\n");
        return 2;
    }
    memcpy(name, assignment, (size_t)(equals - assignment));
    name[equals - assignment] = '\0';

    for (unsigned i = 0; i < OPTION_COUNT; i++)
        if (strcmp(options[i].name, name) == 0)
            field = &options[i];
    if (field == NULL)
    {
        printf("ok=no\nerror=no such option\n");
        return 2;
    }

    {
        const char *want = equals + 1;
        int matched = 0;

        if (field->words != NULL)
            for (unsigned i = 0; field->words[i] != NULL; i++)
                if (strcmp(field->words[i], want) == 0)
                {
                    value = i;
                    matched = 1;
                }
        if (!matched)
        {
            char *end;
            unsigned long parsed = strtoul(want, &end, 10);

            if (*want == '\0' || *end != '\0' || parsed >= (1u << field->width))
            {
                printf("ok=no\nerror=not a value this option takes\n");
                return 2;
            }
            value = (unsigned)parsed;
        }
    }

    fh = fopen(path, "rb");
    if (fh == NULL)
    {
        printf("ok=no\nerror=no save yet\n");
        return 3;
    }
    read = fread(save, 1, sizeof(save), fh);
    fclose(fh);
    if (read != sizeof(save) || !newest_sector(save, 0, &found))
    {
        printf("ok=no\nerror=nothing saved yet\n");
        return 3;
    }

    sb2 = save + (found - save);
    if (field->byte_at != 0)
    {
        sb2[field->byte_at] = (uint8_t)value;
    }
    else
    {
        uint16_t word;
        uint16_t mask = (uint16_t)(((1u << field->width) - 1u) << field->shift);

        memcpy(&word, sb2 + SB2_OPTION_BITS, sizeof(word));
        word = (uint16_t)((word & ~mask) | ((value << field->shift) & mask));
        memcpy(sb2 + SB2_OPTION_BITS, &word, sizeof(word));
    }

    {
        uint32_t sum = 0;
        uint16_t folded;

        for (unsigned w = 0; w < SAVE_SECTOR_DATA / 4; w++)
        {
            uint32_t block;

            memcpy(&block, sb2 + w * 4, sizeof(block));
            sum += block;
        }
        folded = (uint16_t)((sum >> 16) + sum);
        memcpy(sb2 + SAVE_SECTOR_SIZE - 10, &folded, sizeof(folded));
    }

    // Written beside it and renamed over it: a save half-written is a save
    // lost, and this is the player's game.
    snprintf(temp, sizeof(temp), "%s.new", path);
    fh = fopen(temp, "wb");
    if (fh == NULL || fwrite(save, 1, sizeof(save), fh) != sizeof(save))
    {
        if (fh != NULL)
            fclose(fh);
        remove(temp);
        printf("ok=no\nerror=the save could not be written\n");
        return 4;
    }
    fflush(fh);
    fclose(fh);
    if (rename(temp, path) != 0)
    {
        remove(temp);
        printf("ok=no\nerror=the save could not be replaced\n");
        return 4;
    }

    printf("ok=yes\n%s\n", assignment);
    return 0;
}

static int command_save_info(const char *path)
{
    static uint8_t save[SAVE_SECTOR_COUNT * SAVE_SECTOR_SIZE];
    static uint8_t block1[4 * SAVE_SECTOR_DATA];
    const uint8_t *sb2 = NULL;
    FILE *fh = fopen(path, "rb");
    size_t read;
    unsigned badges = 0;
    int have_block1 = 1;

    if (fh == NULL)
    {
        printf("ok=no\nerror=no save yet\n");
        return 3;
    }
    read = fread(save, 1, sizeof(save), fh);
    fclose(fh);
    if (read != sizeof(save))
    {
        printf("ok=no\nerror=that file is not a save for this game\n");
        return 3;
    }

    if (!newest_sector(save, 0, &sb2))
    {
        // An empty save file is what a profile has before its first save, which
        // is a state to report rather than an error to complain about.
        printf("ok=no\nerror=nothing saved yet\n");
        return 3;
    }

    for (unsigned i = 0; i < 4; i++)
    {
        const uint8_t *sector = NULL;

        if (!newest_sector(save, 1 + i, &sector))
        {
            have_block1 = 0;
            break;
        }
        memcpy(block1 + (size_t)i * SAVE_SECTOR_DATA, sector, SAVE_SECTOR_DATA);
    }

    if (have_block1)
        for (unsigned i = 0; i < 8; i++)
        {
            unsigned flag = FLAG_BADGE01 + i;

            if (block1[SB1_FLAGS + flag / 8] & (1u << (flag % 8)))
                badges++;
        }

    {
        uint16_t hours;
        char name[PLAYER_NAME_MAX + 1];

        memcpy(&hours, sb2 + SB2_PLAY_HOURS, sizeof(hours));
        decode_name(sb2 + SB2_PLAYER_NAME, name);

        printf("ok=yes\n");
        printf("name=%s\n", name);
        printf("hours=%u\n", hours);
        printf("minutes=%u\n", sb2[SB2_PLAY_MINUTES]);
        if (have_block1)
            printf("badges=%u\n", badges);
    }
    return 0;
}

// The other half of --import, and mostly a testing tool: importing is a
// first-boot path, and a first boot is otherwise something you get one of.
// Removing what was imported does not touch the player's ROM or their saves.
static int command_forget(void)
{
    char cache[512];
    const char *cached = cache_path(cache, sizeof(cache));

    if (cached == NULL)
    {
        printf("ok=no\nerror=there is nowhere to keep an imported game\n");
        return 4;
    }

    // Removing something that is not there is the outcome asked for, so it is
    // success rather than an error to report.
    if (remove(cached) == 0)
        printf("ok=yes\nremoved=%s\n", cached);
    else
        printf("ok=yes\nremoved=\n");
    return 0;
}

static void load_cart(void)
{
    const char *path = getenv("FRLG_ROM");
    char cache[512];
    char saw[AGB_SHA1_TEXT];
    const char *cached = cache_path(cache, sizeof(cache));
    int err;

    if (cached != NULL && agb_cart_cache_load(cached, FRLG_ROM_SHA1) == 0)
    {
        printf("frlg-native: %s, from the imported copy\n", FRLG_ROM_TITLE);
        return;
    }

    if (path == NULL)
        path = FRLG_DEFAULT_ROM;

    err = agb_cart_import(path, FRLG_ROM_SHA1, saw);
    if (err == AGB_CART_OK)
    {
        printf("frlg-native: imported %s from %s\n", FRLG_ROM_TITLE, path);
        if (cached != NULL && agb_cart_cache_save(cached, FRLG_ROM_SHA1) == 0)
            printf("frlg-native: kept as %s; the ROM is not needed again\n", cached);
        return;
    }

    // Not fatal: the game runs without it, badly. Everything read out of
    // data/*.s reads as zeros, which is its own kind of wrong -- see
    // docs/spikes/0003-empty-cart-region.md.
    if (err == AGB_CART_WRONG_GAME)
    {
        const char *name = name_of_rom(saw);

        if (name != NULL)
            fprintf(stderr, "frlg-native: that file is %s; this build needs %s\n",
                    name, FRLG_ROM_TITLE);
        else
            fprintf(stderr, "frlg-native: %s is not a supported ROM (%s)\n", path, saw);
    }
    else
    {
        fprintf(stderr, "frlg-native: no cart image (%s): %s\n", path,
                err == AGB_CART_WRONG_SIZE ? "wrong size" : "cannot read");
    }
}

int main(int argc, char **argv)
{
    unsigned stall_secs = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 10;
    pthread_t game;
    sigset_t block;
    uint32_t last_frame = 0;
    unsigned stalled_ms = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);

    // Answered before anything opens a window, a device or a session: these are
    // questions about the install, not a run of the game.
    if (argc > 1 && strcmp(argv[1], "--describe") == 0)
        return command_describe();
    if (argc > 3 && strcmp(argv[1], "--set-option") == 0)
        return command_set_option(argv[2], argv[3]);
    if (argc > 2 && strcmp(argv[1], "--options") == 0)
        return command_options(argv[2]);
    if (argc > 2 && strcmp(argv[1], "--save-info") == 0)
        return command_save_info(argv[2]);
    if (argc > 1 && strcmp(argv[1], "--forget") == 0)
        return command_forget();
    if (argc > 1 && strcmp(argv[1], "--import") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "usage: %s --import <rom>\n", argv[0]);
            return 2;
        }
        return command_import(argv[2]);
    }

    frame_limit = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0;

    // Block the frame timer before anything else starts a thread. ITIMER_REAL
    // is delivered to any thread that has not blocked it, and threads inherit
    // the mask in force when they are created -- so blocking after SDL_Init
    // leaves SDL's own backend threads eligible, and the V-blank handler then
    // runs game code on one of them, concurrently with the game thread.
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &block, NULL);

    // Six, because 240x160 was a screen four inches wide and is a postage stamp
    // on a desk monitor. The art was drawn for pixels of a certain apparent
    // size rather than for a certain count of them, and six is what puts them
    // back at roughly that size. host_video_open brings it down if the window
    // would not fit the display.
    if (!host_video_open("frlg-native", AGB_PPU_MIN_W, AGB_PPU_MIN_H, 6))
        return 1;

    // Before anything else can fail: a session that opens after the first
    // complaint is a session missing the complaint.
    host_session_open();
    host_session_capture_log();
    if (host_session_dir() != NULL)
        printf("frlg-native: session %s\n", host_session_dir());

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
        const char *r = getenv("FRLG_SHOT_RANGE");
        static char dir[256];

        if (r != NULL && sscanf(r, "%u:%u:%u:%255s", &shot_first, &shot_last, &shot_step, dir) == 4)
            shot_dir = dir;
    }
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

        // How much world fits, at the zoom the player chose. The window is the
        // question and the viewport is the answer; the renderer clamps it to
        // what the game actually has drawn (ADR-less for now: agb/ppu.h says
        // why 256 and not more).
        {
            const char *want = getenv("FRLG_VIEW");
            int win_w = 0, win_h = 0, z = host_video_zoom();
            int vw = 0, vh = 0;

            // The environment wins here as everywhere else, and it is what lets
            // a headless run compose something other than the native size.
            if (want != NULL && sscanf(want, "%dx%d", &vw, &vh) == 2)
            {
                agb_ppu_set_viewport(vw, vh);
            }
            else if (!agb_port_field_active())
            {
                // Menus, battles and the like are drawn for the hardware's
                // size; past that edge there is nothing of theirs to show. They
                // keep it, and the window letterboxes them.
                agb_ppu_set_viewport(AGB_PPU_MIN_W, AGB_PPU_MIN_H);
            }
            else
            {
                host_video_window_size(&win_w, &win_h);
                if (z > 0 && win_w > 0 && win_h > 0)
                    agb_ppu_set_viewport(win_w / z, win_h / z);
            }
        }

        // FRLG_VRAM_DUMP writes video memory to a file, overwritten each frame,
        // so the file holds whatever was there last. Widescreen needs somewhere
        // to put larger tilemaps, and where VRAM is free is a question about
        // what the game does rather than what its constants say -- the region
        // the arithmetic suggested turned out to be occupied and one nobody had
        // named was empty. Costs a 96 KB write per frame and is for answering
        // that kind of question, not for leaving on.
        {
            const char *dump = getenv("FRLG_VRAM_DUMP");

            if (dump != NULL)
            {
                FILE *fh = fopen(dump, "wb");

                if (fh != NULL)
                {
                    fwrite(agb_mem.vram, 1, sizeof(agb_mem.vram), fh);
                    fclose(fh);
                }
            }
        }

        copy_frame();
        // Through the pipeline rather than straight to the backend: with no
        // stage registered this is the same call with one branch in front.
        host_render_present(framebuffer, SCREEN_W, SCREEN_H);

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

    // The game only stops when asked: its main loop has no exit of its own, and
    // a plain join on a thread still running it is the hang the desktop offers to
    // force-quit out of. The wait is bounded for the same reason -- a game thread
    // wedged somewhere that never reaches a frame boundary must not keep the
    // window alive either.
    agb_frame_stop();
    {
        struct timespec deadline;

        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 2;
        if (pthread_timedjoin_np(game, NULL, &deadline) != 0)
            fprintf(stderr, "frlg-native: the game thread did not stop; closing anyway\n");
    }

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

    printf("frlg-native: ran %u frames\n", frames_ran);
    if (agb_frame_watchdog_ticks() != 0)
        printf("frlg-native: %u frames advanced by the stall watchdog\n",
               agb_frame_watchdog_ticks());
    printf("frlg-native: audio %u frames, %u non-silent, peak %d\n",
           audio_frames, audio_loud_frames, audio_peak);

    // The report is assembled after the log is closed, so the log inside it is
    // complete -- including the lines the crash itself wrote.
    if (crash_happened())
    {
        const char *dir = host_session_dir();
        const char *zip;

        printf("\nfrlg-native: the game stopped because of a bug -- %s\n",
               crash_summary());
        host_session_close();
        zip = host_session_bundle();

        if (zip != NULL)
        {
            fprintf(stderr, "\nEverything needed to reproduce it is in one file:\n  %s\n", zip);
            fprintf(stderr, "\nIt holds what you pressed, your save as it was when this run\n"
                            "began, everything printed, and where the game stopped.\n");
            fprintf(stderr, "\nPlease attach it to an issue:\n  %s\n", FRLG_ISSUE_URL);
        }
        else if (dir != NULL)
        {
            fprintf(stderr, "\nWhat happened was saved here:\n  %s\n", dir);
        }

        // The window is what the player was looking at, so that is where they
        // are told -- which is why it is still open here. Inert when there is
        // no display: a headless run must never wait for an answer.
        host_report_crash(crash_summary(), zip, FRLG_ISSUE_URL);

        host_video_close();
        return 3;
    }

    host_video_close();

    // Last, so the closing summary is in the log the session keeps.
    host_session_close();
    return 0;
}
