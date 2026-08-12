# ADR 0014 — Lockstep keeps a stall watchdog

**Status:** accepted — refines [ADR 0013](0013-lockstep-capture-clock.md)

## Context

ADR 0013 advances frames from the game's V-blank spin and arms no timer, and closed with a stated
consequence: a wait the game performs *without* reaching that spin would hang, and that this would be
"a loud failure, not a silent wrong answer". It was loud. It was also reached within an hour, by the
first play-through recorded on the new clock, at `overworld.c:2045`:

```c
static void DoMapLoadLoop(u8 *state)
{
    while (!LoadMapInStepsLocal(state, FALSE)) ;
}
```

The map-load state machine is run to completion by a bare spin, and one of its steps waits on the DMA3
manager, whose queue only drains in the V-blank handler. On hardware the interrupt preempts the spin.
In lockstep nothing did, and the game stopped for good at the first map load — which is every
play-through past the intro.

Rewriting that loop through the seam would work, and would work again for the next one. There are a
dozen bare spins in the built sources; most are state machines that need no interrupt at all, and
telling those apart from the ones that do means reading each and being right about it. Being wrong in
one direction hangs, and in the other advances frames that should not exist.

## Decision

**The timer stays armed in lockstep, as a watchdog rather than as the clock.** It is re-armed three
frame periods out after every frame, so it fires only where the game has gone that long without
reaching its idle point — and when it does, it advances the frame the game is waiting for, exactly as
the real-time clock would.

Firings are counted, and the port reports the count when it is not zero.

## Consequences

The hang class is gone without enumerating the loops that could cause it, and a run that never needs
the watchdog says so by reporting nothing.

**Determinism survives, for the same reason lockstep works at all.** A loop that spins is a loop doing
nothing observable, so it does not matter when the frame lands inside it. Measured: three concurrent
replays across a map load — one watchdog firing each — produce byte-identical frames, and the intro
capture is unchanged to the byte from before the watchdog existed.

**The count is the honesty.** A firing during real work *would* be wall-clock dependent, so the number
is reported rather than hidden. Zero means the run was driven entirely by the game; a small stable
number means a spin needed that many V-blanks; a number that moves between runs on the same input
means the three-period budget is too tight for something, and that is a bug to look at rather than a
threshold to raise.

**Frame cost matches hardware more closely than the seam would have.** Rewriting the loop to call the
idle hook would advance a frame per *iteration* — twenty-odd for a map load where hardware spends two
or three. The watchdog advances one per *stuck V-blank*, which is what the hardware loses. It costs
wall-clock time in exchange: three frame periods per stuck V-blank, about 50 ms, once per map load.
