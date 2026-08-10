// Capture PCM from mGBA running the reference ROM.
//
// The audio counterpart of mgba_capture: the golden tier compares our frames
// against a known-good emulator, and this is what lets the same be done for
// sound. It writes raw signed 16-bit stereo at a rate the caller chooses, so a
// comparison can resample once rather than argue about it.
//
// It is a host tool, not part of the port. It links the system libmgba, which is
// 64-bit, so it is built outside the port's -m32 world.
//
// usage: mgba-audio ROM OUT.raw FRAMES [RATE]

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/log.h>

static void quiet_log(struct mLogger* logger, int category, enum mLogLevel level,
                      const char* format, va_list args)
{
    (void)logger; (void)category; (void)level; (void)format; (void)args;
}

static struct mLogger silent = {.log = quiet_log};

#define CHUNK 4096

int main(int argc, char** argv)
{
    struct mCore* core;
    color_t* buffer;
    unsigned width, height;
    FILE* out;
    unsigned frames, frame;
    int rate;
    short samples[CHUNK * 2];

    if (argc < 4)
    {
        fprintf(stderr, "usage: %s ROM OUT.raw FRAMES [RATE]\n", argv[0]);
        return 2;
    }

    frames = (unsigned)strtoul(argv[3], NULL, 10);
    rate = argc > 4 ? (int)strtol(argv[4], NULL, 10) : 13379;

    mLogSetDefaultLogger(&silent);

    core = mCoreFind(argv[1]);
    if (!core || !core->init(core))
    {
        fprintf(stderr, "mgba-audio: cannot open %s\n", argv[1]);
        return 1;
    }
    mCoreInitConfig(core, NULL);

    // A video buffer is still required even though nothing reads it: the core
    // renders whether or not anyone is looking.
    core->desiredVideoDimensions(core, &width, &height);
    buffer = calloc((size_t)width * height, sizeof(color_t));
    core->setVideoBuffer(core, buffer, width);

    if (!mCoreLoadFile(core, argv[1]))
    {
        fprintf(stderr, "mgba-audio: cannot load %s\n", argv[1]);
        return 1;
    }
    core->reset(core);

    // Ask the core to resample to the rate we want. blip_buf does the band
    // limiting, which is why this is worth doing here rather than afterwards.
    core->setAudioBufferSize(core, CHUNK);
    blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), rate);
    blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), rate);

    out = fopen(argv[2], "wb");
    if (!out)
    {
        perror(argv[2]);
        return 1;
    }

    for (frame = 0; frame < frames; frame++)
    {
        int available;

        core->runFrame(core);

        // Both channels advance together, so either one's count will do.
        while ((available = blip_samples_avail(core->getAudioChannel(core, 0))) > 0)
        {
            int want = available > CHUNK ? CHUNK : available;

            blip_read_samples(core->getAudioChannel(core, 0), samples, want, 1);
            blip_read_samples(core->getAudioChannel(core, 1), samples + 1, want, 1);
            fwrite(samples, sizeof(short) * 2, (size_t)want, out);
        }
    }

    fclose(out);
    core->deinit(core);
    free(buffer);
    return 0;
}
