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

Five `.c` files carry ARM inline assembly and cannot be compiled for a host target. **278 of the
283 game sources compile natively with no source change at all** — the exclusions are exactly
these, and every one was already destined for an override for an independent reason:

| Excluded | Assembly it carries | Already planned as |
| --- | --- | --- |
| `main.c` | IWRAM clear loop | override — the fiber frame loop ([§6.5](#65-interrupts-and-the-frame-loop)) |
| `script.c` | `svc 2` (HALT) | override — the pointer accessor ([§12](#12-pointer-width)) |
| `m4a.c` | `swi 0x2A` | override — the C mixer ([§6.7](#67-audio)) |
| `multiboot.c` | ARM busy-wait | GameCube link, out of scope |
| `librfu_intr.c` | naked ARM trampolines | RFU wireless, stubbed until link play ([§6.9](#69-serial-and-link-play)) |

That the exclusion list and the already-planned override list turned out to be the same five files
is the strongest evidence so far that the layering in [§2](#2-layer-model) cuts in the right place.

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
and expensive to retrofit. It never hardcodes 240 or 160.

Per scanline it composes, in the GBA's order: four backgrounds (text and affine), the object layer
from OAM, two windows plus the object window, then colour special effects (alpha blend, brightness
increase/decrease) and mosaic. Output is a 32-bit RGBA framebuffer handed to the host layer.

Rendering is software, deliberately ([ADR 0005](adr/0005-sdl3-software-ppu.md)): the GBA's blending
and window rules are an accuracy risk on the GPU, the renderer is nowhere near fill-rate bound, and
determinism is what makes golden-image testing viable. GPU work — upscaling, LCD filters, shaders —
happens above the framebuffer.

### 6.4 DMA

Four channels. Immediate transfers run synchronously when the enable bit is written, covering the
great majority of use. Beyond that: HBlank-triggered DMA drives per-scanline effects and must fire
from inside the PPU's scanline loop; FIFO DMA feeds the sound mixer.

### 6.5 Interrupts and the frame loop

The subtlest part of the port, and the reason `host.h` exposes fibers.
Full rationale: [ADR 0004](adr/0004-fiber-frame-loop.md).

`VBlankIntrWait` is called from deep inside nested game code, not only the top-level loop, and
HBlank handlers must run *between* scanlines while the game is blocked. A "call the game once per
frame" loop cannot express either.

So the game runs on its own **fiber**. The host drives:

1. Begin a frame; poll input into the key registers.
2. Per scanline: render it, and if the game enabled HBlank, switch to the game fiber for its
   handler and back.
3. At scanline 160, raise VBlank: switch to the game fiber, which returns from `VBlankIntrWait` and
   runs a frame of logic until it blocks again.
4. Present the framebuffer; mix audio.

Switching is explicit, so there are no data races on game state and headless runs are reproducible
frame for frame — the property the whole test strategy rests on.

The virtual clock runs at the GBA's true 59.7275 Hz, not 60.

### 6.6 BIOS

`libagbsyscall.s` becomes C. The arithmetic entry points (`Div`, `Sqrt`, `ArcTan2`, `BgAffineSet`,
`ObjAffineSet`) must reproduce the BIOS's exact results including its rounding quirks, because game
logic depends on the values — unit-tested against known vectors rather than trusted.
`CpuSet`/`CpuFastSet` honour the fixed-source and 16/32-bit control bits. `LZ77UnComp`, `RLUnComp`
and `HuffUnComp` are exercised against real game assets.

### 6.7 Audio

`m4a.c`, the sequencer, is already C upstream and used as-is. `m4a_1.s` — 1917 lines of ARM
implementing `SoundMain`, `SoundMainRAM` and the reverb path — is the mixer, reimplemented in C.

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
for deterministic tests and CI).

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
| Unit | BIOS math against hardware-derived vectors; decompressors round-tripping real assets |
| Golden screenshots | PPU output vs mGBA frame dumps, two thresholds, side-by-side diff artifacts |
| Headless drivers | scripted scenarios on the `null` backend; an autopilot playthrough capturing shots |
| Regression | **one driver per fixed bug, named after it, committed with the fix** |
| Link | desync fuzzing across peers |
| Build | ROM build, native build, and the shadow/override drift check |

Golden screenshots use two independent thresholds: a per-channel tolerance absorbing harmless
driver drift, and a separate pixel budget that is what actually fails. A shot where one sprite moved
trips the budget; a shot half a shade darker trips neither.

CI gates expensive per-platform jobs behind change-detection jobs, and runs SDK-free self-tests
everywhere, so platform count stays affordable.

## 15. Conventions

- C11. No compiler extensions outside `platform/host`.
- Port code is `snake_case`, prefixed `agb_` or `host_`. The game's `PascalCase` namespace is left
  alone, so a symbol's origin is always obvious.
- Comments explain *why*, never *what*. Rationale belongs in this file; history belongs in git.
- OS conditionals only under `platform/host/` and `ports/`.
