// Settings a person chooses, kept where the platform keeps settings.
//
// The launcher writes this file and the port reads it. It is a key file --
// GLib's format, so the launcher's side is three lines -- and the port's reader
// is deliberately dim: it ignores group headers and takes any `key=value` it
// recognises, because there is one group and guessing at more structure would
// be inventing a format nobody asked for.
//
// **The environment still wins.** Every setting here has an FRLG_ variable that
// came first, and the harnesses, the traces and CI all set them. A setting that
// could override those would make a recorded run depend on what somebody had
// clicked, which is exactly what the lockstep clock exists to prevent.

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "host_settings.h"

#define PATH_LEN 512
#define FILE_MAX 8192

static char contents[FILE_MAX];
static int loaded;

static int config_root(char *buf, size_t len)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (xdg != NULL && xdg[0] == '/')
        snprintf(buf, len, "%s/frlg-native", xdg);
    else if (home != NULL && home[0] == '/')
        snprintf(buf, len, "%s/.config/frlg-native", home);
    else
        return -1;
    return 0;
}

int host_config_dir(char *buf, size_t len)
{
    if (config_root(buf, len) != 0)
        return -1;
    if (mkdir(buf, 0755) != 0)
    {
        struct stat st;

        if (stat(buf, &st) != 0)
            return -1;
    }
    return 0;
}

static void load_once(void)
{
    char path[PATH_LEN];
    FILE *fh;

    if (loaded)
        return;
    loaded = 1;

    if (config_root(path, sizeof(path)) != 0)
        return;
    strncat(path, "/settings.ini", sizeof(path) - strlen(path) - 1);

    fh = fopen(path, "rb");
    if (fh == NULL)
        return;   // No file is every default, which is the common case.
    contents[fread(contents, 1, sizeof(contents) - 1, fh)] = '\0';
    fclose(fh);
}

// The value for `key`, or NULL. Matches at the start of a line so that a key
// cannot be found inside another key's value.
static const char *lookup(const char *key, char *buf, size_t len)
{
    size_t key_len = strlen(key);
    const char *at = contents;

    load_once();
    while ((at = strstr(at, key)) != NULL)
    {
        const char *value;

        if ((at != contents && at[-1] != '\n') || at[key_len] != '=')
        {
            at += key_len;
            continue;
        }

        value = at + key_len + 1;
        {
            size_t n = strcspn(value, "\r\n");

            if (n >= len)
                n = len - 1;
            memcpy(buf, value, n);
            buf[n] = '\0';
        }
        return buf;
    }
    return NULL;
}

int host_setting_bool(const char *key, int fallback)
{
    char value[32];

    if (lookup(key, value, sizeof(value)) == NULL)
        return fallback;
    return strcmp(value, "true") == 0 || strcmp(value, "1") == 0;
}

int host_setting_int(const char *key, int fallback)
{
    char value[32];

    if (lookup(key, value, sizeof(value)) == NULL)
        return fallback;
    return (int)strtol(value, NULL, 10);
}
