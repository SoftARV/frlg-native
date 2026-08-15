// Display pipelines: stages that run over the finished framebuffer on its way
// to the screen.
//
// This is the seam an alternative renderer needs to be a mod rather than a fork
// (ARCHITECTURE §8). It lives here, at the bottom, because the dependency rule
// is strictly downward: the mod layer registers stages, the presenter runs
// them, and nothing below ever calls upward.
//
// It sits *above* the framebuffer and below presentation, which is what keeps
// it out of the tests: screenshots and the golden harness read
// agb_ppu_framebuffer() directly, so no stage can change what a recorded run is
// compared against.
//
// Two rules from the architecture, and they are the reason this is not just an
// array of function pointers:
//
//   - A stage that fails retires only itself, named, and the frame falls back
//     to what it was given. A broken stage costs a display mode, never the
//     game.
//   - Availability is re-read every frame, so a stage that cannot run right now
//     -- headless, no GPU, wrong window size -- simply does not, without being
//     retired for it.
#ifndef HOST_RENDER_H
#define HOST_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#define HOST_RENDER_MAX_STAGES 8

struct host_render_stage
{
    // Named so a failure can be attributed to something a person recognises.
    const char *name;

    // Re-read every frame. NULL means "always". Returning false skips the stage
    // for this frame only.
    bool (*available)(void *user);

    // Transforms the frame in place. Returning false retires the stage and the
    // frame is presented as it was before this stage ran.
    bool (*run)(uint32_t *pixels, int width, int height, void *user);

    void *user;
};

// Returns a stage id, or -1 if the table is full. The struct is copied.
int host_render_register(const struct host_render_stage *stage);
void host_render_unregister(int id);

// Enumeration, for a settings screen that lists what is actually registered
// rather than a hardcoded list of what someone expected to be.
int host_render_count(void);
const char *host_render_name(int id);
bool host_render_is_enabled(int id);
bool host_render_is_retired(int id);
void host_render_set_enabled(int id, bool on);

// Runs the enabled, available stages and presents the result. Ports call this
// instead of host_video_present. With no stage to run it presents the caller's
// buffer untouched and copies nothing.
void host_render_present(const uint32_t *pixels, int width, int height);

#endif
