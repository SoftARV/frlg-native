# Roadmap

Phases are ordered by dependency, not by appeal. Each ends in something observable — a binary that
runs, a frame that appears, a sound that plays — because "we wrote a renderer" is not evidence and
a screenshot is.

Update the status column when a milestone lands.

| Phase | Goal | Status |
| --- | --- | --- |
| 0 | Foundations: repo, pinned submodule, docs, reference ROM builds | **done** |
| 1 | The game compiles and links natively and reaches `AgbMain` | **in progress** — 278/283 sources compile |
| 2 | Frame loop, interrupts, DMA, BIOS — a window running at 59.7275 Hz | |
| 3 | PPU — the first real frame, and the golden-screenshot harness | |
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

Three known gaps, none on the critical path:

- **32-bit SDL3 is not installed.** `lib32-sdl3 3.4.14-1` is in the enabled multilib repo, one
  `pacman -S` away. Phase 1 does not need it — the game links against the `null` host backend.
- `emcc` is absent, which only matters at Phase 8.

`agbcc` was listed here as a third gap "not on the critical path". **That was wrong**, and
[spike 0001](spikes/0001-relocation-table.md) found out why: the manifest is layout-specific, so it
must come from the byte-matching build. `agbcc` is a hard prerequisite of Phase 7. It has since
been built and installed.

## Phase 1 — It compiles and links *(next)*

The riskiest phase, and first: until 320k lines compile for an x86 target, every estimate after
this is a guess. Expect a long tail of small incompatibilities, not one large problem.

1. ~~CMake compiles the game sources through cpp → `preproc` → `gcc -m32`.~~ **done**
2. ~~Delegate binary asset generation to upstream's Makefile (developer data path).~~ **done**
3. ~~Redirect the hardware memory map; point the memory regions at the arena.~~ **done** — via the
   prelude ([ARCHITECTURE §4.1](ARCHITECTURE.md#41-the-prelude)), not path-order shadowing
4. ~~Assemble `data/*.s` with the host assembler.~~ **abandoned, correctly** — no host assembler
   can ([spike 0002](spikes/0002-host-assembly.md)). Those 1,107 symbols are bound into the cart
   region at link time instead, which is where they were always going to come from.
5. Override the five ARM-assembly sources plus `libagbsyscall.s` and `m4a_1.s`
   ([ARCHITECTURE §4.3](ARCHITECTURE.md#43-files-not-built)) — **176 symbols, next up**.
6. Stub every hardware entry point — enough to link, nothing more.
7. Link, run, reach `AgbMain`, exit cleanly.

**278 of 283 game sources now compile natively as x86-32** (`libgame.a`, 16.6 MB), with no change
to a single line of upstream source. The five exclusions are exactly the files carrying ARM inline
assembly, and each was already on the override list for an independent reason.

Link progress: **1,295 unresolved symbols → 176**, after binding the script and map data into the
cart region. The remainder are the seven excluded files' symbols — 17 BIOS syscalls and the rest
from `main.c`, `script.c`, `m4a.c`, `multiboot.c`, `librfu_intr.c` and the GameCube multiboot stub.

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

## Phase 2 — It runs

Fiber-based frame loop ([ADR 0004](adr/0004-fiber-frame-loop.md)), interrupt controller, immediate
DMA, BIOS calls, input polled into the key registers. A window opens, stays black, and the game
ticks at the correct rate. First point at which the headless determinism harness can exist.

## Phase 3 — It draws

The PPU, in the order the game stresses it: text backgrounds → objects → affine backgrounds →
windows → blending and mosaic. The golden-screenshot harness is built *with* the first background,
not after the last — PPU bugs found late are far more expensive than the harness.

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
