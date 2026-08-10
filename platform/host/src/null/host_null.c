#include <stdio.h>

#include "host.h"

// Headless backend for CI and the determinism harness. Every call succeeds and
// nothing is displayed, so a run exercises the game and the hardware layer
// without needing a display server.

bool host_video_open(const char *title, int width, int height, int scale)
{
    (void)title;
    (void)width;
    (void)height;
    (void)scale;
    return true;
}

void host_video_close(void)
{
}

void host_video_present(const uint32_t *rgba, int width, int height)
{
    (void)rgba;
    (void)width;
    (void)height;
}

bool host_pump_events(void)
{
    return true;
}

uint16_t host_input_keys(void)
{
    return HOST_KEYS_RELEASED;
}

void host_log(const char *msg)
{
    fprintf(stderr, "host: %s\n", msg);
}

bool host_audio_open(int sample_rate)
{
    (void)sample_rate;
    return false;
}

void host_audio_close(void)
{
}

void host_audio_submit(const int8_t *right, const int8_t *left, int samples)
{
    (void)right;
    (void)left;
    (void)samples;
}
