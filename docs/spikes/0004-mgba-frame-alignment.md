# Spike 0004 — Does the port match mGBA, and at which frame?

**Question:** the golden tier is only an oracle if a golden means "matches the ROM". mGBA running
our own reference build is that oracle. Two things had to be true before it could be used: that our
frame numbers mean the same as mGBA's, and that our renderer actually agrees with it.

**Verdict: the renderer agrees exactly, and the frame numbers do not.** mGBA runs 38 frames ahead
of us, and on the two settled screens the port matches it **pixel for pixel at that offset** — 0 of
38,400. On animated scenes no offset aligns everything, and the reason is the stubbed audio.

## The offset

mGBA boots through the BIOS and the ROM's `crt0` before reaching `AgbMain`; the port calls
`AgbMain` directly ([ARCHITECTURE §6.5](../ARCHITECTURE.md#65-interrupts-and-the-frame-loop)). The
game has therefore had 38 fewer frames of life at any given count on our side.

Found by sweeping, not assumed. Our frame 2400 against mGBA frames 2340–2460:

| mGBA frame | Pixels differing |
| --- | --- |
| 2436 | 1,375 |
| 2437 | 788 |
| **2438** | **0** |
| 2439 | 1,053 |
| 2400 (naive) | 2,707 |

A single frame either side is worse by hundreds of pixels, so the alignment is exact rather than
approximate. Frame 100 confirms it at `+38` as well.

## What still differs, and why it is not the renderer

Frames 400 and 900 never reach zero at any offset in a 200-frame window. The best alignments are
`+29` (564 pixels) and `+35` (607 pixels) — different offsets, which already rules out a constant
lag.

The residual is worth looking at rather than counting. On frame 900 every differing pixel belongs
to an **animated object**: the grass tuft and one edge of Gengar. Every background pixel matches,
across four layers, windows and blending. The animations are independently phased, so one offset
cannot align them all at once.

The cause is upstream of the renderer. Scene pacing diverges wherever the game waits on the audio
subsystem, which is a deferred stub until phase 4 — `IsPokemonCryPlaying` and the `m4a` calls
return immediately rather than after a sound. A transition timed by "wait until the cry finishes"
takes a different number of frames here than on hardware. The settled screens have no such wait
left in flight, which is exactly why they align and the mid-transition frames do not.

## What this settles

- **The PPU is correct**, and now demonstrably so rather than by assertion. Two full frames of real
  game output — 240×160, four background layers, objects, windows, blending, the lot — reproduce a
  known-good emulator exactly.
- **Goldens for settled screens come from mGBA**, with the reference frame recorded per frame in
  `tests/golden/manifest.txt`. Those are an oracle.
- **Goldens for mid-transition frames stay self-blessed** and are marked `-`. They catch
  regressions and claim nothing more.
- **Phase 4 should retire the exclusion.** Once the mixer runs, 400 and 900 are worth re-testing:
  if the pacing converges they gain reference frames, and if it does not, that is a finding about
  the mixer rather than the renderer.

## A note on method

The first comparison ran our frame N against mGBA's frame N and reported thousands of differing
pixels on three frames out of four. Read as a verdict that would have said the renderer was wrong
in several places. The only thing that distinguished "wrong" from "offset" was sweeping a range and
finding a zero, and the only thing that distinguished the remaining 607 pixels from a bug was
looking at where they were rather than how many.
