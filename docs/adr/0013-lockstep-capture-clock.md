# ADR 0013 — Captures run on a lockstep clock

**Status:** accepted

## Context

[ADR 0009](0009-preemptive-interrupts.md) delivers interrupts by signal because `AgbMain`'s loop ends
in a busy-wait on `gMain.intrCheck`, a plain memory flag that only the V-blank handler sets: there is
no yield point, so a cooperative switch cannot escape it. A periodic `SIGALRM` at 59.7275 Hz preempts
it. That is right for playing, and it is what makes the port feel like the machine.

It also means **a frame boundary lands wherever wall-clock time puts it**. If a frame's work overruns
the 16.743 ms tick, the game misses that V-blank and the run falls one frame behind a less loaded one.
Section 6.5 recorded this as an open problem from the start.

It stopped being theoretical when the affine layer began drawing. Composing a layer that had been
skipped costs real time per frame, which pushed some frames over the tick, and goldens blessed before
the fix no longer matched — not because a pixel changed, but because the capture arrived a frame
earlier or later. Six concurrent captures of one binary landed on mGBA frame 2437 five times and 2436
once; the same capture under `ctest` landed on 2438. **Every one of them matched some real mGBA frame
exactly**, which is the tell: the renderer was never in question, only which frame was being compared.

A zero-budget comparison against a fixed frame number cannot survive that. It had been passing on
luck.

## Decision

**`FRLG_LOCKSTEP` advances frames from the game's own idle point instead of from a timer**, and every
harness that captures sets it: the golden tier, the audio check, and trace replay.

The busy-wait ADR 0009 could not yield from is exactly where the game is provably doing nothing, so it
is also the one place a frame can be advanced without preempting work. `tools/strip_hardware_waits.py`
rewrites the empty spin in the preprocessed `main.c` to call `agb_frame_idle()`, which is nothing in a
real-time run and the whole frame boundary in lockstep. `VBlankIntrWait`, which already yielded, does
the same. No timer is armed in lockstep, because one that fired would put back the dependence being
removed.

ADR 0009 stands unchanged for playing. This is a second clock for capture, not a replacement.

## Consequences

A capture is now reproducible: six concurrent runs of the same binary produce byte-identical frames,
where before they produced three different ones. Goldens can be blessed on one machine and compared on
another, and the zero-pixel budget the tier is built on is finally honest.

Captures also stop waiting out real time. The golden tier fell from 85 s to 10 s, and a 2400-frame
capture from 40 s to 5 s, because nothing sleeps between frames.

**Lockstep is not for playing.** Nothing paces the game to real time, so it runs as fast as the host
allows and audio has no clock to follow. It is opt-in for that reason.

**It assumes the idle spin is the only place the game waits without calling us.** That is true of this
game today — `main.c`'s `WaitForVBlank` is the only bare spin in the built sources, and the seam fails
the build if it is not found. A future spin, in link code or a debug path, would hang instead of
advancing, and would need the same treatment. That is a loud failure, not a silent wrong answer.
