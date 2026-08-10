# ADR 0012 — Symbol binding requires a fixed load address

**Status:** accepted

## Context

[ADR 0006](0006-rom-supplied-data.md) binds the game's data symbols into the cart region with a
generated linker fragment: `--defsym gControlsGuide_Text_DPad=agb_cart+0x1EAD51`. It recorded an
"absolute-symbol-binding risk" as a future concern for the web target. It is not a future concern.
`--defsym sym=expr` defines an **absolute** symbol — `A` in `nm`, not `B` — and a
position-independent executable does not rebase absolute symbols when the loader moves the image.
Every one of the 1,120 bound symbols therefore pointed at unmapped memory at run time.

It went unnoticed for three phases because nothing dereferenced one. Graphics and text reached
through `INCBIN` are compiled into our own objects and are ordinary relocatable data; only the
`data/*.s` symbols are bound, and those are scripts, map data and menu text that the intro and
title screen never touch. The first dereference was the sound engine following `gMPlayTable` into
IWRAM, which faulted immediately.

This also revises [spike 0003](../spikes/0003-empty-cart-region.md). It attributed the controls-guide
crash to the cart region being zero-filled, which was true and sufficient to explain the symptom it
traced. It was not the whole story: that read would have faulted on the address alone.

## Decision

**The port links at a fixed load address.** `-no-pie` on the desktop port, and the same wherever the
platform allows it.

## Consequences

The binding scheme works as designed, with no indirection in the hot path: game code reaches
`gSpeciesInfo` as a direct load from a link-time constant, exactly as it did on the cartridge.

We give up ASLR. For a single-player game reading its own data files this is a small loss, and the
alternative costs a pointer dereference on every access to game data — which is most of what the
game does.

**Two platforms will not accept this, and both are phase 8.** Android has required
position-independent executables since API 21 and refuses to load anything else. The web target has
no notion of a fixed load address at all. Neither can take `-no-pie`, so both need the alternative
this decision defers rather than removes: bind each symbol as a *pointer variable* the loader fills
in, and have game code reach data through it. That is the "pointer-indirection layer" ADR 0006
already names, now with a measured reason to build it and a known cost — one indirection per data
access, on the platforms that force it.

The choice is per-platform rather than global, so desktop keeps the direct path. What phase 8 must
not do is discover this at porting time: the indirection layer is a prerequisite for Android, not a
detail of it.
