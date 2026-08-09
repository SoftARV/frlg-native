# New features

Things this port intends to do that the original cartridge does not. Everything here is a
deliberate addition; nothing here is a bug.

Faithful behaviour is not documented separately, because unlike a reimplementation this port
*runs the original code* — battle formulas, AI, encounter rates, frame timing and the original
games' own bugs are correct by construction rather than by effort.

## Committed

**Modding** — an embedded Lua layer with schema-driven registries: patch game data, hook engine
events, or own a render pipeline outright. Reference docs generated from the schema.
[ADR 0007](adr/0007-lua-mod-registries.md)

**Display modes** — LCD filters (pixel grid, colour modes, GBA screen effects) as post-process
stages above the framebuffer, plus a widescreen layout. The PPU is resolution-parametric from its
first line of code specifically so this is possible.

**Higher internal resolution** — the renderer draws more than 240×160 while game logic continues to
believe otherwise. UI and battle layouts anchored in screen space must be identified before this is
switched on.

**Smooth scrolling / high refresh** — logic stays locked to 59.7275 Hz, because decoupling it would
break timing assumptions across 320k lines. The PPU instead becomes re-runnable between logic ticks
with interpolated scroll offsets.

**Peer-to-peer link play** — trades and battles over a real transport rather than a stubbed cable.
This is why the serial layer is designed now instead of being permanently stubbed.

**Launcher, updater and packaging** — first-boot ROM import, mod management, update checks and
direct-launch shortcuts, with release pipelines per platform.

**Asset hot-reload** — assets reached through a resource-id table and the VFS, so a `data/`
override directory can substitute them at runtime.

## Explicitly not goals

**Save states.** They would require all mutable state to live in one snapshottable arena, which
constrains the memory map. That constraint is not being adopted.

**A built-in save editor.** Considered and not committed. It would need the save blocks reachable
as structured data rather than an opaque flash blob. Revisit after Phase 6.

## Under consideration

Translation and custom font support, controller and keyboard rebinding, performance presets and FPS
limits, mobile touch controls with editable layouts, a community mod browser, mod profiles with
separate save slots, soft-reset button combination.
