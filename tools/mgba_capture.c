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

// mGBA's headers use PATH_MAX, which is POSIX rather than ISO -- hence gnu11
// for this file. It is a host tool; the port itself stays strict C11.
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>

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

int main(int argc, char** argv)
{
    struct mCore* core;
    color_t* buffer;
    unsigned width, height;
    unsigned* frames;
    int count = argc - 3;
    int i, next = 0;
    unsigned frame = 0;

    if (argc < 4)
    {
        fprintf(stderr, "usage: %s ROM OUTDIR FRAME [FRAME...]\n", argv[0]);
        return 2;
    }

    frames = calloc((size_t)count, sizeof(*frames));
    for (i = 0; i < count; i++)
        frames[i] = (unsigned)strtoul(argv[3 + i], NULL, 10);
    qsort(frames, (size_t)count, sizeof(*frames), compare_unsigned);

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
    core->reset(core);

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
            if (++next >= count)
                break;
        }
        if (next >= count)
            break;
        core->runFrame(core);
        frame++;
    }

    core->deinit(core);
    free(buffer);
    free(frames);
    return 0;
}
