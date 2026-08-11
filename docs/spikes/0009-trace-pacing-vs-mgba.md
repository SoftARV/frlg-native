# Spike 0009 — The same trace does not replay the same on the reference

**Question:** replaying one input trace, the port and mGBA end up in different places — at our frame
1500 the speech is two lines behind. Both are deterministic and both get the same keys. Where does the
difference come from?

**Status: answered.** The port is faster than the machine. mGBA emulates an ARM7 at 16.78 MHz, and
whenever a frame's work overruns 16.743 ms the game misses that V-blank; the port does the same work in
microseconds and never misses one. Over the intro, hardware loses **73 V-blanks** that the port does
not, so the same trace lands on different scenes.

This is not a defect to fix. It is a consequence of recompiling rather than emulating, and it bounds
what the oracle can be asked.

## Ruling out the plumbing first

Both sides act on the trace at the same frame. `mgba-capture` can now read `gMain` out of the reference
build — its address is in `pokefirered_modern.map` — so the two can be asked the same question:

| | our frame | KEYINPUT | `gMain.heldKeys` | `gMain.newKeys` |
| --- | --- | --- | --- | --- |
| port | 1016 → 1017 | `03FE` | `0001` | `0001` |
| mGBA | 1054 → 1055 | `03FE` | `0001` | `0001` |

Identical, at exactly the +38 boot offset. The input path is not it — and the frame after that press,
our text box opens and mGBA's does not, so the two games were already in different states while
drawing the same pixels.

## Where they part

Walking the trace and comparing at the boot offset, the port and mGBA are pixel-identical up to our
frame 365 and differ from 366. The animation that follows is *identical in shape* on both sides — the
same brightness sequence, `10.8, 39.1, 67.1, 95.4, 109.7` — just run 16 frames earlier here. Neither
side is animating at the wrong rate. One of them is simply doing more per frame.

The screen at that moment is the controls guide, which decompresses and draws a page of text.

## The measurement

FRLG's wait is not "wait until the next V-blank" but "clear the flag, then wait for the next one":

```c
static void WaitForVBlank(void)
{
    gMain.intrCheck &= ~INTR_FLAG_VBLANK;
    while (!(gMain.intrCheck & INTR_FLAG_VBLANK))
        ;
}
```

So a frame the game is still working through is a frame it **loses**: the V-blank arrives, the handler
sets the flag, and the game — which has not reached its wait — clears the flag on arrival and waits for
the following one. It never catches up; the scene simply takes an extra frame.

That makes the flag a probe. Sampled at each frame boundary, `gMain.intrCheck & 1` set means the game
had not reached its wait, so that V-blank is lost. Over our frames 340–460:

- **20 late V-blanks in mGBA** (frames 401–417 and 430–432)
- **20 frames of drift** measured from the pixels over the same window (offset 38 → 58)

The first divergence, our 366, is mGBA 404 — inside the first stretch.

Over the whole intro, mGBA loses **73 V-blanks in 11 stretches**, the longest 17 frames: about 6.6% of
the intro is hardware running behind. The port loses none.

## What follows

- **No single offset relates the two across a heavy scene.** The +38 boot offset holds only while both
  keep up. The golden manifest already carries a reference number per frame rather than deriving one,
  which is now a requirement rather than a convenience, and goldens want settled screens — on a moving
  one, a frame number means two different things on the two machines.
- **A trace is portable within the port, not to the reference.** Replay stays exact here (lockstep,
  [ADR 0013](../adr/0013-lockstep-capture-clock.md)); the same trace on mGBA diverges by construction.
- **The intro plays slightly faster here than on hardware** — a second or so across the opening. Every
  animation runs at the right rate; there are just no dropped frames to pad it.

Matching hardware would mean a cycle model: counting instruction and memory-access cycles per frame and
withholding the V-blank when the budget is spent. That is a different project from a source port, and
nothing on the roadmap needs it. It is worth revisiting only if link play arrives, where two machines
must agree frame for frame.

## The instrument

`FRLG_DUMP_KEYS=FIRST:LAST` on `mgba-capture` prints, per frame, the key register and what the game
made of it, plus `intrCheck`. Reading the reference's *own variables* rather than its pixels is what
settled both this and [spike 0008](0008-missing-trainer-pics.md), and the pattern generalises: any
question about what the game thinks can be put to the reference through its symbol map.
