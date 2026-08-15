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

// Headless: there is no window, so the viewport never changes and the zoom is
// nobody's business. Reported as the native size so a caller doing the same
// arithmetic as the windowed one arrives at the same answer.
bool host_video_fullscreen(void)
{
    return false;
}

void host_video_set_fullscreen(bool on)
{
    (void)on;
}

int host_video_zoom(void)
{
    return 1;
}

void host_video_set_zoom(int zoom)
{
    (void)zoom;
}

void host_video_window_size(int *width, int *height)
{
    if (width)
        *width = 240;
    if (height)
        *height = 160;
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

// Nothing to ask on, and a headless run must never wait for an answer -- a CI
// job that stops on a dialog looks exactly like one that hung.
int host_report_crash(const char *detail, const char *path, const char *issues_url)
{
    (void)detail;
    (void)path;
    (void)issues_url;
    return HOST_REPORT_QUIT;
}
