# ADR 0004 — The game runs on a fiber

**Status:** superseded in part by [ADR 0009](0009-preemptive-interrupts.md)

The analysis below — that a "call the game once per frame" loop cannot express a blocked
`VBlankIntrWait` or run HBlank handlers — held up. The proposed *mechanism* did not: `AgbMain`'s
main loop busy-waits on a memory flag and calls nothing, so a cooperative switch has no yield point
to use. Interrupts are delivered by signal instead. The claim of frame-for-frame deterministic
headless runs does not survive that change; see ADR 0009.

## Context

The game's shape is `while (1) { callback1(); callback2(); VBlankIntrWait(); }`, which invites a
host loop that calls the game once per frame. Two things break that:

- `VBlankIntrWait` is called from deep inside nested game code — palette fades, text printers,
  wait loops — not only from the top-level loop. The game blocks mid-callstack.
- HBlank interrupt handlers must run *between scanlines*, while the game is blocked, and they
  touch game state.

A once-per-frame call cannot express either. Only something that can suspend and resume the game
mid-callstack can.

## Decision

Run the game on its own fiber. The host drives the hardware and switches to the game fiber when
the hardware would have raised an interrupt:

1. Begin frame; poll input into the key registers.
2. Per scanline: render it, and if the game enabled HBlank, switch to the game fiber for its
   handler and back.
3. At scanline 160, raise VBlank: switch to the game fiber, which returns from `VBlankIntrWait`
   and runs a frame of logic until it blocks again.
4. Present the framebuffer; mix audio.

`host.h` exposes fiber create/switch: `ucontext` on POSIX, Fibers on Windows, Asyncify on the web.

## Consequences

- Interrupt semantics are reproduced honestly, including HBlank effects and DMA.
- Switching is explicit, so there are no data races on game state and no locking anywhere.
- Headless runs are deterministic frame for frame, which is what makes the determinism harness in
  Phase 6 possible at all.
- Every platform must provide a fiber primitive. Emscripten needs Asyncify, which costs binary
  size and some speed — accepted, and confined to the web port.

## Alternatives rejected

**Game on a thread with handoff** — same expressiveness, but introduces races on game state,
requires synchronisation on every switch, and makes headless runs non-reproducible.

**Call the game once per frame** — cannot express a blocked `VBlankIntrWait` mid-callstack, and
cannot run HBlank handlers at all.
