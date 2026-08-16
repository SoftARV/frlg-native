# 0024 — A machine wider than the hardware

**Status:** proposed
**Date:** 2026-08-16

## Context

The field draws 464x192 where a Game Boy Advance drew 240x160, and that is as far as it goes. Every
remaining limit is the hardware's rather than this port's, and they are all reached at once:

| Limit | Value | Binds |
| --- | --- | --- |
| A text background | 64x64 tiles, 512x512 px | width and height |
| VRAM left for the three map layers | 14 KB, after tilesets and BG0 | height |
| An object's X | 9 bits, 512 px of coordinate space | width, once the layers grow |
| An object's Y | 8 bits, 256 px | height, once the layers grow |

The map layers are what binds today. They are 64x32 tiles because that is the widest a text
background goes and the tallest the free VRAM holds, and the camera needs a metatile of margin at
each edge, which is where 464x192 comes from. Objects would allow roughly 480x240 — so making the
layers bigger moves the wall to the object coordinates, and not before.

[ADR 0015](0015-enhancement-over-preservation.md) already says this port may differ from the
cartridge deliberately. Every such difference so far has been in what the *game* does — stereo by
default, a wider window onto the same 240x160 the game drew. This is the first that changes what the
*machine is*, and that is a different kind of decision, so it gets its own record.

## Decision

**The port's machine may exceed the hardware's limits where the limit is an encoding rather than an
intent, and where the game's own code already works in the wider terms.**

That test matters, and both halves of it are met here:

- **A background's size** is two bits in a register. The game does not think in those two bits: it
  builds its map into `gBGTilemapBuffers1/2/3`, plain arrays in its own heap, and calls
  `CopyBgTilemapBufferToVram` to shuffle them into the shape the hardware wants. The size is the
  register's limit, not the game's.
- **An object's position** is nine bits and eight in OAM. The game computes it as a signed 16-bit
  pair and hands it over to be truncated. Again the limit is the encoding's.

So: **the renderer may read a background straight from the game's own buffer**, at whatever size that
buffer is, instead of from VRAM at one of four sizes. And **an object may be placed from the position
the game computed**, rather than from the nine and eight bits that survived.

Neither needs more VRAM. The map layers stop being copied into it at all, which is 12 KB back and the
end of the collision with the tilesets that [`overworld.patch`](../../platform/game/overrides) has
already had to dodge once.

## Consequences

**The two-block tilemap layout goes away.** A 64-tile-wide background is two 32x32 blocks side by
side, and `field_camera.patch` carries a `TilemapOffset` to write into it — the source of one of the
uglier widescreen bugs. A buffer the renderer reads directly is row-major, and that function becomes
`y * width + x`.

**The viewport stops being a hardware question and becomes a memory one.** How much map to hold is
then a trade between heap and how much world is worth showing, which is a decision to make with a
number rather than against a register.

**Anything that reads those backgrounds out of VRAM stops seeing them.** The field's own code reaches
its map through the buffer, so this is about the port's own tools: the VRAM dump no longer shows the
map layers, and the golden harness sees no change because it captures frames rather than memory.

**It is per background, and opt-in.** A background with no override is read from VRAM exactly as
before, at one of the four sizes, with the hardware's wrapping. Every screen that is not the field
keeps the machine it had, which is what keeps the blast radius to the field and the tests honest.

**The drift check gains nothing to watch.** This is the port's own renderer and the port's own game
layer; no upstream file is patched to make it work beyond the field's own setup, which is patched
already.

## Alternatives

**Enlarge the emulated VRAM.** `agb_mem.vram` is our array and could be 256 KB. Rejected: the game
addresses VRAM through registers that cannot reach past 64 KB, so the extra would need an
out-of-band base anyway — and then the copy into it is pure cost, since the data is already sitting
in a buffer the renderer could read.

**Move BG0's tiles to free its 16 KB.** Would buy enough VRAM for taller layers without any of this.
Rejected: it is 16 KB of font and window graphics whose extent depends on which screen is up, so the
saving is only safe until a screen loads more than measured — the same reasoning that put the map
layers at 0x6000 and cost five frames of garbage tiles.

**Leave it at 464x192.** Genuinely defensible: it is a good picture, and both remaining faults
(issues 12 and 13) are about edges the wider view exposed. Rejected because the ceiling is arbitrary
from a player's side — it is not a number anyone chose, it is where four unrelated encodings
happened to meet.
