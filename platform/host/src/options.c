// Per-save port options, kept beside the save rather than inside it.
//
// The file is `<save>.port.ini`: tied to one save file, so it is per profile
// without needing to know what a profile is, and obvious in a directory listing
// next to the save it belongs to.
//
// Same dim key=value reader as settings.c, for the same reason -- there is one
// group and inventing more structure would be inventing a format nobody asked
// for. The writer emits what it holds, so a file round-trips through the port
// without accumulating anything.

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_options.h"

#define PATH_LEN 512
#define MAX_KEYS 32
#define KEY_LEN 48

struct entry
{
    char key[KEY_LEN];
    int value;
};

static struct entry entries[MAX_KEYS];
static int count;
static char file_path[PATH_LEN];
static int dirty;

static void parse(const char *text)
{
    const char *line = text;

    while (line != NULL && *line != '\0')
    {
        const char *end = strchr(line, '\n');
        const char *eq = strchr(line, '=');
        size_t len = end ? (size_t)(end - line) : strlen(line);

        if (eq != NULL && (end == NULL || eq < end) && line[0] != '#'
            && line[0] != '[')
        {
            size_t klen = (size_t)(eq - line);

            if (klen > 0 && klen < KEY_LEN && count < MAX_KEYS)
            {
                memcpy(entries[count].key, line, klen);
                entries[count].key[klen] = '\0';
                entries[count].value = atoi(eq + 1);
                count++;
            }
        }
        (void)len;
        line = end ? end + 1 : NULL;
    }
}

void host_options_open(const char *save_path)
{
    char buf[4096];
    FILE *fh;
    size_t got;

    count = 0;
    dirty = 0;
    file_path[0] = '\0';

    if (save_path == NULL || save_path[0] == '\0')
        return;

    if ((size_t)snprintf(file_path, sizeof(file_path), "%s.port.ini",
                         save_path) >= sizeof(file_path))
    {
        file_path[0] = '\0';
        return;
    }

    fh = fopen(file_path, "rb");
    if (fh == NULL)
        return;
    got = fread(buf, 1, sizeof(buf) - 1, fh);
    fclose(fh);
    buf[got] = '\0';
    parse(buf);
}

static struct entry *find(const char *key)
{
    int i;

    for (i = 0; i < count; i++)
    {
        if (strcmp(entries[i].key, key) == 0)
            return &entries[i];
    }
    return NULL;
}

int host_option_get(const char *key, int fallback)
{
    const struct entry *e;

    if (key == NULL)
        return fallback;
    e = find(key);
    return e ? e->value : fallback;
}

void host_option_set(const char *key, int value)
{
    struct entry *e;

    if (key == NULL || key[0] == '\0' || strlen(key) >= KEY_LEN)
        return;

    e = find(key);
    if (e != NULL)
    {
        if (e->value != value)
        {
            e->value = value;
            dirty = 1;
        }
        return;
    }

    if (count >= MAX_KEYS)
        return;
    snprintf(entries[count].key, KEY_LEN, "%s", key);
    entries[count].value = value;
    count++;
    dirty = 1;
}

int host_options_flush(void)
{
    FILE *fh;
    int i;

    if (!dirty)
        return 0;
    if (file_path[0] == '\0')
        return -1;

    fh = fopen(file_path, "wb");
    if (fh == NULL)
        return -1;

    fprintf(fh, "[port]\n");
    for (i = 0; i < count; i++)
        fprintf(fh, "%s=%d\n", entries[i].key, entries[i].value);

    if (fclose(fh) != 0)
        return -1;
    dirty = 0;
    return 0;
}
