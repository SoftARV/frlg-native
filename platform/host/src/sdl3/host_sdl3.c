#include <SDL3/SDL.h>

#include <dlfcn.h>
#include <stdlib.h>

#include <stdio.h>
#include <string.h>

#include "host.h"

// Drift between the game's frame pacing and the sound card's clock has to show
// up somewhere; these are where it does.
static unsigned audio_submits, audio_starved, audio_dropped;
static int audio_queue_peak;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *frame;
static uint16_t keys = HOST_KEYS_RELEASED;
static bool quit_requested;

static const struct
{
    SDL_Scancode code;
    uint16_t key;
} key_map[] = {
    {SDL_SCANCODE_X, HOST_KEY_A},
    {SDL_SCANCODE_Z, HOST_KEY_B},
    {SDL_SCANCODE_BACKSPACE, HOST_KEY_SELECT},
    {SDL_SCANCODE_RETURN, HOST_KEY_START},
    {SDL_SCANCODE_RIGHT, HOST_KEY_RIGHT},
    {SDL_SCANCODE_LEFT, HOST_KEY_LEFT},
    {SDL_SCANCODE_UP, HOST_KEY_UP},
    {SDL_SCANCODE_DOWN, HOST_KEY_DOWN},
    {SDL_SCANCODE_S, HOST_KEY_R},
    {SDL_SCANCODE_A, HOST_KEY_L},
};

#define KEY_MAP_COUNT ((int)(sizeof(key_map) / sizeof(key_map[0])))

// A window with no way to close it is no use for testing, and that is what
// Wayland gives a build that cannot load libdecor: the compositor draws no
// decorations of its own, SDL draws none either, and the result is borderless
// with no title bar and no close button. This port is 32-bit (ADR 0012), so a
// 64-bit libdecor on the system does not help it.
//
// Asking for X11 instead is the fix that needs nothing installed: an X server is
// already there under Wayland, and its window manager decorates. Only when this
// build genuinely cannot load libdecor, and only when nothing has asked for a
// particular driver.
static void prefer_a_window_that_can_be_closed(void)
{
    void *decor;

    if (getenv("WAYLAND_DISPLAY") == NULL || getenv("SDL_VIDEO_DRIVER") != NULL)
        return;

    decor = dlopen("libdecor-0.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (decor != NULL)
    {
        dlclose(decor);
        return;
    }

    if (getenv("DISPLAY") == NULL)
        return;

    host_log("no libdecor for this build: asking for X11 so the window has a "
             "title bar");
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
}

bool host_video_open(const char *title, int width, int height, int scale)
{
    prefer_a_window_that_can_be_closed();

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        host_log(SDL_GetError());
        return false;
    }

    // A window is asked for in the units the driver works in, and those differ:
    // Wayland takes logical pixels and the compositor scales them up, X11 takes
    // real ones. On a display asking for 200% that is the difference between a
    // comfortable window and a postage stamp, so the display's own content scale
    // is folded in. FRLG_SCALE overrides the lot.
    {
        const char *want = getenv("FRLG_SCALE");
        float content = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        char line[96];

        if (want != NULL)
            scale = atoi(want) > 0 ? atoi(want) : scale;
        else if (content >= 1.5f)
            scale = (int)(scale * content + 0.5f);

        snprintf(line, sizeof(line), "window scale %d (display asks for %.2f)",
                 scale, content);
        host_log(line);
    }

    if (!SDL_CreateWindowAndRenderer(title, width * scale, height * scale,
                                     SDL_WINDOW_RESIZABLE, &window, &renderer))
    {
        host_log(SDL_GetError());
        return false;
    }

    {
        const char *driver = SDL_GetCurrentVideoDriver();
        char line[96];

        snprintf(line, sizeof(line), "video driver: %s", driver ? driver : "?");
        host_log(line);
    }

    frame = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888,
                              SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!frame)
    {
        host_log(SDL_GetError());
        return false;
    }

    // Integer-friendly scaling with no filtering: the GBA's output is a pixel
    // grid, and smoothing it is a decision for a display mode, not a default.
    SDL_SetTextureScaleMode(frame, SDL_SCALEMODE_NEAREST);
    SDL_SetRenderLogicalPresentation(renderer, width, height,
                                     SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    return true;
}

void host_video_close(void)
{
    if (frame)
        SDL_DestroyTexture(frame);
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
}

void host_video_present(const uint32_t *rgba, int width, int height)
{
    if (!frame)
        return;

    SDL_UpdateTexture(frame, NULL, rgba, width * (int)sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, frame, NULL, NULL);
    SDL_RenderPresent(renderer);
}

bool host_pump_events(void)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev))
    {
        switch (ev.type)
        {
        case SDL_EVENT_QUIT:
            quit_requested = true;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
            bool down = ev.type == SDL_EVENT_KEY_DOWN;

            // Ctrl+Q rather than Escape: a key that quits is a key a player
            // will hit by accident, and losing a recording to a stray press has
            // happened.
            if (down && ev.key.scancode == SDL_SCANCODE_Q
                && (ev.key.mod & SDL_KMOD_CTRL) != 0)
                quit_requested = true;

            for (int i = 0; i < KEY_MAP_COUNT; i++)
            {
                if (key_map[i].code != ev.key.scancode)
                    continue;
                if (down)
                    keys &= (uint16_t)~key_map[i].key;
                else
                    keys |= key_map[i].key;
            }
            break;
        }
        default:
            break;
        }
    }

    return !quit_requested;
}

uint16_t host_input_keys(void)
{
    return keys;
}

void host_log(const char *msg)
{
    SDL_Log("%s", msg);
}

// ---------------------------------------------------------------------- audio ---

static SDL_AudioStream *audio;
static int audio_rate;

// SDL resamples from the mixer's rate to whatever the device runs at, so the
// port hands over the GBA's own 8-bit stereo and does no conversion itself.
bool host_audio_open(int sample_rate)
{
    SDL_AudioSpec spec;

    if (sample_rate <= 0)
        return false;

    if (audio != NULL && sample_rate == audio_rate)
        return true;

    host_audio_close();

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        host_log(SDL_GetError());
        return false;
    }

    spec.format = SDL_AUDIO_S8;
    spec.channels = 2;
    spec.freq = sample_rate;

    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio == NULL)
    {
        host_log(SDL_GetError());
        return false;
    }

    audio_rate = sample_rate;
    SDL_ResumeAudioStreamDevice(audio);
    return true;
}

void host_audio_close(void)
{
    if (audio_submits != 0)
    {
        char line[160];

        snprintf(line, sizeof(line),
                 "audio: %u frames submitted, queue peaked at %d bytes, "
                 "%u starved, %u dropped",
                 audio_submits, audio_queue_peak, audio_starved, audio_dropped);
        host_log(line);
    }

    if (audio != NULL)
    {
        SDL_DestroyAudioStream(audio);
        audio = NULL;
        audio_rate = 0;
    }
}

void host_audio_submit(const int8_t *right, const int8_t *left, int samples)
{
    // One frame is a few dozen samples at the slowest rate and a couple of
    // thousand at the fastest, once the mixer's oversampling is counted, so
    // interleaving on the stack costs nothing and avoids a lock.
    int8_t frame[8192];

    if (audio == NULL || samples <= 0)
        return;

    if (samples > (int)(sizeof(frame) / 2))
        samples = (int)(sizeof(frame) / 2);

    for (int i = 0; i < samples; i++)
    {
        frame[i * 2] = left[i];
        frame[i * 2 + 1] = right[i];
    }

    // Nothing ties the game's frame pacing to the sound card's clock, so the two
    // drift: the queue either grows until it is dropped or empties and the device
    // plays silence. Both are audible, and neither leaves a trace in the mixer's
    // own output -- hence counting them here, where the difference shows.
    {
        int queued = SDL_GetAudioStreamQueued(audio);

        audio_submits++;
        if (queued > audio_queue_peak)
            audio_queue_peak = queued;
        // Ignore the first few frames, where an empty queue is just the start.
        if (queued == 0 && audio_submits > 8)
            audio_starved++;

        // Falling behind is better than blocking the game thread: if the device
        // has stopped consuming, the queue is dropped rather than allowed to
        // grow. The bound is a length of time rather than a multiple of the
        // buffer, which changes with the rate: a fifth of a second at the
        // oversampled rate, stereo, one byte a sample.
        if (queued > 20000)
        {
            SDL_ClearAudioStream(audio);
            audio_dropped++;
        }
    }

    SDL_PutAudioStreamData(audio, frame, samples * 2);
}

// The player is looking at the window, not at a terminal they may never have
// opened. A native message box rather than something drawn in the window: the
// renderer's state after a fault is not something to rely on, and this needs no
// font, no layout and no new dependency to be legible on every platform SDL
// covers.
//
// The buttons are the only three useful things a person can do with a report --
// find it, name it, or go where it belongs. Deliberately no "send": that would
// make this a service rather than a file (ADR 0016).
int host_report_crash(const char *detail, const char *path, const char *issues_url)
{
    SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, HOST_REPORT_FOLDER, "Show the report"},
        {0, HOST_REPORT_COPY, "Copy its location"},
        {0, HOST_REPORT_ISSUES, "Report it"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, HOST_REPORT_QUIT, "Close"},
    };
    SDL_MessageBoxData box;
    char body[1024];
    int chosen = HOST_REPORT_QUIT;

    snprintf(body, sizeof(body),
             "The game hit a bug and stopped. This is the port's fault, not "
             "anything you did, and nothing is lost but this session.\n\n"
             "%s\n\n"
             "A report was saved:\n%s\n\n"
             "It holds what you pressed, your save as it was when this run began, "
             "everything the game printed, and where it stopped. Attaching it to "
             "an issue is enough to reproduce and fix this.",
             detail != NULL ? detail : "", path != NULL ? path : "(none)");

    SDL_zero(box);
    box.flags = SDL_MESSAGEBOX_ERROR;
    box.window = window;
    box.title = "frlg-native stopped";
    box.message = body;
    box.numbuttons = path != NULL ? 4 : 1;
    // With no report to point at, the only honest button is the one that closes.
    box.buttons = path != NULL ? buttons : &buttons[3];

    if (!SDL_ShowMessageBox(&box, &chosen))
        return HOST_REPORT_QUIT;

    if (chosen == HOST_REPORT_COPY && path != NULL)
    {
        SDL_SetClipboardText(path);
    }
    else if (chosen == HOST_REPORT_FOLDER && path != NULL)
    {
        char url[1024];
        char *cut;

        // The folder, not the file: a file manager opened on a directory is
        // where somebody can drag the zip from.
        snprintf(url, sizeof(url), "file://%s", path);
        cut = strrchr(url, '/');
        if (cut != NULL)
            *cut = '\0';
        SDL_OpenURL(url);
    }
    else if (chosen == HOST_REPORT_ISSUES && issues_url != NULL)
    {
        SDL_OpenURL(issues_url);
    }
    return chosen;
}
