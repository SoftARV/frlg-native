# Spike 0003 — What an empty cart region actually breaks

**Question:** the cart region is zero-filled until the Phase 7 importer fills it, and
[spike 0002](0002-host-assembly.md) closed with "this does not block Phases 1–6". Running the game
past the title screen for the first time was the chance to check that.

**Verdict: it does block Phase 6.** The game dies on the controls guide — the first screen that
reads *text* out of `data/*.s` — and it dies inside upstream code that is behaving correctly. The
developer data path covers assets compiled from `src/`, and nothing else.

## Reproduction

Deterministic, on the desktop build: run it, wait ~45 s for controls-guide page 1 (blue, `CONTROLS`
top-left), press A.

```
frlg-native: game thread backtrace
RenderText+0xba            src/text.c:658
AddTextPrinter+0x135       src/text_printer.c:150
AddTextPrinterParameterized4+0x99
Task_ControlsGuide_LoadPage  src/oak_speech.c:825
RunTasks+0xc4
```

Identical backtrace across four runs, and identical with the object renderer removed — this is not
a PPU fault.

## The chain

1. `gControlsGuide_Text_DPad` is defined by no compiled source. It is `data/*.s` text, so the binder
   maps it into the cart region: `-Wl,--defsym,gControlsGuide_Text_DPad=agb_cart+0x1EAD51`.
2. **Nothing ever writes `agb_cart`.** It is plain BSS (`platform/agb/src/memmap.c:5`); every other
   reference to it in the tree is a binder `--defsym`. So the string is 16 MiB of zeros.
3. `CHAR_SPACE` is `0x00` and `EOS` is `0xFF` (`include/characters.h:4`, `:182`). A zero-filled
   string is therefore **an unterminated run of spaces, not an empty string** — the one byte value
   that gives the text engine no reason to stop.
4. The call at `oak_speech.c:825` prints at speed `0`, which takes `AddTextPrinter`'s render-it-all-
   now branch: up to **1024 characters in a single call** (`text_printer.c:91`, `:101`).

So the printer is asked to lay 1024 spaces into a six-tile-wide window, and faults.

**Not established:** the exact faulting write. `text.c:658` is only where the signal lands. Glyph
advances running past the end of the window's pixel buffer is the likely mechanism, but pinning it
down needs a debugger and was not necessary to establish the cause.

## Why the test tiers missed it

Headless runs of 6,000, 9,000 and 12,000 frames all exit cleanly. The reason is not luck: with no
input the game reaches the **title screen** and parks there on `PRESS START` forever. Both host
backends report `HOST_KEYS_RELEASED`, so nothing ever advances. Frame 6,000 is the same title
screen as frame 2,400, give or take its animation.

Reaching the crash takes two presses — Start, to leave the title screen for the controls guide,
and then A, to turn to page 2 — so **no frame-count run of any length can arrive there**.

That is an argument for the scripted-driver tier in [ADR 0008](../adr/0008-testing-strategy.md)
being worth more than its position in the roadmap suggests. It also sets the boundary on what the
determinism harness can claim: the renderer is deterministic given identical input — repeated
headless runs and a windowed run that was left alone all produce byte-identical frames — but a run
that receives a keypress diverges legitimately, and comparing it against one that did not is a
measurement error rather than a bug.

## What it changes

Spike 0002's "does not block Phases 1–6" was measured against linking and rendering. It holds for
Phase 3 — graphics and the copyright text render because they are C data from `src/` — and fails
the moment the game reads a string that lives in `data/*.s`.

The options, none of them decided here:

- **Bring the importer forward** from Phase 7. It is the real fix and it is a large piece of work.
- **Populate the cart region in developer builds** from the ROM build's own binary, as a
  developer-only load that is never distributed — the same boundary
  [required-to-function.md](../required-to-function.md) already draws around developer builds.
- **Accept it for now** and keep Phase 6 scoped to what the compiled-in data path can reach.

The second is the cheapest thing that unblocks Phase 6 without touching the shipping story, since
the cart region and its offsets already exist and only the bytes are missing. Choosing between them
closes off alternatives, so it wants an ADR rather than a commit message.
