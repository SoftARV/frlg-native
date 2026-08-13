// See crash.h. The shape of this file is one rule: a signal handler may call
// almost nothing, so it does almost nothing.
//
// What runs inside the handler is the part that cannot be deferred -- the fault
// address, the frame, and a backtrace of the stack that is about to be unwound
// -- written with raw `write` to a descriptor opened before any of it happened.
// Everything else, including deciding what to tell the player, runs afterwards
// on a stack that was never damaged.

#define _GNU_SOURCE

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "agb/frame.h"
#include "host_session.h"

#include "crash.h"

// The BIOS occupies the bottom 16 KB on hardware, so a read through a null
// pointer there returns harmless nonsense instead of faulting. Nine of the nine
// crashes found by playing so far have been exactly that, which is worth saying
// in the report rather than leaving as an address to look up.
#define BIOS_REGION_END 0x4000

static volatile sig_atomic_t crashed;
static volatile sig_atomic_t handling;
static int report_fd = -1;
static char report_path[512];
static char summary[160];
static int caught_signal;
static unsigned long caught_address;
static unsigned caught_frame;

// ---------------------------------------------------------- safe output ---
//
// printf is not async-signal-safe and neither is snprintf in the strict
// reading, so the handler formats its own. These are the only three shapes it
// needs.

static void emit(const char *text)
{
    ssize_t ignored = write(report_fd, text, strlen(text));

    (void)ignored;
}

static void emit_dec(unsigned long value)
{
    char digits[24];
    int at = (int)sizeof(digits);

    if (value == 0)
        digits[--at] = '0';
    while (value != 0)
    {
        digits[--at] = (char)('0' + (value % 10));
        value /= 10;
    }
    {
        ssize_t ignored = write(report_fd, digits + at, sizeof(digits) - (size_t)at);

        (void)ignored;
    }
}

static void emit_hex(unsigned long value)
{
    static const char nibble[] = "0123456789ABCDEF";
    char out[8];

    for (int i = 7; i >= 0; i--)
    {
        out[i] = nibble[value & 0xF];
        value >>= 4;
    }
    emit("0x");
    {
        ssize_t ignored = write(report_fd, out, sizeof(out));

        (void)ignored;
    }
}

static const char *signal_name(int sig)
{
    switch (sig)
    {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default:      return "signal";
    }
}

// What to tell a person. The address decides it: the null region is a bug we
// already understand the shape of, and saying so turns a hex number into a
// sentence that means something to whoever reports it.
static const char *explain(int sig, unsigned long addr)
{
    if ((sig == SIGSEGV || sig == SIGBUS) && addr < BIOS_REGION_END)
        return "The game read or wrote through a pointer that was never set.\n"
               "On a Game Boy Advance that quietly returns nonsense and the game\n"
               "carries on; here it stops. This is the most common bug in the\n"
               "port so far, and the report has everything needed to fix it.";
    if (sig == SIGSEGV || sig == SIGBUS)
        return "The game touched memory that does not belong to it.";
    if (sig == SIGILL)
        return "The game tried to run something that is not code.";
    if (sig == SIGFPE)
        return "The game divided by zero, or something close to it.";
    return "The game stopped itself.";
}

static void on_fault(int sig, siginfo_t *info, void *context)
{
    (void)context;

    // A fault while reporting a fault would loop for ever. The first one is the
    // one worth having, so the second ends the process immediately.
    if (handling)
        _exit(3);
    handling = 1;
    crashed = 1;

    caught_signal = sig;
    caught_address = (unsigned long)(uintptr_t)(info != NULL ? info->si_addr : NULL);
    caught_frame = agb_frame_count();

    if (report_fd >= 0)
    {
        emit("frlg-native crash report\n\nsignal:  ");
        emit(signal_name(sig));
        emit("\naddress: ");
        emit_hex(caught_address);
        emit("\nframe:   ");
        emit_dec(caught_frame);
        emit("\n\n");
        emit(explain(sig, caught_address));
        emit("\n\nbacktrace (game thread):\n");
#if defined(__GLIBC__)
        {
            void *frames[32];
            int n = backtrace(frames, 32);

            backtrace_symbols_fd(frames, n, report_fd);
        }
#endif
        fsync(report_fd);
    }

    // Say it on the terminal too: a developer watching one is not going to go
    // looking for a file to find out what just happened.
    {
        int saved = report_fd;

        report_fd = 2;
        emit("\nfrlg-native: ");
        emit(signal_name(sig));
        emit(" at ");
        emit_hex(caught_address);
        emit(" on frame ");
        emit_dec(caught_frame);
        emit("\n");
        report_fd = saved;
    }

    // Back to agb_frame_run, which returns normally on a stack that was never
    // damaged. Everything unsafe waits until then.
    agb_frame_abort();

    // Only reached if the run had not started, so there was nothing to jump to.
    _exit(3);
}

void crash_install(void)
{
    // A fixed size on purpose: glibc made SIGSTKSZ a sysconf call, which is not
    // a constant expression and cannot size a static array. 64 KB is far more
    // than a handler that only formats a backtrace needs.
    static char altstack[65536];
    stack_t alt;
    struct sigaction sa;
    static const int caught[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

    // A stack overflow faults with no room left to run a handler on. Its own
    // stack is the difference between a report and a silent kill.
    alt.ss_sp = altstack;
    alt.ss_size = sizeof(altstack);
    alt.ss_flags = 0;
    sigaltstack(&alt, NULL);

    if (host_session_file("crash.txt", report_path, sizeof(report_path)) != NULL)
        report_fd = open(report_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    // glibc's backtrace loads its unwinder on first use, which is a thing to do
    // before the handler needs it rather than inside one.
#if defined(__GLIBC__)
    {
        void *warm[4];

        (void)backtrace(warm, 4);
    }
#endif

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (unsigned i = 0; i < sizeof(caught) / sizeof(caught[0]); i++)
        sigaction(caught[i], &sa, NULL);
}

int crash_happened(void)
{
    return crashed != 0;
}

const char *crash_summary(void)
{
    if (!crashed)
        return NULL;
    snprintf(summary, sizeof(summary), "%s at 0x%08lX on frame %u",
             signal_name(caught_signal), caught_address, caught_frame);
    return summary;
}

// A feature nobody can trigger is a feature nobody has tested. This is how the
// whole path -- handler, unwind, report, bundle -- gets exercised on demand.
void crash_test_tick(unsigned frame)
{
    static unsigned when;
    static int mode = -1;

    if (mode < 0)
    {
        const char *what = getenv("FRLG_CRASH_TEST");
        const char *at = getenv("FRLG_CRASH_FRAME");

        when = at != NULL ? (unsigned)strtoul(at, NULL, 0) : 300;
        mode = 0;
        if (what == NULL)
            return;
        if (strcmp(what, "segv") == 0)
            mode = 1;
        else if (strcmp(what, "bus") == 0)
            mode = 2;
        else if (strcmp(what, "abort") == 0)
            mode = 3;
    }

    if (mode == 0 || frame < when)
        return;

    fprintf(stderr, "frlg-native: FRLG_CRASH_TEST faulting on purpose\n");
    if (mode == 1)
    {
        // The same shape as the bug this port keeps finding: a read through a
        // pointer the game never set.
        volatile int *nowhere = (volatile int *)0x14;

        (void)*nowhere;
    }
    else if (mode == 2)
    {
        volatile int *misaligned = (volatile int *)0x1;

        (void)*misaligned;
    }
    abort();
}
