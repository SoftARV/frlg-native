# Spike 0008 — The trainer pictures do not appear

**Question:** Oak standing in the opening speech is missing, and so is the large player picture on the
gender screen, while Oak's Nidorina animation on the same screens is fine. Where does it go wrong?

**Status: located to one register, not yet explained.** Everything the picture needs is present and
correct except the affine transform, which is all zeros — and the game never sets it, which means the
attribution is still wrong somewhere. Recorded because the next step is a measurement, not a guess,
and because the route to it is now reusable.

## What made it investigable

Reaching Oak's speech takes playing, so none of this was reachable before input traces existed.
`FRLG_INPUT` replays one deterministically, and `mgba-capture` now accepts the same trace — shifted by
the +38 frame boot offset — so the oracle works for moments that need input, not only for the intro
frames the golden tier already covers.

A trace that mashes A every 32 frames walks the intro forward on its own, which is enough to reach the
speech without recording a human playing it.

## What is right

At our frame 1500, showing *"This world"* over an empty platform where mGBA shows Oak:

| | |
| --- | --- |
| `LoadTrainerPic(whichPic=3)` | called, at frame 873 |
| `LZ77UnCompVram` destination | `vram + 0x600` — correct |
| decompressed size | 6144 bytes, header `10 00 18 00` |
| Oak's tiles still in VRAM at frame 1500 | 2340 non-zero bytes of 6144 |
| BG palette 6 | loaded, real colours |
| BG2's tilemap at `0xE000` | 96 non-zero byte entries — a 64×96 picture |
| `DISPCNT` | `1741` — mode 1, BG0/BG1/BG2 and objects enabled |
| `BG2CNT` | `5C81` — 8bpp, char base 0, screen base `0xE000`, priority 1 |

The pictures are `pic.8bpp.lz`, and BG2 is the only layer with 8bpp set, so they belong to BG2.

## What is wrong

```
BG2 affine params: PA=0000 PB=0000 PC=0000 PD=0000  X=00000000 Y=00000000
```

In mode 1 BG2 is an affine layer, and with `PA` zero the texture coordinate never advances: every
pixel on the line samples texel (0, 0), which is transparent, so the layer draws nothing. That is
consistent with what is on screen.

## Why that is not yet the answer

**The game never calls `SetBgAffine`** on this screen — a breakpoint on it and on `bg.c:269`, where the
matrix is written, catches nothing across the whole run. So the matrix is zero because nothing set it,
and an all-zero matrix would draw nothing on hardware either. Yet mGBA, running the same ROM along the
same trace, shows Oak.

So one of these is true, and the measurement below distinguishes them:

- Oak is **not** on BG2, and the 8bpp/char-base reasoning above is a coincidence.
- mGBA's registers at that moment are **not** what ours are, and the difference is upstream of the
  affine layer entirely — a mode, an enable, or a priority.

## The next measurement

`mgba-capture` links libmgba, which exposes `busRead16`. Reading mGBA's `DISPCNT`, `BG0-3CNT` and
BG2's affine parameters at the frame it draws Oak, and diffing them against ours, says which of the
two it is without further reasoning. The oracle has been used on pixels and on audio; this is the same
idea applied to registers, and it is the cheapest way to settle an attribution question.

**Also unexplained:** our run and mGBA's diverge in how far the same A-mashing trace advances the
dialogue — at frame 1500 ours is two lines behind. Harmless for this investigation, since both reach
the speech, but it means frame numbers are not directly comparable between the two and a golden
capture of this screen cannot use a fixed offset. Worth understanding before the determinism harness
leans on one.
