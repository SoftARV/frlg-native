# Architecture

This document explains every part of the project. It is the contract: if a commit changes what a
subsystem does, how layers talk to each other, or how the build works, it updates this file in the
same commit.

- [1. What this is](#1-what-this-is)
- [2. Layer model](#2-layer-model)
- [3. Repository layout](#3-repository-layout)
- [4. Upstream integration](#4-upstream-integration)
- [5. The virtual GBA (`platform/agb`)](#5-the-virtual-gba-platformagb)
- [6. The host abstraction (`platform/host`)](#6-the-host-abstraction-platformhost)
- [7. Ports (`ports/`)](#7-ports)
- [8. Build system and asset pipeline](#8-build-system-and-asset-pipeline)
- [9. Pointer width](#9-pointer-width)
- [10. Enhancement hooks](#10-enhancement-hooks)
- [11. Testing](#11-testing)
- [12. Conventions](#12-conventions)

---

## 1. What this is

`pret/pokefirered` is a complete decompilation: 320k lines of C, with only five `.s` files left
(`crt0`, `rom_header`, `libagbsyscall`, `libgcnmultiboot`, `m4a_1`). Nothing meaningful remains in
assembly.

That makes a **source port** the correct approach, and it rules out the alternatives:

| Approach | Verdict |
| --- | --- |
| Emulation | Rejected — ships a CPU interpreter and gains nothing we can enhance. |
| Static recompilation (N64Recomp-style) | Rejected — that technique exists for games *without* source. We have source. |
| **Source port** | **Chosen.** Compile the game for the host CPU; reimplement the hardware beneath it. |

The game code does not learn that it has been ported. It keeps calling `REG_DISPCNT`, `DmaCopy16`,
`CpuFastSet` and `VBlankIntrWait`. We supply those.

Two properties of the codebase make this tractable, and both were measured, not assumed:

- Hardware access is funnelled through five headers in `include/gba/`. Only **38** raw address
  literals exist in all of `src/`.
- `data/*.s` (event scripts, battle scripts, map data) uses only portable GAS directives —
  `.byte`, `.2byte`, `.4byte`, `.string`, `.align`, `.incbin`, `.section`. No ARM or Thumb
  directives at all, so the host assembler consumes these files unmodified.

## 2. Layer model

```
┌────────────────────────────────────────────────────────────────┐
│  vendor/pokefirered            game logic, 320k LOC, PRISTINE  │
│  thinks it is running on a Game Boy Advance                    │
└───────────────────────▲────────────────────────────────────────┘
                        │  GBA hardware API
                        │  (shadowed headers + link-time overrides)
┌───────────────────────┴────────────────────────────────────────┐
│  platform/agb                  the virtual GBA                 │
│  memmap · io · ppu · dma · irq · timer · bios · m4a · flash    │
│  NO OS CONDITIONALS — identical code on every target           │
└───────────────────────▲────────────────────────────────────────┘
                        │  host.h  (one narrow interface)
┌───────────────────────┴────────────────────────────────────────┐
│  platform/host                 backends                        │
│  video · audio · input · vfs · clock · fiber · log             │
│  sdl3 implementation · null implementation (headless tests)    │
└───────────────────────▲────────────────────────────────────────┘
                        │
┌───────────────────────┴────────────────────────────────────────┐
│  ports/desktop · ports/android · ports/ios · ports/web         │
│  entry point, packaging, manifests, store metadata             │
└────────────────────────────────────────────────────────────────┘
```

**The dependency rule is strictly downward.** No layer may reference anything above it. The PPU
never calls SDL; it writes into a framebuffer that the host layer later presents. This is what
keeps a new platform to "implement `host.h`" rather than "port the renderer".

The single most important consequence: **`platform/agb` contains zero `#ifdef _WIN32` /
`__ANDROID__` / `__APPLE__`.** Every OS conditional lives under `platform/host/` or `ports/`. If a
platform difference is leaking into the AGB layer, the `host.h` interface is wrong and gets fixed
there instead.

## 3. Repository layout

```
frlg-native/
├── vendor/pokefirered/          pinned submodule, never edited
├── platform/
│   ├── agb/
│   │   ├── include/gba/         shadow headers that replace upstream's five
│   │   ├── include/agb/         our own interfaces (memmap, ppu, irq, …)
│   │   ├── src/                 the hardware reimplementation
│   │   └── overrides/           .c files that replace an upstream .c
│   └── host/
│       ├── include/host.h       the entire host interface
│       └── src/sdl3/            SDL3 backend
│       └── src/null/            headless backend for tests
├── ports/desktop/               main(), packaging
├── cmake/                       toolchains, asset pipeline, drift check
├── tools/                       our build-time tools (drift check, converters)
├── tests/
└── docs/
```

## 4. Upstream integration

`vendor/pokefirered` is pinned at commit `c75f3523` and **is never modified**. pret continues to
fix decomp bugs; we want those for free. Every deviation is therefore expressed in one of exactly
three ways, all of them explicit and greppable.

### 4.1 Shadow headers

CMake places `platform/agb/include` ahead of `vendor/pokefirered/include` on the include path. Five
headers are fully replaced:

| Header | Why it is replaced |
| --- | --- |
| `gba/defines.h` | Region base addresses point into our arena; `EWRAM_DATA`/`IWRAM_DATA` become no-ops. |
| `gba/io_reg.h` | `REG_BASE` points at our register file. |
| `gba/macro.h` | `DmaSet` and friends become calls into our DMA engine, not raw register writes. |
| `gba/syscall.h` | BIOS calls become ordinary C function declarations. |
| `gba/types.h` | `vu16`/`vs32` etc. keep their meaning but drop GBA-specific assumptions. |

The other ~212 upstream headers are used verbatim.

Shadowing is invisible at the call site, which is its risk: if pret edits one of those five
headers, we would silently diverge. `tools/check_drift.py` records the upstream hash of every
shadowed header and every overridden `.c`, and CI fails when one moves. Bumping the submodule pin
is always its own commit and always re-runs the check.

### 4.2 Overrides

A handful of upstream `.c` files describe hardware we cannot reproduce by redefining a macro —
the interrupt dispatcher, save flash, the link cable. For those, CMake excludes the upstream file
from the source list and compiles `platform/agb/overrides/<name>.c` instead.

Every override is listed in the table below, with the reason and the upstream file it forked from.
**Adding an override without adding a row here is a defect.**

| Override | Replaces | Reason |
| --- | --- | --- |
| _(none yet)_ | | |

Overrides are a cost, not a tool of convenience: each one is a file that stops receiving upstream
fixes. Prefer a shadow macro; reach for an override only when there is no macro seam.

### 4.3 Files simply not built

`crt0.s`, `rom_header.s`, `libagbsyscall.s`, `libgcnmultiboot.s` and `m4a_1.s` describe cartridge
boot, the BIOS ABI and the ARM sound mixer. None apply to a native binary. They are dropped from
the source list; `libagbsyscall` and `m4a_1` are reimplemented in C under `platform/agb/src/`.

## 5. The virtual GBA (`platform/agb`)

### 5.1 Memory map

One contiguous arena is allocated at startup, carved into the GBA's regions:

| Region | Size | Purpose |
| --- | --- | --- |
| EWRAM | 256 KiB | the game's main heap and save blocks |
| IWRAM | 32 KiB | fast scratch, IRQ stacks |
| I/O | 1 KiB | the register file |
| Palette | 1 KiB | 256 BG + 256 OBJ colours |
| VRAM | 96 KiB | tiles, tilemaps, sprite graphics |
| OAM | 1 KiB | 128 sprite entries |

Addresses are *relocated*, not mapped at the GBA's real addresses. Region macros in the shadow
`defines.h` resolve to offsets inside the arena, so no `mmap` at fixed low addresses is needed —
that trick fails on macOS and iOS, where the low 4 GiB is reserved. The 38 raw address literals in
upstream `src/` are handled individually and each one is recorded in the override table when it
forces an override.

`EWRAM_DATA` and `IWRAM_DATA` become no-ops: the host linker places those variables wherever it
likes. Nothing in the game depends on their addresses, only on their contents.

### 5.2 Register file and I/O

The register file is a plain 1 KiB array, so the overwhelming majority of register writes are just
memory writes and cost nothing. The PPU reads the register file when it renders rather than being
notified on write.

Only registers with an *immediate side effect* are intercepted, by routing them through a function
in the shadow `macro.h` instead of a raw store: the DMA control registers (writing the enable bit
starts a transfer), and `REG_IME`/`REG_IE`/`REG_IF` (interrupt masking).

### 5.3 PPU

A scanline renderer, written to be resolution-parametric from the first line of code — the
widescreen goal is cheap now and expensive to retrofit. It never hardcodes 240 or 160; it reads
`agb_ppu_width()` / `agb_ppu_height()`.

Per scanline it composes, in the GBA's order: the four backgrounds (text and affine modes), the
object layer from OAM, the two windows plus the object window, then colour special effects
(alpha blend, brightness increase/decrease) and mosaic. Output is a 32-bit RGBA framebuffer handed
to the host layer.

Rendering is software, deliberately. A GPU implementation of the GBA's blending and window rules
is a large accuracy risk for a game that is not fill-rate bound at this resolution. GPU-side
upscaling and shader filters belong in the host layer, above the framebuffer.

### 5.4 DMA

Four channels. Immediate transfers run synchronously the moment the enable bit is written — this
covers the great majority of use, since `DmaCopy16`/`DmaFill32` are how the game moves everything
into VRAM.

The channels that matter beyond that are the timed ones: HBlank-triggered DMA is how the game
produces per-scanline effects, and it must fire from inside the PPU's scanline loop; FIFO DMA
feeds the sound mixer. Both are driven by the frame loop below, not by the game.

### 5.5 Interrupts and the frame loop

This is the subtlest part of the port, and the reason for the fiber dependency in `host.h`.

The game is written as `while (1) { callback1(); callback2(); VBlankIntrWait(); }`, but
`VBlankIntrWait` is *also* called from deep inside nested game code, and HBlank interrupt handlers
must run *between* scanlines while the game is blocked. A simple "call the game once per frame"
loop cannot express that.

So the game runs on its own **fiber**. The host drives:

1. Host loop begins a frame; polls input into the key registers.
2. For each scanline: render it, then — if the game enabled the HBlank interrupt — switch to the
   game fiber to run its handler, and switch back.
3. At scanline 160, raise VBlank: switch to the game fiber, which returns from `VBlankIntrWait`
   and runs a full frame of game logic until it blocks again.
4. Present the framebuffer, mix audio.

Fibers rather than a thread: switching is explicit and deterministic, so there are no data races
on game state and a headless run is reproducible frame for frame. `host.h` exposes fiber
create/switch, implemented per platform (`ucontext` on POSIX, Fibers on Windows, Asyncify on the
web).

The virtual clock runs at the GBA's true 59.7275 Hz, not 60.

### 5.6 BIOS

`libagbsyscall.s` becomes C. The arithmetic entry points (`Div`, `Sqrt`, `ArcTan2`,
`BgAffineSet`, `ObjAffineSet`) must reproduce the BIOS's exact results, including its rounding and
its quirks, because game logic depends on the values — these are unit-tested against known
vectors rather than trusted. `CpuSet`/`CpuFastSet` become `memcpy`-class helpers honouring the
fixed-source and 16/32-bit control bits. `LZ77UnComp`, `RLUnComp` and `HuffUnComp` are
straightforward decompressors, exercised against real game assets.

### 5.7 Audio

`m4a.c` — the sequencer that reads the song format — is already C upstream and is used as-is.
`m4a_1.s`, 1917 lines of ARM implementing `SoundMain`, `SoundMainRAM` and the reverb path, is the
mixer, and is reimplemented in C.

The mixer produces the GBA's native ~13.4 kHz PCM into a ring buffer; the host layer resamples to
whatever rate the device wants. Mixing is driven from the frame loop rather than the audio
callback, so audio stays in lockstep with game state.

### 5.8 Save data

`agb_flash*.c` emulate 128 KiB of flash backed by a host file. The on-disk layout is kept
byte-identical to the `.sav` format emulators produce, so saves move in and out of mGBA and off
real hardware without conversion. The path comes from `host_pref_dir()`.

### 5.9 Link cable

Serial and the RFU wireless adapter are stubbed to "no peer connected" — the states the game
already handles gracefully when nothing is plugged in. Real multiplayer is a post-1.0 subject and
is out of scope for the architecture as described here.

## 6. The host abstraction (`platform/host`)

`host.h` is the whole porting surface. A new platform implements it and nothing else:

| Group | Responsibility |
| --- | --- |
| video | create a window/surface, present an RGBA framebuffer, report the safe area |
| audio | open an output stream at a requested rate, consume the mixer's ring buffer |
| input | report a frame's button state, from keyboard, gamepad or touch |
| vfs | open assets, read files, resolve the writable preference directory |
| clock | monotonic time and frame pacing |
| fiber | create and switch execution contexts |
| log | leveled diagnostics |

Two implementations exist: `sdl3` (desktop, Android and iOS all come from this one), and `null`
(headless, for deterministic tests and CI).

SDL3 rather than SDL2: it is the version with a supported mobile story, a modern GPU abstraction
and a sane audio stream API — and it is what is installed here (3.4.14).

## 7. Ports

Each directory under `ports/` holds only what is irreducibly platform-specific: the entry point,
the packaging manifest and store metadata. `ports/desktop` is a `main()` and a CMake install rule.
Android adds a Gradle project and an activity; iOS adds an Xcode target and `Info.plist`; web adds
an Emscripten shell. **No game logic and no hardware logic lives here.**

## 8. Build system and asset pipeline

CMake ≥ 3.24 with Ninja, driven by `CMakePresets.json`. Cross-compilation uses toolchain files in
`cmake/`; the asset tools are always built for the host, never the target.

The game's sources cannot be compiled directly — upstream's `tools/preproc` must run over every
`.c` and `.s` first, to expand the `_("…")` string literals into the game's character encoding.
CMake reproduces that as a per-file custom command: `preproc` → C preprocessor → compiler.

Binary assets (`.png` → `.4bpp`/`.gbapal`/`.lz`, MIDI → song `.s`, maps → generated C) are
produced by roughly 40 KiB of rules spread across upstream's five `*_rules.mk` files.
**We do not reimplement those rules.** The build delegates asset generation to upstream's own
Makefile and consumes the generated files. Reimplementing that pipeline in CMake would be a large
transcription with a large bug surface, for no benefit while upstream's version already works and
stays correct as pret changes it.

The ROM build stays working and stays in CI. It is the reference: when the port renders something
wrong, the question is always "what does the ROM do here", and a byte-comparable ROM keeps that
question answerable. Note that `MODERN=1` (modern GCC) does not reproduce the original ROM's
checksum; byte-matching requires `agbcc`, which is not currently installed here.

## 9. Pointer width

The one genuinely hard constraint. `data/*.s` contains **789 `.4byte` directives**, many of which
are symbol references — pointers embedded directly in data blobs. The script VM reads them back
with `ScriptReadWord` (`src/script.c:188`) and casts the result to a pointer. On a 64-bit build
those pointers truncate, and the game's script engine breaks at the foundations.

The strategy is **32-bit first, 64-bit ready**:

- Phase A builds 32-bit — `-m32` on x86, `armeabi-v7a` on Android, `wasm32` on the web. Embedded
  pointers are correct with no changes, and the host assembler consumes `data/*.s` unmodified.
- From the first commit, every read of a pointer embedded in game data goes through an accessor
  rather than a raw cast. While 32-bit, the accessor is the identity function and costs nothing.
- The 64-bit migration then changes the *data pipeline*, not the game code: emit 32-bit offsets
  instead of pointers, and make the accessor add the arena base. macOS and iOS come online at that
  point, since Apple platforms are 64-bit only.

The alternative — going 64-bit-clean immediately — means writing an `.s`-to-C converter and
auditing every struct layout and save serialisation before anything can render. That is weeks of
pipeline work before the first pixel. Recorded in [ADR 0003](adr/0003-pointer-width.md).

## 10. Enhancement hooks

These are designed for now and built later. Designing them in costs almost nothing today;
retrofitting any of them means rewriting the PPU.

**Widescreen / higher internal resolution.** The renderer is resolution-parametric (§5.3). Game
logic still believes the screen is 240×160 and is not touched — the extra area is background and
object overdraw, revealing more of the map. Anything the game positions in screen space (UI,
textboxes, battle layouts) must be identified and anchored before this is switched on.

**Smooth scrolling / high refresh.** Game logic stays locked to 59.7275 Hz — decoupling it would
break every timing assumption in 320k lines of code. Instead the PPU becomes re-runnable between
logic ticks with interpolated scroll offsets, so scrolling and sprite motion are resampled at the
display's rate while logic ticks unchanged. This requires the PPU to keep the previous frame's
scroll state, which is why it is a design constraint now.

**Modding and asset hot-reload.** Assets currently reach the binary through `.incbin`, which bakes
them in. The hook is that every such blob is reached through a resource-id → pointer table rather
than by direct symbol reference, letting the VFS substitute a file from a `data/` override
directory at runtime. The table is introduced early; the runtime substitution comes later.

Save states were considered and are **not** a goal. They would require all mutable state to live
in one snapshottable arena, which constrains the memory map; that constraint is not being adopted.

## 11. Testing

320k lines of untouched game code cannot be reviewed, so correctness has to come from
differential testing against the real thing.

| Layer | How it is tested |
| --- | --- |
| BIOS math | unit tests against known-good vectors from real hardware |
| Decompressors | round-trip real game assets |
| PPU | golden-image comparison against mGBA frame dumps at fixed points |
| Full game | headless run, scripted inputs, hash the state each frame |
| Build | ROM build and native build both in CI; drift check on shadowed files |

The headless determinism test is the load-bearing one: it turns "did we break something in the
hardware layer" into a single hash comparison, which is the only way to refactor the AGB layer
with confidence.

## 12. Conventions

- C11. No compiler extensions outside `platform/host`.
- Port code is `snake_case`, prefixed `agb_` or `host_`. The game's `PascalCase` namespace is left
  alone, so a symbol's origin is always obvious.
- Comments explain *why*, never *what*. Rationale belongs in this file; history belongs in git.
- OS conditionals only under `platform/host/` and `ports/`.
