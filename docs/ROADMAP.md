# Roadmap

Phases are ordered by dependency, not by appeal. Each one ends in something observable — a binary
that runs, a frame that appears, a sound that plays — because "we wrote a renderer" is not
evidence and a screenshot is.

Update the status column when a milestone lands.

| Phase | Goal | Status |
| --- | --- | --- |
| 0 | Foundations: repo, pinned submodule, docs, reference ROM builds | **done** |
| 1 | The game compiles and links natively and reaches `AgbMain` | next |
| 2 | Frame loop, interrupts, DMA, BIOS — a window that runs at 59.7275 Hz | |
| 3 | PPU — the first real frame on screen | |
| 4 | Audio — the m4a mixer in C | |
| 5 | Saves — flash backed by a host file | |
| 6 | Playable: intro through the first battle, determinism harness | |
| 7 | Windows, Android, web | |
| 8 | 64-bit migration, then macOS and iOS | |
| 9 | Enhancements: widescreen, high refresh, modding | |

## Phase 0 — Foundations *(done)*

- `frlg-native` initialised, `pret/pokefirered` pinned as a submodule at `c75f3523`.
- Reference ROM built and verified: `make modern` → `pokefirered_modern.gba`, 16 MiB, exit 0.
- Architecture and decision records written.

Verified on this machine while scoping: `arm-none-eabi-gcc 16.1`, CMake 4.4, Ninja, clang 22,
libpng, SDL3 3.4.14 (64-bit), and `gcc -m32` against a working `/usr/lib32` multilib.

Three known gaps, none on the critical path:

- **32-bit SDL3 is not installed.** `lib32-sdl3 3.4.14-1` is in the enabled multilib repo, so it is
  one `pacman -S lib32-sdl3` away. Phase 1 does not need it — the game links against the `null`
  host backend and never opens a window — but Phase 2 does.
- `agbcc` is absent, so the *byte-matching* ROM build is unavailable. The modern ROM is a valid
  reference for behaviour; it just has a different checksum.
- `emcc` is absent, which only matters at Phase 7.

## Phase 1 — It compiles and links *(next)*

The riskiest phase, and the one that must come first: until 320k lines compile for an x86 target,
every estimate after this is a guess. Expect the bulk of the work to be a long tail of small
incompatibilities, not one large problem.

1. CMake compiles the game sources through `preproc` → cpp → `gcc -m32`.
2. Delegate binary asset generation to upstream's Makefile; consume the generated files.
3. Assemble `data/*.s` with the host assembler (verified: portable directives only).
4. Shadow the five hardware headers; point the memory regions at the arena.
5. Stub every hardware entry point — enough to link, nothing more.
6. Link, run, reach `AgbMain`, exit cleanly.

Expected friction, in the order it will probably appear: ARM-specific attributes and alignment
assumptions; `#pragma`s and builtins that modern x86 GCC rejects; `sizeof` and struct-layout
assumptions that hold on ARM32 but not x86-32; the 38 raw address literals; and upstream code that
takes the address of something in a hardware region.

Done when the binary runs to the main loop and exits without a crash. Nothing renders.

## Phase 2 — It runs

Fiber-based frame loop (`docs/ARCHITECTURE.md` §5.5), the interrupt controller, immediate DMA, the
BIOS calls, and input polled into the key registers. A window opens, stays black, and the game
ticks at the correct rate. First point at which the headless determinism harness can exist.

## Phase 3 — It draws

The PPU, built in the order the game stresses it: text backgrounds → objects → affine backgrounds
→ windows → blending and mosaic. Golden-image comparison against mGBA from the first background
onward, because PPU bugs found later are far more expensive than the harness.

Milestone: the title screen, pixel-comparable to the ROM.

## Phase 4 — It sounds

`m4a_1.s` reimplemented in C, driven from the frame loop, resampled by the host layer.

## Phase 5 — It remembers

Flash emulation over a host file, in the `.sav` layout emulators use, so saves interchange with
mGBA and real hardware.

## Phase 6 — It plays

Intro through Pallet Town through the first battle without a crash or a visual defect. The
determinism harness runs in CI over a scripted input trace. This is the point the project becomes
something a person can actually play.

## Phase 7 — It travels

Windows and Android from the same SDL3 backend; web via Emscripten (`wasm32` is 32-bit, so it
needs no pointer work). Android needs touch controls, which is the first genuinely new UI in the
project.

## Phase 8 — It grows up

The 64-bit migration: emit offsets instead of embedded pointers, flip the accessor to add the
arena base, audit struct layouts and save serialisation. macOS and iOS follow immediately, since
they are 64-bit only.

## Phase 9 — It gets better

Widescreen, high-refresh interpolation, modding and hot-reload. The hooks are designed in from
Phase 3; this is where they get switched on.
