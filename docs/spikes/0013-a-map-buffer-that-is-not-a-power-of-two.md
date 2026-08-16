# Spike 0013 — A map buffer that is not a power of two

**Question:** the field's map layers are read from the game's own buffer now
([ADR 0024](../adr/0024-a-machine-wider-than-the-hardware.md)), so their size is ours to choose. A
576-pixel view wants 40 metatiles of map behind it, which is 80 tiles and 640 pixels. What breaks?

**Status: not finished.** Two faults found and one fixed; the width stayed at 64 tiles. The height
went up regardless, for an unrelated reason, so the trip was not wasted.

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

## Fault two: not found

With that fixed, a native 240x160 frame still differs from the same frame at a 64-tile buffer — 26 of
249 sampled, down from 110. Always **the rightmost metatile column of the visible window**, and the
region grows leftward as the player walks, which is a column written wrong once and then scrolling
in.

The leading-edge redraw for rightward movement looks right on paper: it writes buffer tile column
`xTileOffset + TILEMAP_W - 2` with map column `pos.x + TILEMAP_W / 2 - 1 + VIEW_ORIGIN_X`, which for
80 tiles is column 78 holding map column `pos.x + 27` — and the fill puts map column `O + j / 2` at
buffer column `xTileOffset + j`, so column 78 is `O + 39`, and `O` is `pos.x - 12`. Those agree.

Everywhere else that mixes a `u8` with 80 was checked and is safe, because the largest intermediate
is 157 and one subtraction is enough. So it is something else, and it was not found.

## What shipped instead

The height, which was waiting on nothing else. Object positions are exact now (ADR 0024), so the
eight-bit Y that capped the view at 240 no longer applies, and the buffer has been 512 tall since
the layers doubled. **464x360**, measured clean in all four directions across 433 scrolling frames of
a walk that goes round twice.

Also kept, because they are correct and cost nothing at 64 tiles:

- the renderer's wrap by subtraction, so a background need not be a power of two;
- the scroll handed to the renderer whole, since `BGxHOFS` is nine bits and describes at most 512
  pixels;
- the camera's signed step, above;
- the spawn window no longer capped by the object coordinate range, which stopped applying when the
  positions became exact.

Everything needed for a wider buffer is therefore in place except the buffer, and the next attempt
starts by finding what writes that rightmost column.

## For whoever picks it up

Set `TILEMAP_W` to 80 in `field_camera.patch`, the allocation and `agb_ppu_set_bg_source` to 80 in
`overworld.patch`, and `AGB_PPU_MAX_W` to 576. Then render a recorded walk at **240x160** — not at
576 — and compare against the same walk at 64 tiles. The fault is visible at the hardware's own size,
which makes it much cheaper to chase than anything that needs a wide viewport to reproduce.

The edge detector in the widescreen work reports left and right hits at 576x360, but it also reports
them on high-contrast map edges when the scroll is a single pixel, so it is not trustworthy on its
own here. The native comparison is.
