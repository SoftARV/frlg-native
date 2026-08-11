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

## What the registers said

`mgba-capture` now dumps them, so the reference can be asked directly. At the frame mGBA draws Oak:

| | mGBA | ours |
| --- | --- | --- |
| `DISPCNT` | `1741` | `1741` |
| `BG0CNT` | `1F08` | `1F08` |
| `BG1CNT` | `1E02` | `1E02` |
| `BG2CNT` | `5C81` | `5C81` |
| `BG3CNT` | `1C0F` | `1C0F` |

**Identical.** So the mode, the enables, the priorities, the character and screen bases and the colour
depths all agree, and the difference is not in the display configuration.

BG2's affine parameters cannot be compared this way: they are write-only on hardware, and mGBA returns
open bus for them — every one reads back as `421D`, the same value, which is the giveaway.

So the attribution question is still open, and the remaining possibilities have narrowed to two: either
our affine renderer is wrong for a case this screen hits, or the picture is drawn by something other
than BG2 and the 8bpp coincidence misled the whole line of reasoning. Forcing an identity matrix in our
renderer and re-capturing would separate them, and is the next thing to try.

**Not attempted yet**, because the attempt ran into an unrelated crash that had to be dealt with
first — see below.

**Also unexplained:** our run and mGBA's diverge in how far the same A-mashing trace advances the
dialogue — at frame 1500 ours is two lines behind. Harmless for this investigation, since both reach
the speech, but it means frame numbers are not directly comparable between the two and a golden
capture of this screen cannot use a fixed offset. Worth understanding before the determinism harness
leans on one.


## An unrelated crash found on the way

Restoring a save so the traced run would pick NEW GAME instead of CONTINUE was a mistake that turned
out to be useful: it selected CONTINUE, and continuing a saved game crashed in every build.

```
LoadSaveblockObjEventScripts () at src/overworld.c:444
  gMapHeader.events = 0x8f659f0   (a valid cart address)
  objectEventCount  = 0
  objectEvents      = (nil)
  fault address     = 0x10
```

A map with no object events has a **legitimately null** `objectEvents`, and the loop copying their
scripts ignores the count and reads sixty-four entries regardless. On hardware that reads the BIOS
region and copies garbage into templates nothing goes on to use, because the count is zero. Here it
faulted, and continuing a saved game could not get past it.

This is the **third instance** of the no-MMU class, and the clearest: nothing is wrong with the pointer,
the data or our relocation — the code simply reads where it should not and the hardware does not mind.
Guarding the loop leaves those templates holding whatever they held rather than holding garbage; both
are unused and neither is read.
