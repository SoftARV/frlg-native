# Roadmap

Phases are ordered by dependency, not by appeal. Each ends in something observable — a binary that
runs, a frame that appears, a sound that plays — because "we wrote a renderer" is not evidence and
a screenshot is.

Update the status column when a milestone lands.

| Phase | Goal | Status |
| --- | --- | --- |
| 0 | Foundations: repo, pinned submodule, docs, reference ROM builds | **done** |
| 1 | The game compiles and links natively and reaches `AgbMain` | **done** |
| 2 | Frame loop, interrupts, DMA, BIOS — a window running at 59.7275 Hz | **done** |
| 3 | PPU — the first real frame, and the golden-screenshot harness | **done** |
| 4 | Audio — the m4a mixer in C | **in progress** — mixer and interpreter done; reversed waves, sequencer and host output left |
| 5 | Saves — flash backed by a host file | |
| 6 | **Playable** — intro through the first battle, determinism harness | |
| 7 | **Shippable** — ROM importer, generated manifest, no data in the binary | |
| 8 | Windows, Android, web; launcher, packaging and updates | |
| 9 | Mods — Lua runtime, schema registries, generated reference docs | |
| 10 | Peer-to-peer link play | |
| 11 | 64-bit migration, then macOS and iOS | |
| 12 | Display modes, widescreen, high refresh | |

Two milestones matter more than the rest. **Phase 6** is when it becomes a game someone can play.
**Phase 7** is when it becomes something we can legally hand to them.

## Phase 0 — Foundations *(done)*

- `frlg-native` initialised, `pret/pokefirered` pinned as a submodule at `c75f3523`.
- Reference ROM built and verified: `make modern` → `pokefirered_modern.gba`, 16 MiB, exit 0.
- Manifest source proven: `make syms` → 50,590 symbols with addresses and sizes, 9,832 of them
  `g`-prefixed data symbols, plus a 35,037-line linker map.
- Architecture and eight decision records written.

Verified on this machine: `arm-none-eabi-gcc 16.1`, CMake 4.4, Ninja, clang 22, libpng, SDL3 3.4.14
(64-bit), and `gcc -m32` against a working `/usr/lib32` multilib.

One known gap: `emcc` is absent, which only matters at Phase 8. `lib32-sdl3` was missing at the
time and has since been installed.

`agbcc` was listed here as a third gap "not on the critical path". **That was wrong**, and
[spike 0001](spikes/0001-relocation-table.md) found out why: the manifest is layout-specific, so it
must come from the byte-matching build. `agbcc` is a hard prerequisite of Phase 7. It has since
been built and installed.

## Phase 1 — It compiles and links *(done)*

The riskiest phase, and first: until 320k lines compile for an x86 target, every estimate after
this is a guess. Expect a long tail of small incompatibilities, not one large problem.

1. ~~CMake compiles the game sources through cpp → `preproc` → `gcc -m32`.~~ **done**
2. ~~Delegate binary asset generation to upstream's Makefile (developer data path).~~ **done**
3. ~~Redirect the hardware memory map; point the memory regions at the arena.~~ **done** — via the
   prelude ([ARCHITECTURE §4.1](ARCHITECTURE.md#41-the-prelude)), not path-order shadowing
4. ~~Assemble `data/*.s` with the host assembler.~~ **abandoned, correctly** — no host assembler
   can ([spike 0002](spikes/0002-host-assembly.md)). Those 1,107 symbols are bound into the cart
   region at link time instead, which is where they were always going to come from.
5. ~~Handle the ARM-assembly sources~~ **done** — `main.c` needed no override at all (its asm is
   inside `#if MODERN`); the rest are excluded and their symbols stubbed
   ([ARCHITECTURE §4.3](ARCHITECTURE.md#43-files-not-built)).
6. ~~Stub every hardware entry point.~~ **done**
7. ~~Link, run, reach `AgbMain`.~~ **done**

**278 of 283 game sources now compile natively as x86-32** (`libgame.a`, 16.6 MB), with no change
to a single line of upstream source. The five exclusions are exactly the files carrying ARM inline
assembly, and each was already on the override list for an independent reason.

**Phase 1 is done.** `frlg-native` links and runs, and `AgbMain` executes its whole initialisation
sequence — RAM reset, GPU register setup, key init, interrupt handler install, sound init, RFU
init, flash detection — before reaching its main loop.

Final link: 1,309 unbound symbols resolved as 1,120 ROM data bound into the cart region, 14 RAM
variables bound to their arena offsets, 99 hard stubs, and 78 deferred-subsystem stubs.

The main loop then spins in `WaitForVBlank`, which waits on a flag only the VBlank **interrupt
handler** sets. That is exactly what phase 2 builds, so the boundary between the phases turned out
to be where the code itself divides.

Three findings worth carrying forward:

- **Keys are active-low.** A zeroed `REG_KEYINPUT` reads as every button held, which the game sees
  as the soft-reset combo — it reset before drawing anything. The register file needs its hardware
  reset state, not zeros.
- **Some files cannot be *executed* even though they compile**: the RFU drivers spin on adapter
  registers forever, and the flash driver runs Thumb code from a stack buffer. Both were found by
  running, not by reading.
- **A stall and a crash need different diagnostics.** The desktop port installs a watchdog and a
  fault handler that print backtraces; without them "it hangs" carried no information.

**The relocation spike is done** — [spike 0001](spikes/0001-relocation-table.md). Deriving the
table from `--emit-relocs` works: 61,137 embedded pointers, every offset inside the image, 100% of
sites holding a valid address. It also found that one pointer in five is a function pointer, making
import a patch pass rather than symbol binding, and that the manifest must come from the
byte-matching build. Running it before the rest of Phase 1 was the right call: both findings would
have been far more expensive to discover at Phase 7.

Expected friction, in the order it will probably appear: ARM-specific attributes and alignment
assumptions; `#pragma`s and builtins modern x86 GCC rejects; `sizeof` and struct-layout assumptions
that hold on ARM32 but not x86-32; the 38 raw address literals; upstream code taking the address of
something in a hardware region.

Done when the binary runs to the main loop and exits without a crash. Nothing renders.

## Phase 2 — It runs *(done)*

**The game ticks.** `AgbMain`'s main loop runs at the GBA's true rate: 1200 frames in 20.10 s
against an expected 20.09 s — 0.05% drift, no crashes. It is executing the intro sequence, calling
into the deferred sound subsystem for cries and music as it goes.

Done: the interrupt controller dispatching through the game's own `gIntrTable`, the frame driver,
immediate DMA, the BIOS calls, and the key register in its hardware reset state.

The delivery mechanism changed: **interrupts preempt via signal, they are not fiber switches**
([ADR 0009](adr/0009-preemptive-interrupts.md)). The main loop busy-waits on a memory flag and
calls nothing, so there is no yield point a cooperative switch could use. A signal handler runs on
the interrupted context's own stack, which is what hardware does.

**A window opens and the game drives it.** The SDL3 and `null` backends both build behind `host.h`;
SDL owns the main thread, the game runs on its own with the frame timer routed there, and keyboard
input reaches `REG_KEYINPUT`.

The first real pixels came from palette memory. Capturing the backdrop across a run:

| Frame | Backdrop |
| --- | --- |
| 1 | RGB(255,255,255) — the `RGB_WHITE` `AgbMain` writes to `BG_PLTT` before anything else |
| 60, 300, 900 | RGB(0,0,0) — the intro screen |

Game-driven colour, changing over time, read back through the prelude's palette redirection. It is
one colour rather than a picture because the PPU is phase 3, but it proves the path end to end.
`FRLG_SHOT=<path>` dumps the framebuffer as PPM — the seed of the phase 3 golden-image harness.

Verified windowed: 2000 frames in 33.65 s against 33.49 s expected, 0.5% drift.

Deferred to their own phases:

- HBlank and V-count interrupts; only V-blank is raised so far. Phase 3 needs HBlank for
  per-scanline effects.
- **`platform/agb` still calls POSIX directly** for the frame timer, which the layering rule
  forbids. Harmless today, blocking for Windows and web. Phase 8.
- **Determinism is an open problem** ([ADR 0009](adr/0009-preemptive-interrupts.md)). The phase 6
  harness cannot assume reproducible frame boundaries from a wall-clock timer.

## Phase 3 — It draws *(done)*

**The title screen renders.** Text backgrounds are done: all four layers, 4bpp and 8bpp, both
flips, all four map sizes with their multi-block layouts, priority ordering, and forced blank.
The object layer is done too: all twelve sizes, 4bpp and 8bpp, both flips, 1D and 2D tile mapping,
the 256-line vertical wrap, priority against the backgrounds and against each other, and both
affine modes with their 32 matrix groups.

Frame captures across a run, by distinct colour count — a flat backdrop scores 1:

| Frame | Colours | What it is |
| --- | --- | --- |
| 100 | 5 | copyright text on black |
| 400 | 11 | dark blue screen |
| 900 | 29 | intro cinematic, forest scene |
| 2400 | 166 | title screen |

`FRLG_SHOT=<path>` captures a frame as PPM; `tools/ppm_to_png.py` converts for review. Captures are
gitignored: they are ROM-derived imagery, and this project ships none.

Affine backgrounds are done as well — all four map sizes, the 20.8 reference point, per-scanline
matrix stepping and both overflow behaviours — which also settled which layers each display mode
defines. **They change nothing on screen yet:** every frame reachable today is mode 0, and the three
places FRLG asks for mode 1 (`oak_speech.c` past the controls guide, `credits.c`, `trade_scene.c`)
all sit behind the Phase 6 blocker. The work is proven by the unit tier, not by a screenshot.

Windows are done too — both rectangles, the object window, the region outside them all, and the
garbage-bounds rules — and this one *is* visible: the intro cinematic's letterbox was leaking
background into its black bands, because the mask that clips it did not exist.

Blending is in too — alpha, both brightness effects, semi-transparent objects, and the target and
window rules deciding when each applies. It shows on the GAME FREAK logo, which fades in rather than
appearing solid. It also forced a composition change the renderer needed anyway: layers are now
collected two-deep per pixel, because a blend needs whatever sits under the top layer and
first-writer-wins cannot supply it.

The unit tier is up: `tests/` runs under CTest, with `test_ppu_objects`, `test_ppu_bg_affine`,
`test_ppu_windows`, `test_ppu_blend`, `test_ppu_mosaic` and `test_ppu_bitmap` driving the renderer from hand-built registers, VRAM and
OAM to cover what a running frame does not reach — 8bpp, 2D tile mapping, the size tables, the wrap
and overflow rules, priority ordering, both affine transforms with their clipping, the mode/layer
table, the window regions with their precedence, the blend arithmetic against hand-computed colours,
the mosaic block snapping on every layer that has it, and the three bitmap modes.

Mosaic is in as well, for backgrounds and objects, each with its own enable and its own half of the
size register. Like the affine backgrounds it changes nothing on a reachable frame — all four
reference captures are byte-identical — because the game does not ask for it in the intro.

The bitmap modes finish the renderer: all three, both frames where there are two, the 160x128
extent of mode 5, and the tile-512 floor they impose on objects. Like mosaic and the affine
backgrounds they change nothing reachable — FRLG's intro is mode 0 throughout.

**Every PPU feature is now implemented.** Nothing is skipped, which retires the rule that carried
the renderer through: while it was being built, absence read as absence and the picture never lied
about what existed.

The golden harness is built: `tools/golden.py` captures the frames in `tests/golden/manifest.txt`,
compares each against a golden with its own two thresholds, and on failure writes a golden / actual
/ difference panel. `ctest -L golden` runs it; `--bless` regenerates the references. It was proved
by breaking the renderer on purpose — dropping the window mask on the object layer failed frame 900
with 63 pixels over budget and a diff marking Gengar's feet.

The goldens are generated per machine and never committed, because they are frames of a
copyrighted ROM ([ADR 0010](adr/0010-goldens-are-generated.md)). That has a consequence worth
stating plainly: **blessed from our own renderer, the tier catches regressions but not
incorrectness.** It says "the same as yesterday", not "the same as the ROM".

**The milestone is met: the title screen is pixel-identical to the ROM.** `tools/mgba_capture.c`
runs the reference build under mGBA and dumps frames in the same PPM format, and the port matches
it on frame 2400 at **0 of 38,400 pixels**, with both thresholds at zero. The copyright screen
matches exactly too.

mGBA's frame numbers run 38 ahead of ours, because it boots through the BIOS and `crt0` where the
port enters `AgbMain` directly. The offset was found by sweeping rather than assumed, and it is
recorded per frame in the manifest.

Frames 400 and 900 are still self-blessed, and the reason is not the renderer: their backgrounds
match mGBA exactly and only animated sprites sit at a different phase, because scene pacing
diverges wherever the game waits on the stubbed audio subsystem. Full evidence in
[spike 0004](spikes/0004-mgba-frame-alignment.md); they are worth re-testing once phase 4 lands.

The scanline interrupts are in: H-blank and V-count match, raised from inside the composition loop,
with the DISPSTAT flags and the three gates each one passes through. Every layer already re-read its
registers per line, so a handler writing from H-blank changes the picture from the next line down —
which `battle_transition.c` needs for its six per-scanline effects, and so does phase 6.

**Phase 3 is done.** The renderer implements every display mode and layer feature the hardware has,
the title screen is pixel-identical to the ROM, and the tier that proves it runs under CTest.

Milestone: the title screen, pixel-comparable to the ROM.

## Phase 4 — It sounds *(in progress)*

`m4a_1.s` reimplemented in C, driven from the frame loop, resampled by the host layer.

`m4a_1.s` is 1917 lines of ARM across 36 routines, and it is two subsystems: a track interpreter
(`MPlayMain`, `TrackStop`, 24 `ply_*` opcodes) and the mixer (`SoundMain`, `SoundMainRAM`,
`SoundMainBTM`, `m4aSoundVSync`). Upstream's `m4a.c` sequencer is not built at all yet.

The order, and where it stands:

1. **Envelope** — **done**. Attack, decay, sustain, release, the pseudo-echo tail and the volume
   fold, in `platform/agb/src/m4a_mixer.c`, driven directly by `test_m4a_envelope`.
2. **PCM mixing loop and the sample walk** — **done**, both paths. Fixed-frequency walks the wave
   at its own rate; the pitched path resamples with linear interpolation off a 9.23 fractional
   position, carrying it across frames and rewinding repeatedly when a step clears a whole loop.
   Both share the eight-bit wrapping accumulate. `test_m4a_mix`.
3. **Buffer management and the mixer driver** — **done**. Which frame of the PCM area is written,
   preparing its two buffers by clearing or by folding in a reverb, and then `SoundMain` over the top:
   the lock, the chain of music players, the compatible-sound oscillators, and a pass over every
   channel that steps its envelope and hands it to the fixed or resampled path. `test_m4a_frame`.

   Upstream reaches the mixing loop by copying its own machine code into IWRAM and jumping there.
   **Not reproduced, and it could not be**: a copied x86 function's relative calls would resolve to
   the wrong targets. `SoundMain` calls the loop directly, and it keeps our own name
   (`agb_m4a_mix_frame`) because `SoundMainRAM` names a block of bytes upstream relocates rather than
   a routine.

   `SoundMainBTM` turned out not to be a mixer routine at all -- it is the 64-byte clear reached as
   `gMPlayJumpTable[35]`. Upstream's header declares it as taking no argument although it receives a
   pointer, which nothing noticed because it is only ever called through unprototyped table entries;
   ours lives in its own file so that declaration is not in scope.

   **Reversed and compressed waves are still not mixed**, and a channel asking for one is skipped
   with a warning rather than mixed wrongly. Walking the ROM's own instrument tables prices the gap:
   of 66 tone tables reachable from the song table, two instruments are reversed -- voices 1 and 33 of
   one table -- and none is compressed. `SoundMainRAM_Unk1` and `Unk2`, 237 lines of ARM between
   them, are what is left.

   Found on the way: the original's scanline CPU budget **cannot fire**, because `gMaxLines` is
   absolute zero in every upstream linker script. Reproducing it would have meant inventing a
   meaning for `VCOUNT` during V-blank.
4. **The track interpreter** — the parameter opcodes are **done** in
   `platform/agb/src/m4a_track.c`, driven by `test_m4a_track`: priority, tempo, key shift,
   instrument select, volume, pan, bend, bend range, tune, both modulation controls and the delay,
   and the direct sound-register write. They are real symbols rather than stubs now, which the
   binding report confirms.

   Control flow is **done** as well: the jump, the three-deep pattern call stack, the repeat
   counter, ending a track, and unlinking a channel from the chain its track keeps. Twenty of the
   sequencer's symbols are provided rather than stubbed now, which took the deferred count from 127
   to 107.

   `TrackStop`, `ChnVolSetAsm` and `ply_endtie` are **done** too, taking the unbound count to 1289
   from the 1311 it started the phase at.

   `m4aSoundVSync` is **done** as well -- the per-frame countdown to the next sound buffer and the
   re-arming of the two FIFO channels.

   The note allocator, `ply_note`, is **done** — 279 lines of ARM, the largest single routine in the
   file. It calls into the sequencer (`ClearChain`, `TrkVolPitSet`, `MidiKeyToFreq`), so it stays
   inert until step 5; it is tested against handlers the test supplies, and its 44 mutants are all
   caught.

   Two things it turned up. The **key split table and the instrument array are different pointers**,
   held at different offsets of the same instrument — the assembler's `keySplitTable` is `.equiv` to
   the `attack` offset, not to `wav`. Reading both from `wav`, which is the obvious mistake, makes a
   key-split instrument select on its own envelope bytes. And the added gate time **cannot overflow**
   its byte: the largest clock entry is 96 and an operand is at most 127, so a wrap test is
   unreachable and the largest reachable sum is pinned instead.

   The driver, `MPlayMain`, is **done** — 338 lines of ARM, and with it the track interpreter is
   complete. Its 58 mutants are all caught.

   Worth knowing when reading the binding report: implementing a caller before its callee makes the
   unbound count **rise**. `ply_note`, `MPlayMain` and `SoundMain` between them reference five
   sequencer routines (`ClearChain`, `TrkVolPitSet`, `MidiKeyToFreq`, `FadeOutBody`, `Clear64byte`)
   that nothing built had asked for before, so the count went up even as symbols were provided. It is
   a measure of what is referenced, not of what is missing.

   Two of its shapes are preserved rather than tidied, and both are pinned by a test: the track loops
   are `do`/`while`, so a player claiming no tracks still runs its first one; and the modulation
   counter is stored as a byte but the sum is used untruncated, so a large speed folds past the
   triangle's midpoint against the wide value.

   Worth knowing for the tests that come next: the recompute pass at the end of a frame **consumes**
   the invalidation flags and clears the low nibble, so a flag set during a tick is never observable
   after the call. Assertions have to be made on the consequence -- whether the volume or the pitch was
   actually recomputed -- and two of mine were wrong until that was understood.
5. **Build upstream's `m4a.c` and drop the deferred entries.** Attempted twice, reverted twice.
   The second attempt, with the driver in place, found the real dependencies -- and found that the
   first attempt's diagnosis was **wrong**.

   The mechanics work and are settled. `m4a.c` compiles and links once one BIOS call is removed
   from its preprocessed copy, and the link needs two of upstream's linker-script absolutes:
   `gNumMusicPlayers = 4` and `gMaxLines = 0`, both declared `extern char []` and read as their own
   address. `gMaxLines` matters -- non-zero switches on the mixer's scanline budget.

   The removal must match the statement exactly and fail when it is absent. A blanket erasure of
   `asm` would be wrong: `global.h` rewrites it to `__asm__`, and the preprocessed file carries a
   second `__asm__` -- glibc's asm label on `strerror_r`. `MusicPlayerJumpTableCopy`, which holds the
   call, is dead code here; nothing calls it and the table is filled by `MPlayJumpTableCopy`.

   **What actually blocks it, in the order the game meets them:**

   - **A busy-wait our frame model cannot satisfy.** `SampleFreqSet` spins on
     `while (VCOUNT != 159)` to phase-align timer 0. A whole frame is rendered inside one signal
     handler on the game thread, so `VCOUNT` sweeps 0..159 while the game thread is *suspended* and
     reads 0 every time it resumes. The wait can never end. The process does not hang -- the frame
     timer keeps counting -- which is why the first attempt read this as "blank frames" and blamed a
     missing `MPlayMain`. It is the only busy-wait on a specific `VCOUNT` value in the game; every
     other read happens inside a handler, where the value is right. Fixing it means either changing
     upstream behaviour through the same build seam, or making `VCOUNT` advance continuously the way
     hardware does -- a timing change with its own consequences, and its own decision to make.
   - **ROM data holding pointers into RAM.** `gMPlayTable` is bound into the cart region and used
     where it lies, but its entries are the addresses the original had for *its* RAM -- `0x03004518`
     and friends -- which are mapped nowhere here. Dereferencing one is the segfault behind
     `m4aSoundInit`. This is [spike 0001](spikes/0001-relocation-table.md)'s relocation problem
     arriving at phase 4 rather than phase 7: the sound engine is the first subsystem that follows a
     pointer stored *inside* ROM data. Supplying that one table in C works and was verified, since
     every entry resolves to a named RAM symbol the binding tool already places in the arena. It does
     not generalise: `gSongTable` and the song and tone data behind it are the same problem at scale.
   - **`SoundMainBTM` is not the mixer, it is the 64-byte clear.** `m4a.c`'s `Clear64byte` reaches
     it as `gMPlayJumpTable[35]`, so while it is stubbed `MPlayOpen` clears nothing and then reads
     uninitialised fields. Thirteen lines of ARM.

   So the order is: finish the mixer (`SoundMain`, `SoundMainRAM`, `SoundMainBTM` -- the rest of
   step 3, not of step 5), decide the `VCOUNT` model, and relocate the song tables. Only then does
   the sequencer have anything to run.

6. Host audio: `host.h` gains an output stream, SDL3 implements it, `null` stays silent.

Waiting at the end of it: [spike 0004](spikes/0004-mgba-frame-alignment.md) excluded frames 400 and
900 from the mGBA oracle **because** audio is stubbed and scene pacing drifts. They are the measure
of whether this phase worked.

## Phase 5 — It remembers

Flash emulation over a host file in the `.sav` layout emulators use, so saves interchange with mGBA
and real hardware.

## Phase 6 — It plays

Intro through Pallet Town through the first battle without a crash or a visual defect. The
determinism harness runs in CI over a scripted input trace, and the autopilot captures shots.

This is the point the project becomes something a person can play.

**Blocked on the cart region having bytes in it.** The game already crashes on the controls guide,
the first screen that reads text out of `data/*.s`: the symbols are bound, but nothing has filled
the region behind them, and a run of `0x00` is `CHAR_SPACE` rather than `EOS`. Evidence, and the
three ways out, in [spike 0003](spikes/0003-empty-cart-region.md). The cheapest unblocks this phase
without touching Phase 7's shipping story.

Worth carrying into the scripted-input harness: frame-count runs cannot see this class of bug at
all, because the game parks on the first screen that waits for a button.

## Phase 7 — It ships clean

The shipping model ([ADR 0006](adr/0006-rom-supplied-data.md)): generate the manifest from the ROM
build, bind data symbols into the cart region with a generated linker fragment, and write the
first-boot importer — verify SHA-1, load, relocate, cache, release.

Ends with a binary containing no game data. Until this lands, nothing can be distributed to anyone.

## Phase 8 — It travels

Windows and Android from the same SDL3 backend; web via Emscripten (`wasm32` is 32-bit, so no
pointer work). Android needs touch controls, the first genuinely new UI in the project. The web
target may hit the absolute-symbol-binding risk in ADR 0006 and need a pointer-indirection layer.

Launcher, packaging and update pipelines land here, because Phase 7 made distribution possible.

## Phase 9 — It mods

Lua runtime, the schema table, registries, the loader, and reference docs generated from the
schema ([ADR 0007](adr/0007-lua-mod-registries.md)). Render pipelines come with it, since they are
the seam that makes an alternative renderer a mod rather than a fork.

## Phase 10 — It connects

Peer-to-peer link play for trades and battles, over `host_net`. Lockstep, so the deterministic
fiber loop is a prerequisite. Desync fuzzing lands with it, not after.

## Phase 11 — It grows up

The 64-bit migration: regenerate data into a native layout, flip the accessor to add the arena
base, audit struct layouts and save serialisation. macOS and iOS follow immediately, being 64-bit
only.

## Phase 12 — It gets better

Display modes and LCD filters, widescreen, high-refresh interpolation. The hooks are designed in
from Phase 3; this is where they are switched on.
