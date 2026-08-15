#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "host_render.h"

struct slot
{
    struct host_render_stage stage;
    bool used;
    bool enabled;
    bool retired;
};

static struct slot slots[HOST_RENDER_MAX_STAGES];

// Grown to fit whatever it is asked to hold. Two buffers rather than one: the
// scratch is what stages transform, and the backup is what a failing stage is
// rolled back to, which is what "falls back to vanilla" has to mean when a
// stage can fail halfway through writing.
static uint32_t *scratch;
static uint32_t *backup;
static size_t capacity;

static bool reserve(size_t pixels)
{
    uint32_t *a, *b;

    if (capacity >= pixels)
        return true;

    a = realloc(scratch, pixels * sizeof(*a));
    if (a == NULL)
        return false;
    scratch = a;

    b = realloc(backup, pixels * sizeof(*b));
    if (b == NULL)
        return false;
    backup = b;

    capacity = pixels;
    return true;
}

int host_render_register(const struct host_render_stage *stage)
{
    int i;

    if (stage == NULL || stage->run == NULL || stage->name == NULL)
        return -1;

    for (i = 0; i < HOST_RENDER_MAX_STAGES; i++)
    {
        if (slots[i].used)
            continue;
        slots[i].stage = *stage;
        slots[i].used = true;
        slots[i].enabled = true;
        slots[i].retired = false;
        return i;
    }
    return -1;
}

void host_render_unregister(int id)
{
    if (id < 0 || id >= HOST_RENDER_MAX_STAGES)
        return;
    memset(&slots[id], 0, sizeof(slots[id]));
}

int host_render_count(void)
{
    return HOST_RENDER_MAX_STAGES;
}

const char *host_render_name(int id)
{
    if (id < 0 || id >= HOST_RENDER_MAX_STAGES || !slots[id].used)
        return NULL;
    return slots[id].stage.name;
}

bool host_render_is_enabled(int id)
{
    if (id < 0 || id >= HOST_RENDER_MAX_STAGES || !slots[id].used)
        return false;
    return slots[id].enabled;
}

bool host_render_is_retired(int id)
{
    if (id < 0 || id >= HOST_RENDER_MAX_STAGES || !slots[id].used)
        return false;
    return slots[id].retired;
}

void host_render_set_enabled(int id, bool on)
{
    if (id < 0 || id >= HOST_RENDER_MAX_STAGES || !slots[id].used)
        return;
    // A retired stage stays retired: it failed once, and turning it back on
    // from a menu would hand the same broken frame to the player again. It
    // comes back by being registered again, which is a deliberate act.
    if (on && slots[id].retired)
        return;
    slots[id].enabled = on;
}

static bool runnable(const struct slot *s)
{
    if (!s->used || !s->enabled || s->retired)
        return false;
    if (s->stage.available != NULL && !s->stage.available(s->stage.user))
        return false;
    return true;
}

void host_render_present(const uint32_t *pixels, int width, int height)
{
    size_t count = (size_t)width * (size_t)height;
    bool started = false;
    int i;

    if (pixels == NULL || width <= 0 || height <= 0)
        return;

    for (i = 0; i < HOST_RENDER_MAX_STAGES; i++)
    {
        if (!runnable(&slots[i]))
            continue;

        // Nothing is copied until there is a stage that will actually run, so
        // the common case -- no display pipeline at all -- costs one pass over
        // the slot table and no memory traffic.
        if (!started)
        {
            if (!reserve(count))
                break;
            memcpy(scratch, pixels, count * sizeof(*scratch));
            started = true;
        }

        memcpy(backup, scratch, count * sizeof(*backup));
        if (!slots[i].stage.run(scratch, width, height, slots[i].stage.user))
        {
            char line[128];

            memcpy(scratch, backup, count * sizeof(*scratch));
            // Off as well as retired, so a settings screen showing what is on
            // is telling the truth. Retirement is what makes it permanent.
            slots[i].retired = true;
            slots[i].enabled = false;
            snprintf(line, sizeof(line),
                     "render stage '%s' failed and was retired",
                     slots[i].stage.name);
            host_log(line);
        }
    }

    host_video_present(started ? scratch : pixels, width, height);
}
