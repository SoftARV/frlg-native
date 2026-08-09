#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <execinfo.h>
#endif

#include "agb/frame.h"

extern void AgbMain(void);

// The game spins on hardware registers in several places. Without the real
// hardware behind them a stall looks identical to a hang, so the watchdog
// reports where it stopped instead of leaving it to a timeout.
static void on_stall(int sig)
{
    const char stalled[] = "\nfrlg-native: stalled, backtrace follows\n";
    const char faulted[] = "\nfrlg-native: faulted, backtrace follows\n";
    const char *msg = sig == SIGALRM ? stalled : faulted;
    size_t len = sig == SIGALRM ? sizeof(stalled) - 1 : sizeof(faulted) - 1;
    ssize_t ignored = write(2, msg, len);
    (void)ignored;

#if defined(__GLIBC__)
    void *frames[24];
    int n = backtrace(frames, 24);
    backtrace_symbols_fd(frames, n, 2);
#endif
    _exit(3);
}

int main(int argc, char **argv)
{
    uint32_t limit = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 60;
    unsigned watchdog = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 10;
    uint32_t frames;

    setvbuf(stdout, NULL, _IOLBF, 0);

    signal(SIGALRM, on_stall);
    signal(SIGSEGV, on_stall);
    signal(SIGBUS, on_stall);
    alarm(watchdog);

    printf("frlg-native: entering AgbMain, stopping after %u frames\n", limit);
    frames = agb_frame_run(AgbMain, limit);
    alarm(0);
    printf("frlg-native: ran %u frames\n", frames);

    return frames == 0;
}
