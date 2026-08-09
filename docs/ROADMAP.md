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
| 3 | PPU — the first real frame, and the golden-screenshot harness | **in progress** — text backgrounds render |
| 4 | Audio — the m4a mixer in C | |
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

## Phase 3 — It draws *(in progress)*

**The title screen renders.** Text backgrounds are done: all four layers, 4bpp and 8bpp, both
flips, all four map sizes with their multi-block layouts, priority ordering, and forced blank. That
is enough for the intro cinematic and the title screen to appear correctly.

Frame captures across a run, by distinct colour count — a flat backdrop scores 1:

| Frame | Colours | What it is |
| --- | --- | --- |
| 100 | 5 | copyright text on black |
| 400 | 5 | dark blue screen |
| 900 | 23 | intro cinematic, forest scene |
| 2400 | 161 | title screen |

`FRLG_SHOT=<path>` captures a frame as PPM; `tools/ppm_to_png.py` converts for review. Captures are
gitignored: they are ROM-derived imagery, and this project ships none.

Remaining, in the order the game stresses them: **objects** → affine backgrounds → windows →
blending and mosaic. Objects are the next and largest piece — nothing in the overworld draws
without them. Unimplemented features are skipped rather than approximated, so absence is visible
rather than subtly wrong.

Still to build: the golden-image harness itself (two thresholds, diff artifacts, `--bless`) and
mGBA reference captures to compare against. HBlank interrupts land here too, since per-scanline
effects depend on them.

Milestone: the title screen, pixel-comparable to the ROM.

## Phase 4 — It sounds

`m4a_1.s` reimplemented in C, driven from the frame loop, resampled by the host layer.

## Phase 5 — It remembers

Flash emulation over a host file in the `.sav` layout emulators use, so saves interchange with mGBA
and real hardware.

## Phase 6 — It plays

Intro through Pallet Town through the first battle without a crash or a visual defect. The
determinism harness runs in CI over a scripted input trace, and the autopilot captures shots.

This is the point the project becomes something a person can play.

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
