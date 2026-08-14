// Capture reference frames from mGBA running the reference ROM.
//
// This is the oracle half of the golden tier: the port is compared against a
// known-good emulator running the same build of the same game, so a golden says
// "matches the ROM" rather than only "matches yesterday".
//
// It is a host tool, not part of the port. It links the system libmgba, which
// is 64-bit, so it is built outside the port's -m32 world and shares nothing
// with it but the PPM format the harness reads.
//
// usage: mgba-capture ROM OUTDIR FRAME [FRAME...]     (frames in any order)
//
// FRLG_INPUT=<trace> replays the port's own input trace here too, so a frame
// that can only be reached by playing can still be compared. The trace holds
// active-low masks, the way the key register reads; mGBA wants the pressed bits,
// so they are inverted on the way in. FRLG_SAV=<file> supplies the save the
// trace was recorded against, which a replayed trace is meaningless without.

// mGBA's headers use PATH_MAX, which is POSIX rather than ISO -- hence gnu11
// for this file. It is a host tool; the port itself stays strict C11.
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>
// The affine parameters are write-only on hardware, so the bus returns open bus
// for them and busRead16 cannot see what the game wrote. mGBA keeps every I/O
// write in its own shadow array regardless, which is the only way to read them
// back -- hence reaching past the core interface into the GBA it is driving.
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/renderers/video-software.h>

// mGBA narrates every DMA and BIOS call to stderr otherwise, which buries the
// harness output and says nothing about whether a frame is right.
static void quiet_log(struct mLogger* logger, int category, enum mLogLevel level,
                      const char* format, va_list args)
{
    (void)logger;
    (void)category;
    (void)level;
    (void)format;
    (void)args;
}

static struct mLogger silent = {.log = quiet_log};

static int compare_unsigned(const void* a, const void* b)
{
    unsigned x = *(const unsigned*)a;
    unsigned y = *(const unsigned*)b;

    return (x > y) - (x < y);
}

// The same P6 PPM the port's own FRLG_SHOT writes, so the harness reads either
// without knowing which produced it.
static int write_ppm(const char* path, const color_t* pixels, unsigned width, unsigned height)
{
    FILE* fh = fopen(path, "wb");
    unsigned i;

    if (!fh)
    {
        perror(path);
        return 0;
    }

    fprintf(fh, "P6\n%u %u\n255\n", width, height);
    for (i = 0; i < width * height; i++)
    {
        // mGBA packs a colour as R, G, B, A from the low byte up.
        unsigned char rgb[3] = {(unsigned char)(pixels[i] & 0xFF),
                                (unsigned char)((pixels[i] >> 8) & 0xFF),
                                (unsigned char)((pixels[i] >> 16) & 0xFF)};

        fwrite(rgb, 1, 3, fh);
    }
    fclose(fh);
    return 1;
}

#define TRACE_MAX 65536

static struct
{
    unsigned frame;
    unsigned keys;   // already inverted to mGBA's sense
} trace[TRACE_MAX];

static unsigned trace_count;

static void load_trace(void)
{
    const char* path = getenv("FRLG_INPUT");
    FILE* fh;
    char line[128];

    if (!path)
        return;

    fh = fopen(path, "r");
    if (!fh)
    {
        fprintf(stderr, "mgba-capture: cannot read %s\n", path);
        return;
    }

    while (trace_count < TRACE_MAX && fgets(line, sizeof(line), fh))
    {
        unsigned frame, keys;

        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%u %x", &frame, &keys) != 2)
            continue;
        trace[trace_count].frame = frame;
        trace[trace_count].keys = (~keys) & 0x3FF;
        trace_count++;
    }
    fclose(fh);
    fprintf(stderr, "mgba-capture: replaying %u input events\n", trace_count);
}

int main(int argc, char** argv)
{
    struct mCore* core;
    color_t* buffer;
    unsigned width, height;
    unsigned* frames;
    int count = argc - 3;
    int i, next = 0;
    unsigned frame = 0;
    unsigned trace_pos = 0;
    unsigned keys = 0;
    unsigned trace_offset = 38;

    {
        const char* off = getenv("FRLG_INPUT_OFFSET");

        if (off)
            trace_offset = (unsigned)strtoul(off, NULL, 10);
    }

    if (argc < 4)
    {
        fprintf(stderr, "usage: %s ROM OUTDIR FRAME [FRAME...]\n", argv[0]);
        return 2;
    }

    frames = calloc((size_t)count, sizeof(*frames));
    for (i = 0; i < count; i++)
        frames[i] = (unsigned)strtoul(argv[3 + i], NULL, 10);
    qsort(frames, (size_t)count, sizeof(*frames), compare_unsigned);

    load_trace();
    mLogSetDefaultLogger(&silent);

    core = mCoreFind(argv[1]);
    if (!core || !core->init(core))
    {
        fprintf(stderr, "mgba-capture: cannot open %s\n", argv[1]);
        return 1;
    }
    mCoreInitConfig(core, NULL);
    core->desiredVideoDimensions(core, &width, &height);
    buffer = calloc((size_t)width * height, sizeof(color_t));
    core->setVideoBuffer(core, buffer, width);

    if (!mCoreLoadFile(core, argv[1]))
    {
        fprintf(stderr, "mgba-capture: cannot load %s\n", argv[1]);
        return 1;
    }
    // FRLG_SAV loads the port's own flash image, so a trace recorded from a save
    // reaches the same place here. Opened read-write because mGBA maps flash
    // writable and a read-only file leaves it with no save memory at all.
    {
        const char* sav = getenv("FRLG_SAV");
        struct VFile* vf = sav ? VFileOpen(sav, O_RDWR) : NULL;

        if (sav && !vf)
            fprintf(stderr, "mgba-capture: cannot read %s\n", sav);
        if (vf)
            core->loadSave(core, vf);
    }
    core->reset(core);

    // FRLG_DUMP_KEYS=FIRST:LAST asks what the game itself made of the input over
    // a range of frames -- the register it read, and the held/new keys it
    // derived. gMain is at a known address in the reference build's map, so this
    // is the same question the port can answer about itself, asked of the
    // reference. Pixels cannot answer it: two runs can look identical and be one
    // press apart.
    unsigned keys_first = 0, keys_last = 0;
    {
        const char* range = getenv("FRLG_DUMP_KEYS");

        if (range)
            sscanf(range, "%u:%u", &keys_first, &keys_last);
    }

    // One pass for every frame asked for: the emulator is cheap, but running it
    // from reset once per frame would not be.
    while (next < count)
    {
        while (frames[next] == frame)
        {
            char path[512];

            snprintf(path, sizeof(path), "%s/frame%u.ppm", argv[2], frame);
            if (!write_ppm(path, buffer, width, height))
                return 1;
            printf("  captured frame %u\n", frame);

            // The sequencer's own limits, read out of the reference: maxChans is
            // how many software mixing channels it will hand out, and a song with
            // more tracks than that loses instruments.
            if (getenv("FRLG_DUMP_SOUND"))
            {
                uint32_t si = core->busRead32(core, 0x03007FF0);

                printf("    frame %u SoundInfo %08X: reverb=%u maxChans=%u masterVol=%u freq=%u\n",
                       frame, si, core->busRead8(core, si + 5),
                       core->busRead8(core, si + 6), core->busRead8(core, si + 7),
                       core->busRead8(core, si + 8));
            }

            // FRLG_DUMP_REGS asks what the reference had in its display
            // registers at the same moment. Comparing those rather than pixels
            // is what settles which layer a missing picture belongs to.
            if (getenv("FRLG_DUMP_REGS"))
            {
                static const struct { const char* name; unsigned off; } regs[] = {
                    {"DISPCNT", 0x000}, {"BG0CNT", 0x008},  {"BG1CNT", 0x00A},
                    {"BG2CNT", 0x00C},  {"BG3CNT", 0x00E},  {"BG2PA", 0x020},
                    {"BG2PB", 0x022},   {"BG2PC", 0x024},   {"BG2PD", 0x026},
                    {"BG2X_LO", 0x028}, {"BG2X_HI", 0x02A}, {"BG2Y_LO", 0x02C},
                    {"BG2Y_HI", 0x02E}, {"BLDCNT", 0x050},
                    {"SOUNDCNT_L", 0x080}, {"SOUNDCNT_H", 0x082},
                };
                const uint16_t* io = ((struct GBA*)core->board)->memory.io;
                unsigned r;

                printf("    frame %u registers:", frame);
                for (r = 0; r < sizeof(regs) / sizeof(regs[0]); r++)
                    printf(" %s=%04X", regs[r].name, io[regs[r].off >> 1]);
                printf("\n");

                // The shadow array is only as truthful as mGBA's decision to
                // store a write in it. What its renderer is about to draw with
                // is not a decision -- it is the state the frame comes from.
                {
                    const struct GBAVideoSoftwareRenderer* sr =
                        (const struct GBAVideoSoftwareRenderer*)((struct GBA*)core->board)->video.renderer;
                    unsigned b;

                    for (b = 2; b < 4; b++)
                        printf("    frame %u renderer bg%u: enabled=%d dx=%d dy=%d "
                               "sx=%d sy=%d refx=%d refy=%d\n",
                               frame, b, sr->bg[b].enabled, sr->bg[b].dx, sr->bg[b].dy,
                               sr->bg[b].sx, sr->bg[b].sy, sr->bg[b].refx, sr->bg[b].refy);
                }
            }
            if (++next >= count)
                break;
        }
        if (next >= count)
            break;

        if (keys_last && frame >= keys_first && frame <= keys_last)
        {
            // gMain, from pokefirered_modern.map; heldKeys at +0x2C, newKeys at
            // +0x2E, and KEYINPUT is the register the game read them from.
            uint32_t gmain = 0x0300462C;

            printf("keys %u KEYINPUT=%04X held=%04X new=%04X intrCheck=%04X\n", frame,
                   core->busRead16(core, 0x04000130),
                   core->busRead16(core, gmain + 0x2C),
                   core->busRead16(core, gmain + 0x2E),
                   core->busRead16(core, gmain + 0x1C));
        }

        // Applied before the frame runs, so the keys this frame sees are the
        // ones the trace names for it -- the same rule the port follows.
        // mGBA runs the BIOS and the ROM's crt0 where the port enters AgbMain
        // directly, so its frame numbering leads ours by a fixed offset -- the
        // same one the golden manifest records. The trace is indexed by our
        // numbering, so it is shifted here rather than rewritten.
        while (trace_pos < trace_count
               && trace[trace_pos].frame + trace_offset <= frame)
            keys = trace[trace_pos++].keys;
        core->setKeys(core, keys);

        core->runFrame(core);
        frame++;
    }

    core->deinit(core);
    free(buffer);
    free(frames);
    return 0;
}
