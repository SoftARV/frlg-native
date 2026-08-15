#ifndef GUARD_HOST_H
#define GUARD_HOST_H

#include <stdbool.h>
#include <stdint.h>

// The entire porting surface. A new platform implements this and nothing else.
// Nothing here knows what a GBA is: the port layer above wires host input to
// the key register and the PPU's framebuffer to host_video_present.

// GBA key bits, active low -- a set bit means released.
#define HOST_KEY_A (1 << 0)
#define HOST_KEY_B (1 << 1)
#define HOST_KEY_SELECT (1 << 2)
#define HOST_KEY_START (1 << 3)
#define HOST_KEY_RIGHT (1 << 4)
#define HOST_KEY_LEFT (1 << 5)
#define HOST_KEY_UP (1 << 6)
#define HOST_KEY_DOWN (1 << 7)
#define HOST_KEY_R (1 << 8)
#define HOST_KEY_L (1 << 9)
#define HOST_KEYS_RELEASED 0x03FF

bool host_video_open(const char *title, int width, int height, int scale);
void host_video_close(void);
void host_video_present(const uint32_t *rgba, int width, int height);

// The size of one game pixel on screen. Independent of the window: a bigger
// window shows more of the world at the same zoom, rather than the same amount
// of world made larger.
int host_video_zoom(void);
void host_video_set_zoom(int zoom);

// In real pixels, so the caller can work out how much world now fits.
void host_video_window_size(int *width, int *height);

// False once the user has asked to quit.
bool host_pump_events(void);

uint16_t host_input_keys(void);

// Audio output. The GBA's mixer produces 8-bit signed stereo at its own rate --
// about 13.4 kHz -- and the host resamples to whatever the device wants.
//
// `open` may be called again with a different rate: the game changes it through
// m4aSoundMode. Submitting before opening, or after a failed open, is silently
// ignored, so a platform with no audio needs no special case above it.
bool host_audio_open(int sample_rate);
void host_audio_close(void);

// One frame's worth. The two sides arrive as separate buffers, the way the
// mixer keeps them.
void host_audio_submit(const int8_t *right, const int8_t *left, int samples);

void host_log(const char *msg);

// Tell the player the game stopped, and offer the few things they can usefully
// do about it. `detail` is the one-line summary, `path` the report to hand over.
// Returns what they chose, so the port can act on it; HOST_REPORT_QUIT when
// there is no display to ask on, since a headless run must never block.
#define HOST_REPORT_QUIT 0
#define HOST_REPORT_FOLDER 1
#define HOST_REPORT_ISSUES 2

int host_report_crash(const char *detail, const char *path, const char *issues_url);

#endif // GUARD_HOST_H
