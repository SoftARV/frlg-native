// See host_session.h for what a session is and why every run keeps one.
//
// Backend-independent on purpose: the headless build records exactly what the
// windowed one does, so the whole path can be exercised without a display.

#define _GNU_SOURCE

#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "host_session.h"
#include "host_settings.h"

// How many runs are kept. Enough that a tester who plays a few times still has
// the one that broke, small enough that a save copy each is not a disk problem.
#define SESSIONS_KEPT 5

#define PATH_LEN 512

static char session_dir[PATH_LEN];
static int session_live;

// $XDG_DATA_HOME, then the fallback the spec names. Not a home-relative path of
// our own invention: a player's session data belongs where the rest of their
// application data is.
static int data_root(char *buf, size_t len)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");

    if (xdg != NULL && xdg[0] == '/')
        snprintf(buf, len, "%s/frlg-native", xdg);
    else if (home != NULL && home[0] == '/')
        snprintf(buf, len, "%s/.local/share/frlg-native", home);
    else
        return -1;
    return 0;
}

static int make_dir(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

int host_make_dir(const char *path)
{
    return make_dir(path);
}

static int make_dir_p(const char *path)
{
    char work[PATH_LEN];
    size_t n = strlen(path);

    if (n >= sizeof(work))
        return -1;
    memcpy(work, path, n + 1);

    for (char *p = work + 1; *p != '\0'; p++)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        if (make_dir(work) != 0)
            return -1;
        *p = '/';
    }
    return make_dir(work);
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *entry;
    char child[PATH_LEN];

    if (dir == NULL)
        return;
    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        // One level deep is all a session is; anything else is not ours to walk.
        if (unlink(child) != 0)
            remove_tree(child);
    }
    closedir(dir);
    rmdir(path);
}

// Names are timestamps, so sorting them lexically sorts them by age.
static void prune(const char *root)
{
    DIR *dir = opendir(root);
    struct dirent *entry;
    char *names[64];
    int count = 0;

    if (dir == NULL)
        return;
    while ((entry = readdir(dir)) != NULL && count < (int)(sizeof(names) / sizeof(names[0])))
    {
        if (entry->d_name[0] == '.')
            continue;
        names[count] = strdup(entry->d_name);
        if (names[count] != NULL)
            count++;
    }
    closedir(dir);

    qsort(names, (size_t)count, sizeof(names[0]), compare_names);
    for (int i = 0; i <= count - SESSIONS_KEPT; i++)
    {
        char victim[PATH_LEN];

        snprintf(victim, sizeof(victim), "%s/%s", root, names[i]);
        remove_tree(victim);
    }
    for (int i = 0; i < count; i++)
        free(names[i]);
}

int host_data_dir(char *buf, size_t len)
{
    if (data_root(buf, len) != 0)
        return -1;
    return make_dir_p(buf);
}

int host_session_open(void)
{
    char root[PATH_LEN];
    const char *off = getenv("FRLG_NO_RECORD");
    time_t now;
    struct tm parts;

    // The opt-out. Recording is the default while the port is in active
    // development, because a crash nobody can replay costs more than the disk
    // does (ADR 0016). FRLG_NO_RECORD is what the harnesses set and it wins;
    // the setting is what a person chose in the launcher.
    if (off != NULL && off[0] != '\0' && off[0] != '0')
        return -1;
    if (off == NULL && !host_setting_bool("record-sessions", 1))
        return -1;

    if (data_root(root, sizeof(root)) != 0)
        return -1;

    snprintf(session_dir, sizeof(session_dir), "%s/sessions", root);
    if (make_dir_p(session_dir) != 0)
    {
        fprintf(stderr, "frlg-native: no session directory (%s): %s\n",
                session_dir, strerror(errno));
        session_dir[0] = '\0';
        return -1;
    }
    prune(session_dir);

    now = time(NULL);
    localtime_r(&now, &parts);
    {
        size_t used = strlen(session_dir);

        strftime(session_dir + used, sizeof(session_dir) - used,
                 "/%Y-%m-%d-%H%M%S", &parts);
    }

    // A second run inside the same second is rare and not worth a loop: the
    // suffix makes it unambiguous when it happens.
    if (make_dir(session_dir) != 0)
    {
        size_t used = strlen(session_dir);

        snprintf(session_dir + used, sizeof(session_dir) - used, "-%d", (int)getpid());
        if (make_dir(session_dir) != 0)
        {
            session_dir[0] = '\0';
            return -1;
        }
    }

    session_live = 1;
    return 0;
}

const char *host_session_dir(void)
{
    return session_live ? session_dir : NULL;
}

const char *host_session_file(const char *name, char *buf, size_t len)
{
    if (!session_live)
        return NULL;
    snprintf(buf, len, "%s/%s", session_dir, name);
    return buf;
}

void host_session_keep(const char *path, const char *name)
{
    char dest[PATH_LEN];
    FILE *in, *out;
    char block[8192];
    size_t got;

    if (!session_live || path == NULL)
        return;

    in = fopen(path, "rb");
    if (in == NULL)
        return;   // Nothing to keep is normal: a first run has no save yet.

    snprintf(dest, sizeof(dest), "%s/%s", session_dir, name);
    out = fopen(dest, "wb");
    if (out == NULL)
    {
        fclose(in);
        return;
    }
    while ((got = fread(block, 1, sizeof(block), in)) > 0)
        fwrite(block, 1, got, out);
    fclose(in);
    fclose(out);
}

// A tester pasting a terminal is how reports lose the first half, so the session
// keeps its own copy. Both streams are spliced, not just stderr: the port says
// what it loaded and what rate it opened on stdout and complains on stderr, and
// a log holding one without the other reads as though half the run is missing.
//
// Each stream keeps its own pipe and pump so the terminal still behaves --
// stdout stays stdout, and a caller redirecting one of them is unaffected --
// and both pumps append to the one log, which keeps the order a reader expects.
struct stream_capture
{
    int fd;              // the descriptor being spliced: 1 or 2
    int pipe_fds[2];
    int original;
    pthread_t thread;
};

static struct stream_capture captures[2];
static int log_file = -1;
static int log_running;

static void *log_pump(void *arg)
{
    struct stream_capture *cap = arg;
    char block[4096];
    ssize_t got;

    while ((got = read(cap->pipe_fds[0], block, sizeof(block))) > 0)
    {
        ssize_t ignored;

        ignored = write(cap->original, block, (size_t)got);
        ignored = write(log_file, block, (size_t)got);
        (void)ignored;
    }
    return NULL;
}

static int capture_stream(struct stream_capture *cap, int fd)
{
    cap->fd = fd;
    cap->original = dup(fd);
    if (cap->original < 0 || pipe(cap->pipe_fds) != 0)
        return -1;
    if (pthread_create(&cap->thread, NULL, log_pump, cap) != 0)
        return -1;
    dup2(cap->pipe_fds[1], fd);
    return 0;
}

void host_session_capture_log(void)
{
    char path[PATH_LEN];

    if (!session_live || log_running)
        return;

    if (host_session_file("session.log", path, sizeof(path)) == NULL)
        return;

    log_file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_file < 0)
        return;

    // Unbuffered, so a crash cannot leave the last few lines -- the ones that
    // matter -- sitting in a buffer nothing will flush. stdout keeps the line
    // buffering main() asked for, which survives the descriptor swap.
    setvbuf(stderr, NULL, _IONBF, 0);

    if (capture_stream(&captures[0], 1) != 0 || capture_stream(&captures[1], 2) != 0)
        return;
    log_running = 1;
}

void host_session_close(void)
{
    if (!log_running)
        return;

    fflush(stdout);
    fflush(stderr);
    log_running = 0;

    // Put the real descriptors back before draining, so anything said from here
    // on still reaches the terminal rather than a pipe with no reader left.
    for (unsigned i = 0; i < 2; i++)
    {
        struct stream_capture *cap = &captures[i];

        dup2(cap->original, cap->fd);
        close(cap->pipe_fds[1]);
        pthread_join(cap->thread, NULL);
        close(cap->pipe_fds[0]);
    }
    close(log_file);
    log_file = -1;
}
