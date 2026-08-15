# Spike 0012 — Why a wide view draws trees in the middle of a road

**Question:** walking towards a joined map at a wide viewport, the ground ahead of the player is drawn
as trees or cliff until the boundary is crossed, after which it corrects itself. Where does the wrong
metatile come from, and what would it take to hold enough map to draw the right one?

**Status: answered and fixed** — by the second of the two routes below, not the first. The obvious
fix works and was built, then abandoned: the constant it changes turns out to be part of the save
file. The narrow one, reading the joined map's layout in the drawing path, shipped.

## The wrong metatile is the map's border

`MAP_OFFSET` is how far around the player the map buffer reaches, and upstream says why it is 7 in the
comment above it:

> Map coordinates are offset by 7 when using the map buffer because it needs to load sufficient
> border metatiles to fill the player's view (**the player has 7 metatiles of view horizontally in
> either direction**).

Seven metatiles is 112 pixels, which is exactly half of the hardware's 240-pixel screen. The virtual
map is sized to match — `mapLayout->width + MAP_OFFSET_W` — and a map connection fills the margin with
`FillConnection(..., /*width*/ MAP_OFFSET, height)`: seven metatiles of the joined map, no more.

Past that, `MapGridGetMetatileIdAt` does not fail. It answers:

```c
if (block == MAPGRID_UNDEFINED)
    return GetBorderBlockAt(x, y) & MAPGRID_METATILE_ID_MASK;
```

`GetBorderBlockAt` returns the map's own **border** metatile — the repeating tree or water pattern a
map is fenced with. So a viewport wider than the hardware's asks for metatiles the buffer never held,
and gets a plausible-looking wrong answer instead of a blank. Crossing the boundary re-bases the
coordinates onto the joined map, whose own layout covers those columns, and the picture repairs
itself. That is the whole reported symptom.

**It is not the camera.** The camera fills 32 metatiles and every one of them is drawn; the ones past
the seventh are drawn from data that says "border".

## Raising MAP_OFFSET works, and costs more than it looks

16 is the half-width of the widest view the field can draw, and at that value the camera's window lies
inside the virtual map at every position on every map. It needs `MAX_MAP_DATA_SIZE` raised to `0x4000`
(Diglett's Cave B1F, the largest layout, needs 13216 of 16384 entries), the one place the offset is
written as a literal — `dest += VMap.Xsize * 7` — corrected, and `FillConnection` clamped, because a
joined map shallower than the strip walks off the front of its own layout.

It cannot be a prelude redefinition, because `fieldmap.h` defines `MAP_OFFSET` itself and would
overwrite one; force-including `fieldmap.h` ahead of it drags `global.h` into every translation unit,
where its `abs` macro breaks every file that later includes `<stdlib.h>`. A shadow header found by
`-iquote` works, and reaches all ten files that convert coordinates — verified by reading the value
back out of the preprocessed sources rather than trusting the include path.

All of that was built and it draws correctly. **Then the measurements.**

## MAP_OFFSET is part of the save format

Two structures in the save block are shaped or framed by it:

- **`gSaveBlock2Ptr->mapView`** is a fixed `u16[0x100]`, and the 15×14 window upstream stores there is
  210 of them. A window derived from `MAP_OFFSET` 16 would be 33×32 — 1056 entries, written over
  whatever follows it in the save block. Fixable: pin the window's shape and move only where it sits.
- **The quest log** stores object coordinates as they stand, and `currentCoords` is a map coordinate
  *plus MAP_OFFSET*:

  ```c
  questLog->objectEvents[i].x = gObjectEvents[i].currentCoords.x;
  ```

  So a scene recorded under one offset and replayed under another puts every object nine metatiles
  from where it was recorded. Converting at that boundary is straightforward and was done.

Neither is the end of it. With both conversions in place, a recorded play-through still diverges from
the same trace on the previous build — first as one frame of the player's walk animation, then, once
that changes what a step lands on, as a different wild encounter entirely. The remaining path was not
found.

## What shipped, and what to do instead

The camera work is independent of all of this and was measured on its own: with `MAP_OFFSET` left at
7, a build carrying the camera changes is **byte-identical** to the previous one across a recorded
play-through. That half shipped — it fixes a genuine and much more visible fault, where a viewport
taller than 176 pixels showed a band of unrelated map across the top, because the vertical axis had no
margin at all and the tilemap wrapped.

The next attempt should **not** move `MAP_OFFSET`. The coordinate frame is load-bearing in the save
file, and a constant that ten files and two save structures agree on is the wrong seam. The narrower
one is the drawing path: when the camera asks for a metatile outside the virtual map, resolve it
through the map connection and read the joined map's layout directly, the way `GetBorderBlockAt`
resolves the border today. That touches what is *drawn* and nothing that is *stored*, which is the
distinction this spike is really about.

## What was done

`MetatileIdFromConnectedMap` in `field_camera.patch`, called from `DrawMetatileAt` when the
coordinate falls outside the map buffer. It reads the joined map's own layout, placed by the same
arithmetic `FillWestConnection` and its three siblings use — the joined map's `(0, 0)` sits at a
fixed virtual coordinate per direction, so a coordinate outside the buffer belongs to that map if it
lands inside its width and height.

Nothing stored changes, and neither does collision or elevation: those still stop where the buffer
stops, which was never wrong, because the player cannot reach these metatiles without crossing onto
the map that owns them — and crossing re-bases the buffer.

Measured over a recorded walk from Route 22 into Viridian City, at 464x192 against the commit
before: **125 of 146 sampled frames identical, 21 differing, and every one of them within four
hundred frames of the boundary**. At the hardware's own size, 189 of 189 identical — the columns this
changes are off screen there, and are redrawn before they are not.

## For the next person measuring this

Four separate conclusions in this investigation were read off frames that turned out to be a menu, a
fade or the quest log. Sampling a recording at a fixed step lands on the field less often than it
looks, and two builds agreeing on a menu says nothing about the camera. `FRLG_SHOT_FIELD=1` exists
now and skips everything that is not the overworld.

The other trap: a replay diverging does not mean the rendering changed. Compare a **static** frame
first — same save, no input — which isolates geometry from game state. The camera changes here looked
like a regression across 24 frames of a recording and were identical on every static frame, which is
what pointed at the save format rather than at the drawing.
