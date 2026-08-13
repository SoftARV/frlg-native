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

#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>

static void quiet_log(struct mLogger* logger, int category, enum mLogLevel level,
                      const char* format, va_list args)
{
    (void)logger; (void)category; (void)level; (void)format; (void)args;
}

static struct mLogger silent = {.log = quiet_log};

#define CHUNK 4096


// FRLG_INPUT replays the port's own input trace here too, shifted by the boot
// offset, so a sound that needs a button press can be compared and not only the
// intro. The trace holds active-low masks, the way the key register reads; mGBA
// wants the pressed bits, so they are inverted on the way in.
#define TRACE_MAX 65536

static struct
{
    unsigned frame;
    unsigned keys;
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
        fprintf(stderr, "mgba-audio: cannot read %s\n", path);
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
    fprintf(stderr, "mgba-audio: replaying %u input events\n", trace_count);
}

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

    load_trace();
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
    // FRLG_SAV loads the port's own flash image, which is the same 128K layout
    // mGBA expects. Music in the overworld is otherwise forty minutes of trace
    // away, and the reference cannot be trusted to stay in step that long.
    {
        const char* sav = getenv("FRLG_SAV");
        struct VFile* vf = sav ? VFileOpen(sav, O_RDWR) : NULL;

        if (sav && !vf)
            fprintf(stderr, "mgba-audio: cannot read %s\n", sav);
        if (vf)
            core->loadSave(core, vf);
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

        // FRLG_AUDIO_MUTE=direct silences the two direct-sound FIFOs by clearing
        // their routing bits in SOUNDCNT_H, every frame. Unlike the PSG's enable
        // bits in SOUNDCNT_L, which the sequencer rewrites as channels come and
        // go, SOUNDCNT_H is written only at init and when the sound option
        // changes -- so this one does not leak, and the reference's hardware
        // channels can be compared against the port's on their own.
        {
            const char* mute = getenv("FRLG_AUDIO_MUTE");

            if (mute && mute[0] == 'd')
            {
                uint16_t cnt_h = core->busRead16(core, 0x04000082);

                core->busWrite16(core, 0x04000082, (uint16_t)(cnt_h & ~0x3300));
            }
        }

        {
            static unsigned trace_pos = 0;
            static unsigned keys = 0;
            unsigned offset = 38;
            const char* off = getenv("FRLG_INPUT_OFFSET");

            if (off)
                offset = (unsigned)strtoul(off, NULL, 10);
            while (trace_pos < trace_count && trace[trace_pos].frame + offset <= frame)
                keys = trace[trace_pos++].keys;
            core->setKeys(core, keys);
        }

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
