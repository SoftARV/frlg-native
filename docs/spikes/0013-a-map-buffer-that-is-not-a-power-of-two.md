# Spike 0013 — A map buffer that is not a power of two

**Question:** the field's map layers are read from the game's own buffer now
([ADR 0024](../adr/0024-a-machine-wider-than-the-hardware.md)), so their size is ours to choose. A
576-pixel view wants 40 metatiles of map behind it, which is 80 tiles and 640 pixels. What breaks?

**Status: three faults found and fixed, and the width still is not right.** The buffer is 40
metatiles and could feed 576 pixels; the ceiling stays at 464, because past that the outer columns
smear vertically — content dragged down the picture, reported from play and confirmed in a
screenshot.

## What the width was supposed to buy

640 pixels of buffer against a 576-pixel view leaves 24 pixels of spare room on the left and 40 on
the right — a metatile either side, which is what the camera's rotation needs. The heap cost is
10 KB a layer against 8, and there is 112 KB.

The renderer needed one change for it: it wrapped a handed-over background by masking, which requires
a power of two. That is now a bounded subtract, which does not.

## Fault one: a modulus that does not divide the type

`tilemap_move_something` steps the camera's tile offset and wraps it:

```c
cameraOffset->xTileOffset += b;      // a u8, so this wraps at 256
cameraOffset->xTileOffset %= TILEMAP_W;
```

`b` is a `u32` holding a small negative number when the camera moves left or up, which is how
upstream passes it. The `u8` wraps at 256 and the modulus is taken afterwards, so the two only agree
when **the modulus divides 256**. 32 does, and 64 does, which is why this was correct for as long as
the buffer was one of the hardware's own sizes. 80 does not: a step back from zero landed at 14
instead of 78.

Fixed, by stepping in signed arithmetic before the wrap. It is worth keeping at 64 even though
nothing there can see it, because the next size that is not a power of two will.

## Fault two: a line the change did not reach

With that fixed, a native 240x160 frame still differed — 26 of 249 sampled, down from 110. Always
**the rightmost metatile column of the visible window**, and the region grew leftward as the player
walked: a column written wrong once and then scrolling in.

The whole-map fill wraps the buffer column it is about to write:

```c
temp = sFieldCameraOffset.xTileOffset + j;
if (temp >= 64)          // TILEMAP_W everywhere else
    temp -= 64;
```

Every other wrap in the file had been turned into `TILEMAP_W`. This one had not, because it sits in a
nested loop and is indented four spaces further than its siblings — and the edit that changed the
others matched on text including the indentation, and silently changed nothing here.

So the fill scattered columns and the leading-edge redraws corrected them one per step, which is
exactly why the wrong region shrank from the right as the player walked. At 64 tiles the stray
constant was the right answer, which is why it survived every measurement until the buffer changed
width.

**The lesson is about the edit, not the code.** A textual replacement that does not assert how many
times it matched is a silent no-op waiting to happen; the same script asserted counts for every other
substitution in that file and would have caught this immediately.

## Fault three: the scanline buffers were 512 wide

`SCREEN_MAX_W` sized every scanline buffer in the renderer, and the framebuffer, and it was 512 while
`AGB_PPU_MAX_W` had become 576. Every pixel past 512 was written off the end of an array — the exact
shape of fault this project keeps a `no-mmu` label for.

It hid for the same reason fault two did: the two constants had agreed for as long as the viewport
could not exceed 464, so the coupling was invisible until the width crossed 512. They are derived
from each other now, with a `_Static_assert` so they cannot drift apart again.

Found by a unit test that walks a marker down every column of an 80-tile buffer at the widest
viewport and checks each lands where it belongs: seven columns did not, and all seven rendered at
screen x 512. That test is kept.

**It is not what play reported, though.** Fixing it left the recorded session pixel-identical across
41 frames at 576x360. Two real bugs, one visible symptom, and they were not the same bug.

## What is still wrong

At 576 the outer columns of the picture smear vertically: the water's columns drag down past where
the water ends, the building's grey drags down into the grass, and at the left there are solid green
bars. The middle of the picture is correct.

Vertical smearing means those buffer columns hold the same tilemap entry repeated down their length,
because a column is filled by a loop that varies the map row. What it is **not**:

- **not the buffer's slack** — a 96-tile buffer renders identically to an 80-tile one at 576;
- **not the map connections** — disabling `MetatileIdFromConnectedMap` changes nothing;
- **not the incremental redraws** — forcing `DrawWholeMapView` on every camera step changes nothing,
  which puts it inside `DrawWholeMapViewInternal` or below it;
- **not the scanline overrun** above;
- **not outside the map** — the columns were checked against the map's own width and are inside it.

## What shipped

The three faults, the height, and a buffer wide enough for 576 whenever the smearing is understood
— but the width ceiling stays at 464. Also **464x360** — 40 metatiles of map across where the hardware had 15,
and what this display asks for at zoom 6 fullscreen. A native 240x160 walk is identical across all
249 sampled frames.

The height came along for a different reason: object positions are exact now
([ADR 0024](../adr/0024-a-machine-wider-than-the-hardware.md)), so the eight-bit Y that capped the
view at 240 stopped applying, and the buffer had been 512 tall since the layers doubled.

Three other things were needed and are worth naming, because none of them is about width as such:

- the renderer wraps a handed-over background by subtraction rather than by masking, so its size need
  not be a power of two;
- the scroll is handed to the renderer whole, because `BGxHOFS` is nine bits and describes a
  background of at most 512 pixels — the same truncation as an object's position, in the same shape;
- the spawn window is no longer capped by the object coordinate range, which stopped applying the
  moment those positions became exact.

## What found the first two

Rendering a recorded walk at **240x160** and comparing against the same walk at 64 tiles. The fault
was visible at the hardware's own size, which made it far cheaper to chase than anything needing a
wide viewport — and it is the measurement to reach for first when a change to the map buffer goes
wrong, because it holds everything else still.

**Three measurements in this spike were wrong before they were right**, which is the other thing to
carry forward. A world-column mapping that double-counted the scroll made two different columns look
like one and reported 10% agreement; only a control — the same column mid-screen in two frames, which
should agree and did not — showed the mapping was at fault rather than the renderer. Build the
control first.

The edge detector from the widescreen work is not the one to trust here. It reported 44 hits on the
left and 42 on the right at 576x360 *after* both faults were fixed, spread evenly across every column
it samples — which is what its heuristic does on high-contrast map edges when the scroll is a single
pixel. A real stale band concentrates in a few columns; this did not.
