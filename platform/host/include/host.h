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

// False once the user has asked to quit.
bool host_pump_events(void);

uint16_t host_input_keys(void);

void host_log(const char *msg);

#endif // GUARD_HOST_H
