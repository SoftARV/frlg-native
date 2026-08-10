# Architecture

This document explains every part of the project. It is the contract: if a commit changes what a
subsystem does, how layers talk to each other, or how the build works, it updates this file in the
same commit.

- [1. What this is](#1-what-this-is)
- [2. Layer model](#2-layer-model)
- [3. Repository layout](#3-repository-layout)
- [4. Upstream integration](#4-upstream-integration)
- [5. Game data](#5-game-data)
- [6. The virtual GBA](#6-the-virtual-gba)
- [7. The host abstraction](#7-the-host-abstraction)
- [8. Mods](#8-mods)
- [9. Launcher, packaging and updates](#9-launcher-packaging-and-updates)
- [10. Ports](#10-ports)
- [11. Build system](#11-build-system)
- [12. Pointer width](#12-pointer-width)
- [13. Extension points](#13-extension-points)
- [14. Testing](#14-testing)
- [15. Conventions](#15-conventions)

---

## 1. What this is

`pret/pokefirered` is a complete decompilation: 320k lines of C, with only five `.s` files left
(`crt0`, `rom_header`, `libagbsyscall`, `libgcnmultiboot`, `m4a_1`). Nothing meaningful remains in
assembly.

That makes a **source port** the correct approach, and it rules out the alternatives:

| Approach | Verdict |
| --- | --- |
| Emulation | Rejected — ships a CPU interpreter and gains nothing we can enhance. |
| Static recompilation | Rejected — that technique exists for games *without* source. We have source. |
| Reimplementation | Rejected — re-deriving 320k lines of behaviour by hand, then spending years documenting where it diverges. |
| **Source port** | **Chosen.** Compile the game for the host CPU; reimplement the hardware beneath it. |

The game code does not learn that it has been ported. It keeps calling `REG_DISPCNT`, `DmaCopy16`,
`CpuFastSet` and `VBlankIntrWait`. We supply those.

Two measured properties of the codebase make this tractable:

- Hardware access is funnelled through five headers in `include/gba/`. Only **38** raw address
  literals exist in all of `src/`.
- `data/*.s` uses only portable GAS directives — no ARM or Thumb directives at all — so the host
  assembler consumes those files unmodified.

**What we get for free.** Because the original code runs, battle formulas, trainer AI, encounter
rates, growth curves, frame timing and the original games' own bugs are correct by construction.
A reimplementation earns each of those and then maintains a register of where it still diverges.
We have no such register and never will. This is the single largest advantage of the approach and
it should shape where effort goes: not into parity, into everything above it.

**What we ship.** Code and a generated manifest. No game data — that comes from the player's own
ROM ([§5](#5-game-data)).

## 2. Layer model

```
┌────────────────────────────────────────────────────────────────┐
│  launcher            ROM import · mods · updates · shortcuts   │
└───────────────────────▲────────────────────────────────────────┘
┌───────────────────────┴────────────────────────────────────────┐
│  mods                embedded Lua · schema registries          │
│  patches data · hooks events · owns render pipelines           │
└───────────────────────▲────────────────────────────────────────┘
┌───────────────────────┴────────────────────────────────────────┐
│  vendor/pokefirered  game logic, 320k LOC, PRISTINE            │
│  thinks it is running on a Game Boy Advance                    │
└───────────────────────▲────────────────────────────────────────┘
                        │  GBA hardware API
                        │  (shadowed headers + link-time overrides)
┌───────────────────────┴────────────────────────────────────────┐
│  platform/agb        the virtual GBA                           │
│  memmap · io · ppu · dma · irq · timer · bios · m4a · flash    │
│  NO OS CONDITIONALS — identical code on every target           │
└───────────────────────▲────────────────────────────────────────┘
                        │  host.h  (one narrow interface)
┌───────────────────────┴────────────────────────────────────────┐
│  platform/host       video · audio · input · vfs · clock ·     │
│                      fiber · net · log                         │
│  sdl3 implementation · null implementation (headless tests)    │
└───────────────────────▲────────────────────────────────────────┘
┌───────────────────────┴────────────────────────────────────────┐
│  ports/  desktop · android · ios · web · switch                │
└────────────────────────────────────────────────────────────────┘
```

**The dependency rule is strictly downward.** No layer may reference anything above it. The PPU
never calls SDL; it writes into a framebuffer the host layer later presents. This is what keeps a
new platform to "implement `host.h`" rather than "port the renderer".

The single most important consequence: **`platform/agb` contains zero `#ifdef _WIN32` /
`__ANDROID__` / `__APPLE__`.** Every OS conditional lives under `platform/host/` or `ports/`. If a
platform difference leaks into the AGB layer, the `host.h` interface is wrong and gets fixed there.

## 3. Repository layout

```
frlg-native/
├── vendor/pokefirered/          pinned submodule, never edited
├── platform/
│   ├── agb/
│   │   ├── include/gba/         shadow headers replacing upstream's five
│   │   ├── include/agb/         our own interfaces
│   │   ├── src/                 the hardware reimplementation
│   │   └── overrides/           .c files replacing an upstream .c
│   └── host/
│       ├── include/host.h       the entire host interface
│       └── src/{sdl3,null}/     backends
├── import/                      ROM validation, extraction, cache, relocation
├── mods/                        Lua runtime, schema registries, loader
├── launcher/                    first boot, mod manager, updates
├── ports/                       entry points, packaging, manifests
├── cmake/                       toolchains, asset pipeline, drift check
├── tools/                       manifest generation, drift check, golden differ
├── tests/                       units, drivers, goldens
└── docs/
```

## 4. Upstream integration

`vendor/pokefirered` is pinned at commit `c75f3523` and **is never modified**. pret continues to
fix decomp bugs; we want those for free. Every deviation is expressed in one of exactly three ways,
all explicit and greppable.

### 4.1 The prelude

`platform/agb/include/agb/prelude.h` is force-included ahead of every game translation unit with
`-include`. It pulls in upstream's hardware headers first — which sets their include guards — and
then redefines what it needs. When game code later reaches its own `#include "gba/io_reg.h"`, the
guard short-circuits the file and our definitions stand.

| Redefined | Effect |
| --- | --- |
| `EWRAM_START`, `IWRAM_START`, `PLTT`, `VRAM`, `OAM` | region bases point into the arena |
| `REG_BASE` | the whole register file follows, since every `REG_ADDR_*` is `REG_BASE + offset` |
| `SOUND_INFO_PTR`, `INTR_CHECK`, `INTR_VECTOR` | fixed IWRAM addresses become arena offsets |
| `IWRAM_DATA`, `EWRAM_DATA`, `COMMON_DATA` | no-ops; the host linker places these freely |
| `DmaSet`, `DmaStop` | call the DMA engine instead of writing registers |

Everything derived from those — `EWRAM_END`, `BG_PLTT`, `OBJ_VRAM0`, `DmaCopy16`, `DmaFill32` —
follows automatically, because macro bodies expand at the point of use rather than of definition.
No upstream header is edited and no upstream header is replaced.

**Path-order shadowing does not work here, and this was measured rather than assumed.** A quoted
include resolves against the including file's own directory first, so `include/global.h` doing
`#include "gba/gba.h"` finds vendor's copy immediately and never consults `-iquote`. A shadow
directory earlier on the include path has no effect on headers that include each other.

Two include-path hazards, both of which produced silent wrong behaviour before being pinned down:

- **vendor/include must stay off the bracket chain.** The game ships its own `strings.h`; putting
  its directory on the `-I` chain makes it hijack POSIX `<strings.h>` for every host header that
  needs it, which silently changes which declarations a translation unit sees.
- **The directory must not be named twice.** cpp de-duplicates the search path and keeps the later
  entry, so `-iquote include` combined with `-idirafter include` silently discards the `-iquote`.

The prelude is invisible at the call site, which is its risk: if pret changes one of the macros it
overrides, we would diverge silently. `tools/check_drift.py` records the upstream hash of every
header the prelude depends on and every overridden `.c`, and CI fails when one moves. Bumping the
submodule pin is always its own commit and always re-runs the check.

### 4.2 Overrides

A few upstream `.c` files describe hardware we cannot reproduce by redefining a macro. For those,
CMake excludes the upstream file and compiles `platform/agb/overrides/<name>.c` instead.

Every override is listed here, with its reason and the upstream file it forked from.
**Adding an override without adding a row is a defect.**

| Override | Replaces | Reason |
| --- | --- | --- |
| _(none yet)_ | | |

Overrides are a cost, not a convenience: each one stops receiving upstream fixes. Prefer a shadow
macro; reach for an override only when there is no macro seam.

### 4.3 Files not built

`crt0.s`, `rom_header.s`, `libagbsyscall.s`, `libgcnmultiboot.s` and `m4a_1.s` describe cartridge
boot, the BIOS ABI and the ARM sound mixer. None apply to a native binary. `libagbsyscall` and
`m4a_1` are reimplemented in C under `platform/agb/src/`.

**271 of the 283 game sources compile natively with no source change at all.** The exclusions fall
into two groups, and every one describes hardware rather than game logic.

**Cannot be compiled** — ARM inline assembly:

| Excluded | Assembly it carries | Owned by |
| --- | --- | --- |
| `script.c` | `svc 2` (HALT) | override — the pointer accessor ([§12](#12-pointer-width)) |
| `m4a.c` | `swi 0x2A` | the mixer ([§6.7](#67-audio)) — a build seam, not an override |
| `multiboot.c` | ARM busy-wait | GameCube link, out of scope |
| `librfu_intr.c` | naked ARM trampolines | link play ([§6.9](#69-serial-and-link-play)) |

**Cannot be executed** — drives hardware that does not exist natively, found by running:

| Excluded | Why | Owned by |
| --- | --- | --- |
| `librfu_rfu.c`, `librfu_stwi.c`, `librfu_sio32id.c`, `sloopsvc.c` | spin on wireless-adapter registers that never change; reads as a hang | link play, phase 10 |
| `agb_flash*.c` (4 files) | `ReadFlashId` copies Thumb code into a stack buffer and calls it | saves, phase 5 ([§6.8](#68-save-data)) |
| `isagbprn.c` | writes to no$gba debug I/O addresses in no real region | a host-console version would improve on the original |

`main.c` is **not** excluded. Its only ARM assembly is an IWRAM clear inside `#if MODERN`, so
upstream's own non-modern path avoids it; `MODERN` gates nothing but `NOINLINE` and an `abs` macro,
so no layout or ABI changes. The modern path exists to work around a `RegisterRamReset` hazard that
does not apply when `RegisterRamReset` is ours.

The exclusion list is almost exactly the set of files that were already going to be replaced for
independent reasons, which is good evidence the layering in [§2](#2-layer-model) cuts in the right
place. Unresolved symbols are bound by `tools/gen_symbol_bindings.py`: ROM data to the cart region,
RAM variables to their true arena offsets, and routines the port has not written yet to generated
per-name stubs. A stub for a *deferred subsystem* (sound, link, flash) warns once and returns;
every other stub aborts. A silent no-op in game logic is worse than a crash, because it looks like
it worked.

## 5. Game data

The port ships no graphics, audio, text or data tables. They come from the player's ROM.
Rationale and risks: [ADR 0006](adr/0006-rom-supplied-data.md).

### 5.1 The manifest

We build the ROM ourselves, so the manifest is a build output rather than a reverse-engineering
project. `make syms` emits **50,590 symbols with addresses and sizes** — 9,832 of them `g`-prefixed
data symbols — the linker map adds section detail, and `--emit-relocs` adds 49,547 relocation
records. `tools/` turns those into the shipped manifest: symbol names, ROM offsets, sizes,
relocation entries, and the native symbol table the patch pass needs.

It contains no ROM bytes, no graphics, no dialogue, no audio samples.

**The manifest must be generated from the byte-matching build**, which needs `agbcc`. Addresses
from the `MODERN=1` build describe a different layout — `ld_script.ld` pins object placement in 512
explicit entries to reproduce retail, while `ld_script_modern.ld` is wildcards, and a modern-GCC
`.text` is a different size, shifting everything after it. A manifest built from the modern ROM
would describe addresses no player's cartridge has.

### 5.2 The cart region and symbol binding

The arena reserves a cart region at the GBA's `0x08000000`. At first boot the player's ROM is
SHA-1 verified, loaded into that region, and released — never copied into the cache.

Data symbols are **not compiled into the binary**. A generated linker fragment defines each one at
its address inside the cart region, so `extern const struct SpeciesInfo gSpeciesInfo[]` resolves
into the loaded image and game code reaches its data unmodified.

### 5.3 Relocation

Pointers embedded in ROM data are ROM addresses (`0x08xxxxxx`). After loading, the importer walks
the relocation table and rewrites every one of them. **Import is a patch pass over the loaded
image, not merely symbol binding.**

The table is derived mechanically by relinking the ROM with `--emit-relocs` and reading back
`.rel.rodata`, `.relscript_data` and `.rel.data`. Proven against a byte-identical retail ROM in
[spike 0001](spikes/0001-relocation-table.md): **49,547 embedded pointers, every offset inside the
image, 100% of sites holding a valid address**, and the ROM still matching `firered.sha1` with
relocations emitted.

Each record names its target symbol, which is what makes the three cases separable:

| Target | Share | Resolved to |
| --- | --- | --- |
| Data | 87.65% | `cart_base + (rom_addr - 0x08000000)` |
| Code | 11.15% | the **native** function of that name |
| RAM variable | 1.20% | the **native** variable of that name |

One pointer in nine is a function pointer, and native code does not live in the ROM image — so the
importer needs a symbol-name → native-address table alongside the relocation table. Both are build
outputs.

This is the same seam the pointer-width strategy needs ([§12](#12-pointer-width)), which is why the
two are one mechanism rather than two: native function addresses must fit the ROM's 4-byte slots,
which is 32-bit-only, exactly as ADR 0003 independently concluded.

### 5.4 Data the host cannot build

`data/*.s` — event scripts, battle scripts, map data — **is never assembled, on any platform.**
[Spike 0002](spikes/0002-host-assembly.md) found that no host assembler can build it: GNU `as`
splits macro arguments on whitespace, clang's integrated assembler rejects `.if` on non-absolute
symbols, and the two fail on different files. `event_scripts.s` alone is 83% of `script_data`.

Those symbols are bound into the cart region at link time instead, by `tools/gen_cart_syms.py`,
which emits one `--defsym <symbol>=agb_cart+<rom offset>` per symbol from the ROM build's `.sym`.
Game code then reaches its data through the symbol it always used. This resolved **1,107 of the
1,295** unresolved symbols in a real link.

Since the data was always going to come from the player's ROM ([§5.2](#52-the-cart-region-and-symbol-binding)),
this costs nothing and removes a whole class of toolchain risk — MSVC has no GAS at all, and
Android, iOS and wasm all use clang.

### 5.5 The developer data path

A developer build compiles the `.c`-defined data in, exactly as upstream's ROM build does, so
renderer work can proceed before the importer exists. This keeps the importer off the critical path
for Phases 1–3. Script and map data still come from a locally built ROM, per §5.4.

**Developer builds are never distributed.** They embed ROM-derived data.

Full extraction is proven for script and map data and **still unproven for `.c`-defined tables**:
the compiler emits those from source, so excluding them needs `-fdata-sections` plus per-symbol
section stripping rather than a linker flag. Tracked for Phase 7.

## 6. The virtual GBA

### 6.1 Memory map

One contiguous arena, carved into the GBA's regions:

| Region | Size | Purpose |
| --- | --- | --- |
| EWRAM | 256 KiB | the game's main heap and save blocks |
| IWRAM | 32 KiB | fast scratch, IRQ stacks |
| I/O | 1 KiB | the register file |
| Palette | 1 KiB | 256 BG + 256 OBJ colours |
| VRAM | 96 KiB | tiles, tilemaps, sprite graphics |
| OAM | 1 KiB | 128 sprite entries |
| Cart | 16 MiB | the loaded ROM image ([§5](#5-game-data)) |

Addresses are *relocated*, not mapped at the GBA's real addresses — that trick fails on macOS and
iOS, where the low 4 GiB is reserved. Region macros in the shadow `defines.h` resolve to offsets
inside the arena. The 38 raw address literals in upstream `src/` are handled individually.

`EWRAM_DATA` and `IWRAM_DATA` become no-ops: the host linker places those variables wherever it
likes. Nothing depends on their addresses, only their contents.

### 6.2 Register file and I/O

A plain 1 KiB array, so the overwhelming majority of register writes are just memory writes and
cost nothing. The PPU reads the register file when it renders rather than being notified on write.

Only registers with an *immediate side effect* are intercepted, by routing them through a function
in the shadow `macro.h`: the DMA control registers (writing the enable bit starts a transfer) and
`REG_IME`/`REG_IE`/`REG_IF`.

### 6.3 PPU

A scanline renderer, resolution-parametric from its first line of code — widescreen is cheap now
and expensive to retrofit. It never hardcodes 240 or 160; it reads `agb_ppu_width()` /
`agb_ppu_height()`.

Per scanline it composes, in the GBA's order: four backgrounds (text and affine), the object layer
from OAM, two windows plus the object window, then colour special effects (alpha blend, brightness
increase/decrease) and mosaic. Output is a 32-bit XRGB framebuffer handed to the host layer.

**Implemented: every mode and every layer feature.** Text backgrounds:
all four layers, 4bpp and 8bpp, both flips, all four map sizes with their multi-block layouts,
priority ordering front-to-back, and forced blank. Affine backgrounds: all four map sizes, the
20.8 reference point, per-scanline matrix stepping, and both overflow behaviours. Objects: all
twelve sizes, 4bpp and 8bpp, both flips, 1D and 2D tile mapping, the 256-line vertical wrap,
priority against the backgrounds and against each other, and both affine modes.

Which layers exist is decided by the display mode — mode 0 is four text layers, mode 1 is two text
plus BG2 affine, mode 2 is BG2 and BG3 affine — and a layer its mode does not define is not drawn
at all. An affine map is square, always 8bpp, and one byte per entry: a tile number with no flip
bits and no palette bank.

**Windows** resolve once per scanline into a per-pixel set of the layers allowed to draw there.
The regions are tried in a fixed order — window 0, then window 1, then the object window, then
everything outside all of them — and the first match owns the pixel, so an overlap belongs to the
earlier window. Inside and outside are independent control bytes rather than one and its inverse.
A window whose end lies past the screen, or before its start, is garbage that reads as the far
edge. With no window enabled in `DISPCNT` there is no masking at all, whatever the control
registers hold.

The object window is why the object pass runs before the mask is resolved: objects in the window
graphics mode contribute shape rather than colour, and their opaque texels *are* the region.

**Colour effects** are why composition collects the two frontmost layers per pixel rather than
stopping at the first: a blend needs whatever sits under the top layer, and front-to-back with
first-writer-wins cannot supply it. Anything below those two cannot affect the result, so two is
all that is kept. Alpha blending needs the top layer to be a first target *and* the one under it a
second target; the brightness effects need only the first. A semi-transparent object asks to be
blended whatever the effect register selects and outranks it, but still only where something
underneath is a second target — failing that it falls back to whatever the register wanted. Every
effect is gated by the colour-effect bit of the window covering the pixel.

Objects resolve into a scanline buffer before any background is drawn, because which object owns a
pixel is settled among the objects alone — lowest priority value wins, and OAM order breaks a tie.
Only that winner then competes with the backgrounds, at the priority it carries. Objects in the
OBJ-window graphics mode are skipped rather than drawn, which is what hardware does with them too.

An affine object replaces its two flip bits with a 5-bit selector into 32 matrix groups, each
interleaved into the unused fourth halfword of four OAM entries. Rendering runs the transform
backwards — screen offset from the centre of the object's box, through the matrix, into texture
space — so a source coordinate landing outside the object clips rather than sampling a neighbour.
The double-size mode grows that box, not the object: a rotated sprite needs the corners its own
box cannot hold.

**Mosaic** snaps a coordinate back to the start of its block, so the pixel there repeats across the
rest. Backgrounds and objects have separate sizes in the one register and separate enables — a
`BGCNT` bit per background, an OAM attribute bit per object — and a size field holds one less than
its block, so zero means a block of one pixel and no mosaic at all. Objects snap in their own space,
before a flip turns the coordinate around, and always draw at the unsnapped screen position.

**The bitmap modes** replace BG2's tiles and map with a frame buffer, transformed by the same
matrix — mode 3 is one full-screen 16-bit frame, mode 4 drops to 8-bit paletted and gains a second,
mode 5 keeps 16-bit and shrinks to 160×128 to afford one. A frame buffer does not wrap: off its
edge nothing is drawn. Direct colour has no transparent index, so every pixel of a mode 3 or 5
frame is opaque, while mode 4 keeps index 0 transparent as everywhere else. These modes define BG2
and nothing else, and they take half the object tile region, so objects below tile 512 have nothing
to draw from.

**The whole PPU is now implemented.** Nothing in the renderer is skipped. That was the point of the
rule while it was being built — a feature that was absent read as absent, so the picture never
lied about what existed.

**The scanline interrupts are raised from inside the composition loop.** Each line sets `VCOUNT`,
matches it against `DISPSTAT` and raises V-count if asked, draws, then sets the H-blank flag and
raises H-blank. Every layer re-reads its registers per line, so a handler writing scroll, window or
blend registers changes the picture from the following line down — which is exactly what the game's
battle transitions do, and the only reason they can work. Forced blank stops the picture, not the
interrupts.

One divergence to know about: an affine background's reference point is recomputed per line from
`BGxX`/`BGxY` rather than latched at the top of the frame and stepped. A handler that writes those
mid-frame therefore acts as though it had written them before the frame began. Fixing it needs the
internal registers to be latched, and writes to them intercepted, as the DMA control registers
already are.

The PPU composes on the game thread at V-blank, immediately after the game's own handler, so the
register writes and DMA copies that handler performs appear in the same frame. The host thread
copies the finished buffer out to present; a torn copy costs one frame of tearing and never blocks
the game.

Rendering is software, deliberately ([ADR 0005](adr/0005-sdl3-software-ppu.md)): the GBA's blending
and window rules are an accuracy risk on the GPU, the renderer is nowhere near fill-rate bound, and
determinism is what makes golden-image testing viable. GPU work — upscaling, LCD filters, shaders —
happens above the framebuffer.

### 6.4 DMA

Four channels. Immediate transfers run synchronously when the enable bit is written, covering the
great majority of use. Beyond that: HBlank-triggered DMA drives per-scanline effects and must fire
from inside the PPU's scanline loop; FIFO DMA feeds the sound mixer.

### 6.5 Interrupts and the frame loop

The subtlest part of the port. Full rationale: [ADR 0009](adr/0009-preemptive-interrupts.md),
which supersedes the fiber mechanism originally proposed in
[ADR 0004](adr/0004-fiber-frame-loop.md).

The game waits in two different ways, and only one of them can yield:

- `VBlankIntrWait` calls into us and can wait **cooperatively**.
- `AgbMain`'s loop ends in a **busy-wait** on `gMain.intrCheck`, a plain memory flag set only by
  the V-blank handler. It calls nothing, so there is no yield point at all.

The second is why interrupts are delivered by **signal** rather than by a cooperative switch. A
periodic timer at 59.7275 Hz raises `SIGALRM`; the handler dispatches through the game's own
`gIntrTable`, honouring `REG_IME` and `REG_IE` as the BIOS vector would. `gIntrTable` is ordered by
the vector's scan priority rather than by IE bit, so slot → flag needs an explicit mapping.

A signal handler runs on the interrupted context's **own stack** — game code is paused, not run
alongside — which is exactly hardware's behaviour, and is why this introduces none of the races a
second thread would.

Everything the handler can reach must be async-signal-safe. The deferred-subsystem reporter uses
`write()` rather than `stdio`, because taking the stdio lock inside a handler while the interrupted
context already holds it deadlocks.

The clock runs at the GBA's true 59.7275 Hz, not 60. Measured: 1200 frames in 20.10 s against
20.09 s expected.

**Determinism is an open problem**, not a solved one: interrupts arrive on wall-clock time, so the
point in game code where a frame boundary lands varies. Boundaries that land inside the idle spin
are deterministic in effect; ones landing mid-callback are not. The phase 6 harness will need a
headless clock that advances only at known-safe points.

### 6.6 BIOS

`libagbsyscall.s` becomes C. The arithmetic entry points (`Div`, `Sqrt`, `ArcTan2`, `BgAffineSet`,
`ObjAffineSet`) must reproduce the BIOS's exact results including its rounding quirks, because game
logic depends on the values — unit-tested against known vectors rather than trusted.
`CpuSet`/`CpuFastSet` honour the fixed-source and 16/32-bit control bits. `LZ77UnComp`, `RLUnComp`
and `HuffUnComp` are exercised against real game assets.

### 6.7 Audio

`m4a.c`, the sequencer, is already C upstream and needs no override. One statement stops it
*compiling* — `asm("swi 0x2A")` inside `MusicPlayerJumpTableCopy`, asking the BIOS to fill the
sequencer's dispatch table — and **nothing in this game calls that function**: the table is filled by
`MPlayJumpTableCopy`, which we supply. Removing that one statement from the preprocessed copy makes
it build and link, which keeps 1781 lines of sequencer receiving upstream decomp fixes. It does not
yet make it *run*: three separate dependencies stand behind it, set out in
[ROADMAP phase 4](ROADMAP.md#phase-4--it-sounds), and one of them — following a pointer stored inside
ROM data — belongs to phase 7.

`m4a_1.s` — 1917 lines of ARM across 36 routines — is reimplemented in C across
`platform/agb/src/m4a_mixer.c` and `platform/agb/src/m4a_track.c`. It is two subsystems, not one: a
track interpreter (`MPlayMain`, `TrackStop` and 24 `ply_*` opcode handlers) and the mixer proper
(`SoundMain`, `SoundMainRAM`, `SoundMainBTM`, `m4aSoundVSync`).

It is **translated rather than rewritten**, so the result can be checked against a known-good
emulator sample for sample, the way the PPU is checked frame for frame. Where the original relies on
something a reader would take for a mistake, the comment says so and the code keeps the behaviour —
the envelope's master-volume fold reaches a `SoundInfo` byte through a `SoundChannel` offset, and
that is load-bearing.

**Done so far: the envelope and both mixing paths** — attack, decay, sustain, release, the
pseudo-echo tail, the volume fold, and the two ways a wave is walked. Fixed-frequency plays it at
its own rate, one sample per output sample. The pitched path resamples, stepping a 9.23 fractional
position by the sound header's `divFreq` times the channel's frequency and interpolating between
neighbouring samples; the position carries across frames in the channel, and overrunning a loop
rewinds however many loop lengths it takes to land back inside.

A frame's buffers are prepared before any channel reaches them: cleared, or seeded with a reverb
that sums both sides of this frame with both sides of another — the frame ahead, or the start of the
area when the DMA counter says this is the last one. Which frame of the area is written comes from
that same counter, so the mixer stays ahead of what the hardware is reading out.

The scanline budget in the original — bail out of the channel loop once `VCOUNT` passes a deadline —
is **not reproduced, because it cannot fire**: `gMaxLines` is defined as absolute zero in every one
of upstream's linker scripts, and zero means no maximum. Reproducing it would have meant giving our
`VCOUNT` a meaning during V-blank that it does not have.

The mixing accumulate **wraps at eight bits rather than clamping**. The original keeps four output
samples packed in a register and rotates them past an accumulator, masking so one sample's low bits
cannot reach its neighbour; nothing in that arrangement saturates, so a loud mix distorts the way
the hardware does rather than flattening against a ceiling.

The **track interpreter** is the other half of `m4a_1.s`, in `platform/agb/src/m4a_track.c`. Its
handlers keep the game's own names rather than an `agb_` prefix, deliberately: they are the symbols
the sequencer's jump table names, and this is what supplies them. Each takes the player and the
track, reads its operands from the track's command stream, and sets flags saying what it
invalidated — the volume or the pitch has to be recomputed before the next note sounds.

The sequencer's dispatch table is filled by `MPlayJumpTableCopy` from a template in the game's own
`m4a_tables.c`. On hardware that template lives in the BIOS ROM and every entry is bounds-checked
against it; here it is an ordinary array, so the copy is just a copy. `SoundInit` fills the table
before `MPlayExtender` overrides nine of its entries, so the order matters and is preserved.

**The mixer driver.** `SoundMain` is what the sequencer calls once a frame: it takes the same kind
of lock the sequencer does, drives the chain of music players, then the compatible-sound
oscillators, and finally mixes every channel into whichever frame of the PCM area the sound DMA is
not reading out. Each channel steps its envelope and is then handed to the path its tone type asks
for — fixed-frequency, or resampled.

Upstream reaches the mixing loop by **copying its own machine code into IWRAM** and jumping there,
because IWRAM is faster than ROM. That is a hardware optimisation with no meaning on a host — and a
copied x86 function would not survive it, since its relative calls would resolve to the wrong
targets — so `SoundMain` calls the loop directly. It keeps our own name, `agb_m4a_mix_frame`, for
that reason: `SoundMainRAM` names a block of bytes upstream relocates, and this is not that.

`SoundMainBTM` is not a mixer routine at all despite the name — it is the 64-byte clear that
`m4a.c`'s `Clear64byte` dispatches to as `gMPlayJumpTable[35]`. It lives in its own file because
upstream's header declares it `void SoundMainBTM(void)` while it in fact receives a pointer; nothing
upstream noticed, because it is only ever reached through a table of unprototyped entries.

**Reversed and compressed waves are not mixed.** A channel whose tone type asks for one is skipped
with a warning rather than mixed as though its wave were ordinary — audibly wrong beats quietly
wrong. Walking the ROM's instrument tables says what this costs: of 66 tone tables reachable from the
song table, **two instruments are reversed** (voices 1 and 33 of one table) and **none is
compressed**.

**The interpreter half of `m4a_1.s` is fully translated.** The parameter opcodes — priority, tempo, key shift, instrument
select, volume, pan, bend, bend range, tune, both modulation controls and the delay, and the direct
write to a compatible-sound register; control flow — the jump, the three-deep pattern call stack, the
repeat counter and the end of a track; and then `ply_note` and `MPlayMain`.

Ending a track releases the channels it owns rather than cutting them off, so their envelopes
finish, and unlinks each from the chain the track keeps. The chain's head lives in the track rather
than in a channel, which makes losing the first one a separate case from losing a middle one.

Stopping a track is the other case: it cuts its channels off outright rather than releasing them,
so they are free again at once, and a compatible-sound channel is told to switch its oscillator off
because that is hardware rather than something we mix. Ending a tie releases the first channel still
holding the key and only that one.

The per-frame tick, `m4aSoundVSync`, counts down to the moment the sound DMA needs the next buffer
and re-arms the two FIFO channels when it arrives. **The re-arming has no audible effect here** —
the host consumes the mixer's PCM buffer directly rather than through a sound FIFO — but the
registers are written anyway, because the game can read them back and the cost is nothing.

**Starting a note** is `ply_note`, the largest single routine in the file. Its operand is an index
into the game's clock table; up to three more bytes may follow — key, velocity, added gate time —
each optional and each recognised only by being below `0x80`, so a note that supplies none of them
repeats the previous one. The added gate time is stored into a byte without widening, but it cannot
overflow: the largest clock entry is 96 and an operand is at most 127.

Which instrument actually sounds takes one level of indirection, and only one. A **key-split**
instrument redirects through a table of entry numbers; a **rhythm** instrument indexes by key
directly and the entry it lands on carries its own key, and optionally its own pan. If the entry is
itself a split or a rhythm the note is dropped. The two pointers involved sit at different offsets of
the same instrument — the entries where a plain instrument keeps its waveform, the split table where
it keeps its envelope — and neither has a field in the C struct, so both are reached through the
offsets upstream's assembler names.

**Channel allocation** is where a note is won or lost. A mixed note takes the first idle channel it
finds, and only if there is none does it look for a victim: a releasing channel is always a better
one than a sounding channel however unimportant the sounding one looks, then the lowest priority
wins, and an exact tie is settled on track address so the choice does not depend on iteration order.
The search begins holding the newcomer's own priority and track, which is what stops it from
stealing a channel that outranks it on either count — with everything busy and more important, the
note is simply dropped. A compatible-sound note has exactly one channel it may play on, chosen by
type, and the same rules decide whether it may take it.

The channel is then unhooked from wherever it was and put at the head of the track's chain. Upstream
copies gate time, key, velocity and running status across as a single word, which lands the track's
running status in the channel's priority — immediately overwritten by the computed one. Here the four
are assigned separately, which is identical in effect and says what it means.

**The driver**, `MPlayMain`, is what a frame of music actually is. It begins by taking a lock: the
player's `ident` doubles as a signature and a busy flag, so a player caught mid-update is left alone
and the flag is released however the call leaves. Players form a chain, and each drives the next
before doing its own work, so the whole chain is serviced from one call on the head.

How many sequencer ticks a frame is worth comes from a **tempo accumulator** — it climbs by the
player's increment and spends 150 per tick, carrying the remainder into the next frame. Each tick
walks every track: the channels it owns are counted one step closer to their release, a track flagged
as just started is given its defaults, and its command stream runs until the wait counter is
non-zero.

A command byte splits three ways at exact boundaries: from `0xCF` up it is a note, `0xB1` to `0xCE`
indexes the dispatch table, and `0x80` to `0xB0` is a wait taken from the clock table. Below `0x80`
it is not a command at all but the operands of the previous one — running status — and the driver
leaves the pointer sitting on it for the handler to consume. Only commands from `0xBD` up are
remembered that way.

Then the **modulation sweep** steps, as a triangle: a counter climbs and the half of its range past
the midpoint is read back down. The counter is kept in a byte but the sum is not truncated before it
is used, so a large speed folds past the midpoint against the wide value — kept, because it is
audible. A step that lands where the sweep already was invalidates nothing.

What the ticks invalidated is recomputed **once at the end** rather than inside each handler that
invalidated it: a track may be told to change volume several times in one tick and its channels only
need to hear about it once. That pass consumes the flags, which is why the flag itself is never
observable after the call — only its effect is.

Two of upstream's shapes are preserved rather than tidied. Both track loops are `do`/`while`, so a
player claiming **no tracks still has its first one run**. And a track whose handler ended it mid-tick
is dropped immediately, without spending its wait or stepping its modulation, though it still counted
as alive when the tick began.

`ply_note` and `MPlayMain` both reach into the sequencer — for `ClearChain`, `TrkVolPitSet`,
`MidiKeyToFreq`, `FadeOutBody` and `Clear64byte` — so both are inert until step 5 builds `m4a.c`.
They are tested against handlers the test supplies, the way the PPU tests supply the interrupt table.

The mixer produces the GBA's native ~13.4 kHz PCM into a ring buffer; the host layer resamples.
Mixing is driven from the frame loop, not the audio callback, so audio stays in lockstep with game
state.

### 6.8 Save data

`agb_flash*.c` emulate 128 KiB of flash backed by a host file, byte-identical to the `.sav` format
emulators produce, so saves move in and out of mGBA and off real hardware without conversion. The
path comes from `host_pref_dir()`.

### 6.9 Serial and link play

Peer-to-peer link play — trades and battles — is a committed feature, so the serial layer is
designed for a real transport rather than permanently stubbed.

The game drives serial through registers and the RFU wireless adapter. Our serial layer presents
the same register behaviour and carries the payload over `host_net`. Link play is *lockstep*: both
peers must agree on every frame, which is why the deterministic fiber loop is a prerequisite rather
than a nicety, and why desync fuzzing is part of the test plan.

Until the transport lands, the layer reports "no peer connected" — a state the game already handles
gracefully.

## 7. The host abstraction

`host.h` is the whole porting surface. A new platform implements it and nothing else:

| Group | Responsibility |
| --- | --- |
| video | create a window/surface, present an RGBA framebuffer, report the safe area |
| audio | open an output stream, consume the mixer's ring buffer |
| input | a frame's button state, from keyboard, gamepad or touch |
| vfs | open assets, read files, resolve the writable preference directory |
| clock | monotonic time and frame pacing |
| fiber | create and switch execution contexts |
| net | datagram transport for link play |
| log | leveled diagnostics |

Two implementations: `sdl3` (desktop, Android and iOS all come from this one) and `null` (headless,
for tests and CI). Both are built; the port selects one with `FRLG_HOST_BACKEND`.

**Threading.** SDL owns the main thread, because macOS and iOS require windowing there. The game
runs on its own thread, with `SIGALRM` unblocked only on that thread so the frame timer preempts
game code and nothing else.

Two ordering rules govern that mask, and both were learned by getting them wrong:

- **The game thread must unblock explicitly.** A new thread inherits its creator's mask, so
  blocking on the main thread before `pthread_create` blocks it everywhere and no frame advances.
- **The block must happen before anything else starts a thread** — in particular before SDL is
  initialised. `ITIMER_REAL` is delivered to *any* thread that has not blocked it, and SDL's
  backend threads inherit whatever mask was in force when they were created. Block after
  `SDL_Init` and the V-blank handler eventually runs game code on an SDL thread, concurrently with
  the game thread. That presents as a hang deep inside unrelated game code, and never reproduces
  headless.

The main thread is the hardware side: it pumps events, writes the key register and presents the
framebuffer. It never runs game code, so it races with nothing — and a real GBA updates its key
register asynchronously too. This is not in tension with
[ADR 0009](adr/0009-preemptive-interrupts.md), which rejects threads for *interrupt delivery*;
handlers still run on the game thread's own stack.

**Known layering debt.** `platform/agb/src/frame.c` calls `setitimer` and `sigaction` directly, so
the AGB layer currently depends on POSIX — which [§2](#2-layer-model) says it must not. It builds
everywhere POSIX exists, so nothing is blocked today, but Windows and the web target cannot work
until the timer and the preemption primitive move behind `host.h`. Tracked for phase 8.

The main thread is the hardware side: it pumps events, writes the key register and presents the
framebuffer. It never runs game code, so it races with nothing — and a real GBA updates its key
register asynchronously too. This is not in tension with
[ADR 0009](adr/0009-preemptive-interrupts.md), which rejects threads for *interrupt delivery*;
handlers still run on the game thread's own stack.

## 8. Mods

An embedded Lua layer. Design and rules: [ADR 0007](adr/0007-lua-mod-registries.md).

**One schema table is the source of truth** for every registry — its merge semantics (`record`,
`deep`, `compose`), the data path it writes, and the value schema every registration is validated
against. The loader is built from that table and the reference documentation is generated from it,
so engine and docs cannot drift. Registrations come in four modes: `register`, `override`, `patch`,
`remove`.

Mods act at three levels:

- **Data** — patch the cart region after import, before the game starts.
- **Hooks** — register callbacks on engine events.
- **Render pipelines** — own a display mode outright, layered above the framebuffer. This is the
  seam that makes an alternative renderer a mod rather than a fork.

Two rules are non-negotiable, adopted from prior art because they are hard-won:

- **A callback that throws retires only its own feature**, attributed to its mod; the frame falls
  back to vanilla. A broken mod costs a display mode, never the game.
- **Availability is re-read every frame**, so a pipeline that cannot run headless simply does not.

Lua rather than LuaJIT: iOS forbids JIT, and performance-critical work is already in C.

## 9. Launcher, packaging and updates

The launcher owns everything around the game rather than inside it: first-boot ROM import, mod
management and profiles, update checks, and direct-launch shortcuts for Steam entries and handheld
frontends.

It is a separate layer above the game, not a mode of it, so a corrupted mod set or a failed update
never blocks booting.

Packaging is per-platform and lives under `ports/`. Release pipelines are built early rather than
retrofitted — shipping a binary is only possible at all because we ship no game data ([§5](#5-game-data)),
so distribution is a first-class concern from the start.

## 10. Ports

Each directory under `ports/` holds only what is irreducibly platform-specific: the entry point,
the packaging manifest, store metadata. **No game logic and no hardware logic lives here.**

## 11. Build system

CMake ≥ 3.24 with Ninja, driven by `CMakePresets.json`. Cross-compilation uses toolchain files in
`cmake/`; asset tools are always built for the host, never the target.

The game's sources cannot be compiled directly — upstream's `tools/preproc` must run over every
`.c` and `.s` first, to expand `_("…")` string literals into the game's character encoding and to
resolve `INCBIN`. CMake reproduces that per file: C preprocessor → `preproc` → compiler, with the
vendor tree as the working directory so `INCBIN` paths resolve.

The preprocessing and compilation steps **must be given the same `-std`**. Preprocessing at a newer
standard bakes host-header constructs into the output that the older standard then rejects — GCC 16
defaults to gnu23, whose `stddef.h` emits `typedef __typeof__(nullptr) nullptr_t;`, which fails
under `-std=gnu11`.

Binary assets in the *developer* data path are produced by roughly 40 KiB of rules across
upstream's five `*_rules.mk` files. **We do not reimplement those rules** — the build delegates to
upstream's Makefile and consumes the generated files. Reimplementing them would be a large
transcription with a large bug surface, for no benefit while upstream's version works and stays
correct as pret changes it.

The ROM build stays in CI, for two reasons. It is the **oracle** — when the port renders something
wrong, "what does the ROM do here" must stay answerable. And it **generates the manifest**
([§5.1](#51-the-manifest)), so it is load-bearing for shipping, not just for reference.

`MODERN=1` (modern GCC) does not reproduce the original ROM's checksum; byte-matching needs
`agbcc`, which is not currently installed here and is not required.

## 12. Pointer width

`data/*.s` contains **789 `.4byte` directives**, many of them symbol references — pointers embedded
directly in data blobs. The script VM reads them back with `ScriptReadWord`
(`vendor/pokefirered/src/script.c:188`) and casts the result to a pointer. On a 64-bit build those
truncate, and the script engine breaks at the foundations.

The strategy is **32-bit first, 64-bit ready** ([ADR 0003](adr/0003-pointer-width.md)):

- Phase A builds 32-bit — `-m32` on x86, `armeabi-v7a` on Android, `wasm32` on the web. Relocated
  host pointers fit the ROM's 4-byte slots, so the importer's relocation pass
  ([§5.3](#53-relocation)) is sufficient and game code is untouched.
- Every read of a pointer embedded in game data goes through an accessor from the first commit.
  While 32-bit, it is the identity function.
- The 64-bit migration changes the *data pipeline*: regenerate data into a native layout with wider
  slots, and make the accessor add the arena base. macOS and iOS come online there, since Apple
  platforms are 64-bit only.

## 13. Extension points

Designed now, built later. Each costs almost nothing today and would mean rewriting the PPU to
retrofit. The full list of intended features is [new-features.md](new-features.md).

Every one of these is a **layer over a reference configuration the port can always produce** —
native 240×160, one frame per logic tick, nothing enhanced ([ADR 0011](adr/0011-reference-configuration.md)).
That is what keeps the conformance comparison against mGBA alive past the point where the port
stops looking like a Game Boy Advance. An enhancement that cannot be switched off is a defect.

**Display pipelines.** LCD filters, colour modes and shader effects attach above the framebuffer as
post-process stages — the same seam mods use to own a display mode.

**Widescreen and higher internal resolution.** The renderer is resolution-parametric
([§6.3](#63-ppu)). Game logic still believes the screen is 240×160 and is not touched; the extra
area is background and object overdraw. Anything the game positions in screen space — UI,
textboxes, battle layouts — must be identified and anchored before this is switched on.

**Smooth scrolling / high refresh.** Logic stays locked to 59.7275 Hz; decoupling it would break
timing assumptions across 320k lines. The PPU instead becomes re-runnable between logic ticks with
interpolated scroll offsets, which requires it to retain the previous frame's scroll state — a
design constraint now.

**Asset hot-reload.** Every asset is reached through a resource-id → pointer table rather than a
direct symbol reference, letting the VFS substitute a file from a `data/` override directory. The
importer already populates that table ([§5.2](#52-the-cart-region-and-symbol-binding)), so the two
share a mechanism.

## 14. Testing

320k lines of untouched game code cannot be reviewed, and that is not where the bugs will be — they
will be in the hardware layer, where a one-line blend mistake is invisible in review and obvious on
screen. Strategy and thresholds: [ADR 0008](adr/0008-testing-strategy.md).

| Tier | What it covers |
| --- | --- |
| Unit | PPU rendering from hand-built register state; BIOS math against hardware-derived vectors; decompressors round-tripping real assets |
| Golden screenshots | PPU output vs mGBA frame dumps, two thresholds, side-by-side diff artifacts |
| Headless drivers | scripted scenarios on the `null` backend; an autopilot playthrough capturing shots |
| Regression | **one driver per fixed bug, named after it, committed with the fix** |
| Link | desync fuzzing across peers |
| Build | ROM build, native build, and the shadow/override drift check |

Tests live in `tests/` and are registered with CTest under two labels. `ctest -L unit` is the fast
path — the PPU suites and the golden harness's own threshold check, well under a second. `ctest -L
golden` runs the port for thousands of frames and takes about a minute. Plain `ctest` runs both.

Unit tests link the `agb` archive directly; the linker pulls in only the objects a test actually
references, so a PPU test costs nothing of the game library and needs none of the ROM pipeline.
They spell out the hardware layout they drive rather than importing the renderer's own constants —
a test built on the same macro as the code under test cannot catch that macro being wrong.

Golden screenshots use two independent thresholds: a per-channel tolerance absorbing harmless
driver drift, and a separate pixel budget that is what actually fails. A shot where one sprite moved
trips the budget; a shot half a shade darker trips neither. `tools/golden.py` captures each frame in
the manifest, compares it, and on failure writes a golden / actual / difference panel as a PNG.

**The goldens themselves are generated per machine and never committed** — they are frames of a
copyrighted ROM ([ADR 0010](adr/0010-goldens-are-generated.md)). `tests/golden/manifest.txt` is
committed and carries the frame numbers, thresholds and descriptions; `--bless` fills
`tests/golden/images/`, which is gitignored. A run with no goldens fails rather than passing
quietly, because an absent reference is an unproven frame.

The oracle is mGBA running our own reference ROM build. `tools/mgba_capture.c` drives it headlessly
and writes the same PPM the port's `FRLG_SHOT` does, so the harness reads either without knowing
which produced it; `--bless-reference` fills the goldens from it. It is a host tool built outside
the port's `-m32` world, because the system libmgba is 64-bit, and it is optional — without it the
tier still runs, blessed from our own renderer, as a regression net rather than an oracle.

mGBA's frame numbers run 38 ahead of ours, since it boots through the BIOS and `crt0` where the
port enters `AgbMain` directly, so the manifest carries a reference frame per row. At that offset
the port reproduces mGBA **exactly** on settled screens. Frames caught mid-transition carry `-`
instead: their backgrounds match, but scene pacing diverges wherever the game waits on the stubbed
audio subsystem, so animated sprites sit at a different phase
([spike 0004](spikes/0004-mgba-frame-alignment.md)).

CI gates expensive per-platform jobs behind change-detection jobs, and runs SDK-free self-tests
everywhere, so platform count stays affordable.

## 15. Conventions

- C11. No compiler extensions outside `platform/host`.
- Port code is `snake_case`, prefixed `agb_` or `host_`. The game's `PascalCase` namespace is left
  alone, so a symbol's origin is always obvious.
- Comments explain *why*, never *what*. Rationale belongs in this file; history belongs in git.
- OS conditionals only under `platform/host/` and `ports/`.
