#include <SDL3/SDL.h>

#include "host.h"

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

bool host_video_open(const char *title, int width, int height, int scale)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        host_log(SDL_GetError());
        return false;
    }

    if (!SDL_CreateWindowAndRenderer(title, width * scale, height * scale,
                                     SDL_WINDOW_RESIZABLE, &window, &renderer))
    {
        host_log(SDL_GetError());
        return false;
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

            if (down && ev.key.scancode == SDL_SCANCODE_ESCAPE)
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
