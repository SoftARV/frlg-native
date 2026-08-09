# ADR 0009 — Interrupts preempt; fibers cannot deliver them

**Status:** accepted
**Supersedes:** the delivery mechanism in [ADR 0004](0004-fiber-frame-loop.md). The analysis of
*why* a per-frame call fails still stands; the fiber answer does not.

## Context

ADR 0004 put the game on a fiber so the host could switch to it when hardware would have raised an
interrupt. That reasoning held for `VBlankIntrWait`, which blocks by calling into us and can
therefore yield cooperatively.

It does not hold for the main loop. `AgbMain` ends every iteration in:

```c
static void WaitForVBlank(void)
{
    gMain.intrCheck &= ~INTR_FLAG_VBLANK;
    while (!(gMain.intrCheck & INTR_FLAG_VBLANK))
        ;
}
```

That is a busy-wait on a plain memory flag which only the V-blank handler sets. It calls nothing.
**A cooperative switch can never escape it**, because there is no yield point to switch at. Fibers
are the wrong primitive: the game does not wait to be resumed, it waits to be *interrupted*.

## Decision

Deliver interrupts by signal. A periodic timer at the GBA's true 59.7275 Hz raises `SIGALRM`, and
the handler dispatches through the game's own `gIntrTable`, honouring `REG_IME` and `REG_IE`
exactly as the BIOS vector would.

A POSIX signal handler runs **on the interrupted context's own stack**, which is precisely how a
hardware interrupt behaves: game code is paused, not run alongside. That property is what makes
this faithful rather than merely expedient, and it is why the concurrency objection ADR 0004 raised
against threads does not apply here — there is still exactly one thread and no shared-state race.

## Consequences

- The main loop runs. Measured: 1200 frames in 20.10 s against an expected 20.09 s, 0.05% drift.
- Handler code must be async-signal-safe, because it can interrupt anything. The deferred-subsystem
  reporter uses `write()` rather than `stdio` for exactly this reason: taking the stdio lock inside
  a handler while the interrupted context already held it is a deadlock.
- `SIGALRM` now belongs to the frame driver, so the desktop watchdog moved to `ITIMER_VIRTUAL`.
  A spin still burns CPU time, so a stall still trips it.
- `VBlankIntrWait` remains cooperative — it waits for the frame counter to advance — so both wait
  styles the game uses are served by one mechanism.

## The cost: determinism is no longer free

ADR 0004 claimed frame-for-frame reproducible headless runs, and that claim does not survive.
Interrupts now arrive on wall-clock time, so the *point in game code* at which a frame boundary
lands varies between runs.

The mitigating detail is that the game spends the wait doing nothing: state is identical whether
the loop spins ten times or ten thousand, so a boundary that lands inside the spin is
deterministic in effect. A boundary that lands mid-callback is not.

This is an **open problem for the phase 6 determinism harness**
([ADR 0008](0008-testing-strategy.md)), not a solved one. The likely answer is a headless mode
that advances a virtual clock only at known-safe points rather than on a real timer. Recording it
here so the harness is not designed on a false assumption.

## Alternatives rejected

**Fibers alone** — cannot escape a busy-wait, as above.

**Game on a thread, handlers on another** — genuinely concurrent, so handler and game code race on
game state. Hardware preempts; it does not run two contexts at once.

**Overriding `main.c` to replace `WaitForVBlank`** — would sidestep the busy-wait, but the same
pattern appears elsewhere in the game, and it would trade a general mechanism for a growing list of
per-file forks.
