# Spike 0013 — A map buffer that is not a power of two

**Question:** the field's map layers are read from the game's own buffer now
([ADR 0024](../adr/0024-a-machine-wider-than-the-hardware.md)), so their size is ours to choose. A
576-pixel view wants 40 metatiles of map behind it, which is 80 tiles and 640 pixels. What breaks?

**Status: answered.** Two faults, both fixed. The second one is the more interesting: it was not in
the arithmetic at all, it was a line the change never reached.

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

## What shipped

Both faults fixed, and with them **576x360** — 40 metatiles of map across where the hardware had 15,
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

## What found it

Rendering a recorded walk at **240x160** and comparing against the same walk at 64 tiles. The fault
was visible at the hardware's own size, which made it far cheaper to chase than anything needing a
wide viewport — and it is the measurement to reach for first when a change to the map buffer goes
wrong, because it holds everything else still.

The edge detector from the widescreen work is not the one to trust here. It reported 44 hits on the
left and 42 on the right at 576x360 *after* both faults were fixed, spread evenly across every column
it samples — which is what its heuristic does on high-contrast map edges when the scroll is a single
pixel. A real stale band concentrates in a few columns; this did not.
