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
| 4 | Audio — the m4a mixer in C | **done** |
| 5 | Saves — flash backed by a host file | **done** |
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

   **Reversed and compressed waves are both mixed**, which completes `m4a_1.s`. `SoundMainRAM_Unk1`
   covers three cases -- compressed forward, compressed reversed, and uncompressed reversed -- and all
   three are written, along with `Unk2`, the block decoder. 17 mutants for the reversed walk and 15 for
   the compressed one.

   The compressed instrument is **Nidorino's cry** in the intro battle, at frames 961--985. Finding it
   corrected a claim this step originally made -- that nothing in the game is compressed -- which came
   from scanning only the tone tables the song table reaches directly, never the sub-tables a rhythm or
   key-split instrument points at. A static reading was taken for a complete one; the runtime settled
   it in one line.

   One of the compressed decoder's 15 mutants is caught **only by AddressSanitizer**: halving the
   block-length step writes 128 samples into a 64-byte buffer while producing byte-identical output for
   all 64, so no test that looks at samples can see it. Worth remembering as an argument for an ASan
   preset -- the surviving mutant was checked by hand and the real code is clean.

   Two things about the reversed path are easy to get wrong and are pinned by tests. The play position
   is turned round **once**, on the channel's first frame, and it mirrors the position rather than
   simply moving to the end -- a note told to start partway in has to keep that offset. And a reversed
   wave **does not loop**; running out ends the note, whatever the wave's own loop flag says.

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
5. **Build upstream's `m4a.c`** — **done**. The sequencer is built, initialises, and runs: the
   player chain, the tempo accumulator, the track interpreter and the mixer all execute every frame,
   and **no sound call is deferred any more**. The golden tier is unchanged by it.

   Two statements had to go, both hardware no host can run, both removed from the preprocessed copy
   by a script that fails when either is absent
   ([ARCHITECTURE §6.7](ARCHITECTURE.md#67-audio)): the BIOS dispatch-table call, and
   `SampleFreqSet`'s spin until the display reaches scanline 159. The second can never end here,
   because a whole frame renders inside one signal handler while the game thread is suspended, so
   `VCOUNT` reads 0 every time it resumes. The link also needs `gNumMusicPlayers = 4` and
   `gMaxLines = 0`.

   Getting there turned up three things worth more than the step.

   - **The bindings were broken by position independence.** `--defsym sym=agb_cart+N` defines an
     *absolute* symbol, which a PIE does not rebase, so all 1,120 bound symbols pointed at unmapped
     memory. Nothing had dereferenced one in three phases: graphics and text come through `INCBIN`
     into our own objects, and only `data/*.s` symbols are bound. The sound engine following
     `gMPlayTable` was the first. Fixed by linking at a fixed address
     ([ADR 0012](adr/0012-fixed-load-address.md)), which Android and the web will not allow --
     phase 8 needs the indirection layer as a prerequisite rather than a detail.
   - **A real bug in our own `ply_note`.** `attach_channel` read the track's chain head *before*
     unhooking the channel; the original reads it after. Stealing a channel that was already the
     head of its own track's chain therefore pointed it at itself, and the next frame's walk down
     the chain never ended. Pinned by a test that fails without the fix. Forty-four mutants had not
     caught it -- only running the thing did.
   - **`umul3232H32` was still a stub**, and `MidiKeyToFreq` puts every pitched note's frequency
     through it. Six lines, now implemented.

   A build wrinkle fixed on the way: the bindings step writes `agb_stubs.c` but only declared
   `bindings.rsp` as its output, so a build that newly implemented a stubbed symbol linked against
   the previous stub object and failed once before succeeding.

7. **The PSG channels** — **done**, in `platform/agb/src/psg.c`. This was what was still missing by
   ear after the mixer was finished. The GBA has two
   sound systems: the m4a engine mixes direct sound in software -- which is everything phase 4 built --
   and four **hardware PSG channels** (two squares, a programmable wave, noise) that it drives by
   writing registers. `m4a.c` writes 25 of them, `CgbSound` runs every frame doing so, and **nothing
   reads them**: the port's only reference to those addresses is `ply_port` storing a byte into the
   register file.

   Counting instruments reachable from the song table: 466 direct sound, 258 rhythm or key-split, and
   **88 on PSG channels -- 11%, all silent**. Reported by ear first: the intro's cry and drums play,
   the title screen is missing parts. That is the shape you would expect when the sampled half works
   and the synthesised half does not.

   It is a new subsystem rather than more of the mixer: four channel generators with their own
   envelopes, sweep and length counters, mixed into the same PCM buffer the host already consumes.
   Sampling is at the mixer's rate rather than the hardware's, and the final add clamps where the
   software mixer's own accumulate wraps -- one reproduces the m4a engine, the other is the DAC.

   **Measured against mGBA** rather than left to judgement — [spike 0007](spikes/0007-audio-against-mgba.md).
   Per-second level tracks the reference across the whole intro, so the notes and their timing are
   right. The balance is defensible: our overall level runs about 1.3x mGBA's, which is as much its
   output convention as ours. The one clear spectral difference, excess treble, was measured to come
   from the direct-sound path and not from the PSG -- switching the PSG off makes it worse.

6. **Host audio** — **done, with a caveat**. `host.h` gained an output stream, SDL3 implements it,
   and `null` stays silent. The mixer hands each finished frame to a sink the port installs, and the
   game's music reaches the device at its own 13,379 Hz.

   The caveat is [spike 0006](spikes/0006-release-build-silence.md): audio is produced by the
   **Debug** build and not by the optimised one, while both render identically. It is real, it
   reproduces, and it is not yet diagnosed.



Waiting at the end of it: [spike 0004](spikes/0004-mgba-frame-alignment.md) excluded frames 400 and
900 from the mGBA oracle **because** audio is stubbed and scene pacing drifts. They are the measure
of whether this phase worked.

## Phase 5 — It remembers *(done)*

Flash emulation over a host file in the `.sav` layout emulators use, so saves interchange with mGBA
and real hardware.

`platform/agb/src/flash.c` supplies **the chip, not the driver**. Upstream's `agb_flash*.c` talk to
real flash through timed command sequences and one of them runs code copied onto the stack, so they
are not built; what they drive is 128 KiB in thirty-two 4 KiB sectors, and that is what this is. The
save code above them is upstream's and cannot tell the difference — it now identifies the chip, reads
it and programs it, and none of `IdentifyFlash`, `ReadFlash` or `SetFlashTimerIntr` is deferred any
more.

Two behaviours are the chip's rather than a convenience, and the save code depends on both: an erased
cell reads `0xFF`, and programming can only clear bits. The file is written after each sector is
programmed rather than at exit, so a save survives the program being killed, and a short file is
padded with erased cells so a save from a smaller part still loads.

Eleven mutants, all caught. One of them needed a sharper test than the obvious: reading at exactly
the end of a sector proves nothing, because the clip yields a size of zero there anyway. The guard
only earns its place *beyond* the end, where the subtraction underflows and asks for a copy of nearly
four gigabytes.

**Exercised by the game itself.** A player reached a save point and saved: the file is 131,072 bytes,
fourteen sectors carry the `0x08012025` signature with ids 0 to 13 each exactly once, the save counter
reads 1, and the checksums are the game's own. It went to slot 2, which is the rotation the save code
does, and the remaining eighteen sectors are still erased. `ProgramFlashSectorAndVerify` read every
sector back and accepted it, or the save-failed screen would have appeared instead.

**And it round trips.** Restarting the port offers CONTINUE and resumes where the player left off, so
the read path works through the game's own code and not merely through our tests. Phase 5 is done in
the sense that matters: the game remembers.

Still untested is whether **mGBA reads the same file**. It should -- the layout is the emulator
convention, which is why it was chosen -- but that is a claim rather than a measurement. It needs no
player, so it belongs with phase 6's harness.

## Phase 6 — It plays

**Input traces record and replay.** `FRLG_INPUT_RECORD=<file>` writes what the keyboard did, a line
per frame on which it changed; `FRLG_INPUT=<file>` replays one. The keys are supplied by the frame
driver on the game's own thread, at the same point in every frame, rather than by the presenting
thread writing the register whenever it gets round to it — which is what makes a replay repeatable.
Three replays of the same trace produce a byte-identical frame 900.

That is the harness the rest of the phase needs: reaching a place in the game reproducibly is what
lets the save round trip, the mGBA save read and any visual defect be checked without a player.
`mgba-capture` takes the same trace, so the oracle reaches those moments too.

**The first thing it was pointed at**: the missing trainer pictures -- Oak standing in the opening
speech, and the large player picture on the gender screen. [Spike 0008](spikes/0008-missing-trainer-pics.md)
narrowed it to BG2's affine transform being all zeros, which draws nothing, and then found that the game
never sets that transform -- which made the attribution look wrong until the reference was asked what
its renderer held there. Solved; see below.

**Building it found something worse than it was for.** With a save file present, the game **could not
boot** — `LoadGameSave` runs before the save blocks' pointers are set, so its sector copy wrote
through null. On hardware those writes are ignored and the real load happens later; here every build
crashed. It was invisible until phase 5 produced a save to load, and it is the same no-MMU class as
the naming screen, now with writes rather than reads
([ARCHITECTURE §4.3](ARCHITECTURE.md#43-files-not-built)).

**The script VM is built**, which was the thing standing between the intro and the game. `script.c`
was excluded for one statement -- `svc 2`, BIOS Halt, inside a loop that never ends, reached only on
finding a corrupt script pointer. The game means to stop there, and waiting for the next V-blank for
ever does the same without the BIOS, so the same seam that handles m4a.c handles this.

That one line was holding back **forty-eight routines**, all of them called while showing a message:
hard stubs fell from 54 to 6, and the six left are libc internals, the ARM interrupt entry we replace,
and two ROM header symbols -- none reachable in play. Found by running the game rather than by
reading: it stopped on `IsMsgSignpost`.



Intro through Pallet Town through the first battle without a crash or a visual defect. The
determinism harness runs in CI over a scripted input trace, and the autopilot captures shots.

This is the point the project becomes something a person can play.

**A trace replays exactly here, and diverges on the reference** — by construction, not by defect. The
port does a frame's work in microseconds where the hardware needs more than its 16.743 ms, and FRLG
loses a V-blank outright whenever that happens: 73 of them across the intro, none here. So the intro
plays a little faster than on a cartridge, and frame numbers cannot be carried between the two across a
heavy scene. [Spike 0009](spikes/0009-trace-pacing-vs-mgba.md).

**The capture clock is deterministic** ([ADR 0013](adr/0013-lockstep-capture-clock.md)). It was not,
and the tier was passing on luck: with frames driven by a wall-clock timer, a capture on a loaded
machine misses a V-blank and lands a frame behind one on an idle machine. Six concurrent captures of
one binary produced three different frames, each of them pixel-exact against some real mGBA frame.
Under `FRLG_LOCKSTEP` they produce one, and the tier runs in a tenth of the time.

**The trainer pictures appear.** Oak in the opening speech and the player on the gender screen were
missing because the affine matrices are the identity at rest, not zero, and `RegisterRamReset` was
clearing them along with the rest of the display registers — the game never writes a matrix for those
screens. [Spike 0008](spikes/0008-missing-trainer-pics.md); the answer came from reading what mGBA's
renderer was about to draw with, not from reasoning about what the game ought to write.

**Continuing a saved game works.** A map with no object events has a legitimately null pointer and the
loop copying their scripts reads sixty-four entries regardless — the third instance of the no-MMU
class, and the first one reachable only because saving worked.

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

**The loading and relocation half of this is done, pulled forward into phase 4** — the sound engine
is the first subsystem to follow a pointer stored inside ROM data, and nothing works without it. The
ROM is loaded into the cart region and its pointers rewritten once, by a table generated from the
same build the bindings come from ([ARCHITECTURE §5.3](ARCHITECTURE.md#53-relocating-what-the-image-points-at),
[spike 0005](spikes/0005-relocation-classes.md)). Of 61,142 records, 51,227 are rewritten and 9,915
are deliberately left alone. The golden tier is unchanged by it, which is the point: loading 16 MiB
of real game data and rewriting 51,227 pointers inside it altered no pixel of the first 2,400 frames.

That also settles [spike 0003](spikes/0003-empty-cart-region.md): the controls guide read
`gControlsGuide_Text_DPad` as an unterminated run of spaces because the region was zeros. It now
holds the real string, terminated 68 bytes in.

What stays here is the shipping half: a *retail* cartridge rather than the development build, which
means regenerating the manifest, the bindings and the relocation table together from the
byte-matching build, plus SHA-1 verification, caching and the first-boot flow.

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
