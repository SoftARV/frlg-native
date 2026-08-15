# 0022 — The port gets its own sources in the game layer

**Status:** accepted
**Date:** 2026-08-15

## Context

The port needs an options screen of its own: somewhere to switch on the display pipelines
([§8](../ARCHITECTURE.md#8-mods)) and whatever else is added from here, reached from the pause menu
and looking like the screen next to it.

A screen calls the game's own window, text and task helpers. That places it in the **game layer**,
and the game layer had nowhere for code of ours to live:

- `platform/agb/` is *below* the game. The dependency rule is strictly downward, so it cannot call
  up into `AddWindow` or `CreateTask`.
- `vendor/pokefirered/` is a pinned submodule and is never edited.
- `platform/agb/overrides/` exists, but an override means **forking a whole upstream file**. Adding
  one row to a menu is not worth forking a thousand lines that then stop receiving upstream fixes.

Three of the four places a change can go were wrong for a reason, and the fourth was the wrong tool.

## Decision

**`platform/game/` holds sources the port adds to the game layer**, compiled into the `game` target
through `preproc` exactly like the upstream ones.

Through `preproc` specifically, because the port's UI needs the game's *text encoding*: `_("PORT")`
becomes real bytes only if preproc sees it. This is the first string the port owns — every character
on screen until now has come from the cartridge.

**Reaching this code from an upstream file is a patch, not an override.** Adding the pause-menu entry
is three insertions into the preprocessed copy of `start_menu.c` — an enum value, a table row, and one
`AppendToStartMenuItems` call — each an exact match with an expected count, the same mechanism as the
six existing classes of edit ([§4.2](../ARCHITECTURE.md#42-overrides)). Upstream fixes keep flowing;
an override would have stopped them for the sake of six lines.

Naming follows the port's convention rather than the game's: `agb_port_*`, snake case. The game's
`PascalCase` namespace is not encroached on even here, where our code sits beside it.

## Consequences

**There is now a fourth kind of change to upstream behaviour**, and it is the cheapest one: add code
in `platform/game/`, reach it with a small patch. The override table stays empty, which is the
outcome to want.

**Port code in the game layer may call downward past `platform/agb`.** The options screen talks to
`host_render` and `host_options` directly. That is legal — the rule forbids referencing anything
*above*, not skipping a level below — and it is what `ports/desktop/main.c` already does. An
`agb_`-prefixed passthrough would be indirection with nothing on the other side of it.

**A patch is a promise about upstream text.** Each insertion is anchored to a line that must still
exist, with a count that must still match, so a submodule bump reports the change instead of silently
dropping the pause-menu entry. That is the same bargain every other patch class makes.

**These sources are ours, so they are ours to test.** They are game-layer code, which the golden tier
covers only by playing through them; anything in `platform/game/` that can be tested without a screen
should be.

## Alternatives

**Override `start_menu.c`.** Rejected: a thousand-line fork to add six lines, and the first entry in
an override table that is deliberately empty.

**Put the screen in `platform/agb/`.** Rejected: it would have to call up into the game layer, which
is the one thing the layer model forbids outright.

**Draw the screen without the game's helpers**, straight into VRAM from the host side. Rejected: it
would not look like the game, it would duplicate the window and text engines, and it would have to be
kept in step with them by hand.
