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

**Everything above it is the point.** The host is not a Game Boy Advance, and the 240×160 screen, the
13,379 Hz mixer and the 16.78 MHz ARM7 are what the original was affordable within rather than
anything the game requires. Most of it stays as it is; a lot of it will not. Deliberate divergence
from the cartridge is the purpose, not a debt against it, and only *accidental* divergence is a bug —
[ADR 0015](adr/0015-enhancement-over-preservation.md).

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
| `OPTIONS_SOUND_MONO` | a new save defaults to stereo, not mono |

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

The sound default is the first row that changes the *game* rather than the machine under it, and it
will not be the last — see [ADR 0015](adr/0015-enhancement-over-preservation.md). Stereo costs a
native host nothing, the port reproduces the reference's image to within 0.2 dB, and the option screen
still offers mono. A macro rather than an override because `OPTIONS_SOUND_MONO` has exactly one use in
the whole game — `SetDefaultOptions` in `new_game.c`. It reaches only new saves; the field lives in
the save block, so an existing one keeps whatever it stored.

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

Overrides carry a **maintenance** cost, not a fidelity one: a forked file stops receiving upstream
fixes. Prefer a shadow macro; reach for an override only when there is no macro seam. That an override
makes the port differ from the cartridge is not itself an objection — deliberate divergence is what
this project is for ([ADR 0015](adr/0015-enhancement-over-preservation.md)); it just has to be
recorded, which is what the table above is.

### 4.3 Files not built

`crt0.s`, `rom_header.s`, `libagbsyscall.s`, `libgcnmultiboot.s` and `m4a_1.s` describe cartridge
boot, the BIOS ABI and the ARM sound mixer. None apply to a native binary. `libagbsyscall` and
`m4a_1` are reimplemented in C under `platform/agb/src/`.

**271 of the 283 game sources compile natively with no source change at all.** The exclusions fall
into two groups, and every one describes hardware rather than game logic.

**Cannot be compiled** — ARM inline assembly:

| Excluded | Assembly it carries | Owned by |
| --- | --- | --- |
| `script.c` | `svc 2` (HALT) | **built** — the halt becomes `VBlankIntrWait`, which is what it meant; the pointer accessor is a 64-bit concern ([§12](#12-pointer-width)) |

**Four classes of edit are made in the preprocessed copy** rather than by overriding a whole
file for a line. Each has its own script, each match is exact, and each fails the build when its
target is absent, so a submodule bump is reported rather than silently changing what compiles:

| Script | Class | Files |
| --- | --- | --- |
| `strip_hardware_waits.py` | reaches hardware no host has | `m4a.c`, `main.c`, `script.c` |
| `patch_layout_assumptions.py` | assumes upstream's linker script | `load_save.c` |
| `patch_struct_layout.py` | is laid out differently by the cartridge's compiler | every game source |
| `patch_null_tolerance.py` | reads and writes only a machine without an MMU tolerates | `naming_screen.c`, `load_save.c`, `overworld.c`, `battle_transition.c`, `sprite.c`, `trainer_card.c`, `pokemon_summary_screen.c`, `pokemon_storage_system_tasks.c`, `region_map.c`, `battle_controllers.c`, `trade_scene.c` |

The third is worth understanding, because more of it will turn up. **The GBA has no MMU**: every
address in its map is readable, address zero included — that region is BIOS ROM — so a read through a
null pointer returns garbage instead of killing the program, and upstream code is entitled to that.
`naming_screen.c` frees its own state inside `RunTasks()` and then animates its sprites in the same
frame, so the cursor's callback reads through the pointer just cleared. On hardware that is one
frame of a misplaced cursor and then the callback is gone. Here it was a segfault, and it is what
stopped a player naming their character.

Mapping a readable page at address zero would answer the whole class at once, and is not available:
`vm.mmap_min_addr` forbids it without privileges we should not want. So each site is repaired where
it is.

**The class covers writes as well**, which is worse, because a write cannot be redirected somewhere
harmless — it has to go somewhere real. `load_save.c` is the second instance: the save blocks'
pointers start null and are not set until the title screen, but `LoadGameSave` runs at boot, so the
sector copy writes through null plus the sector's offset. On hardware those writes land on the BIOS
region and are ignored, and the real load happens later and works. Here the game **could not boot at
all with a save present**. Pointing the three pointers at their own objects makes that early copy land
where it belongs; offset zero is one of the offsets `SetSaveBlocksPointers` itself picks, so it is a
state the game already handles.

Ten instances in six phases, every one found by playing rather than by reading, and each only
reachable once the phase before it worked: a save to exist, a save to load, a battle to reach, a
Pokémon worth looking at, a trainer card worth opening, a box to move a Pokémon into.

**The tenth is a write, and not a freed-pointer one.** `SetMovingMonPriority` sets the held Pokémon's
sprite priority, and `DoCursorNewPosUpdate` calls it whenever the cursor moves onto the buttons, the
party or a box. Only `UpdateCursorPos` checks `sIsMonBeingMoved` first. With nothing held the sprite
pointer is null and the write lands on address 5 — the priority byte's offset inside `struct Sprite` —
which on hardware is BIOS ROM and dropped. It is guarded in the callee rather than at the three call
sites, because upstream already tests that same pointer for null before destroying it: a null there is
an expected state, not a lost sprite, and one guard covers every caller.

**The fourth class has the widest reach of the four, and applies to the cart rather than to the code.**
The cartridge was compiled by `agbcc` ([`Makefile:82`](../vendor/pokefirered/Makefile)), which pads
some structures our compilers do not — on ARM as well as x86, so it is agbcc's doing and not the
target's. Such a type occupies *fewer* bytes here than in the ROM, and every table of it read out of
the cart ([§5.3c](#53c-not-compiling-the-data-in)) is misparsed from its second element onward. The
first element is right, which is what makes it so quiet: the data is byte-perfect, the relocations are
correct, and only the reader's stride is wrong.

**Which types are affected cannot be told from their shape.** `MonCoords` is two bytes here and four
in the cart; `LevelUpMove` is also two bytes and is two in both. Each type is verified against the
ROM's own symbol size over the entry count in the source — `gBattleMoves` is 4260 bytes over 355
moves, so twelve each against our nine — and only then widened.

`union AffineAnimCmd` is six bytes here and eight in the cart. Reading an affine animation with a
six-byte stride never reaches the END marker, so the animation never completes — a Pokémon's send-out
loops forever, the sprite keeps whatever pixels its buffer already held, and the matrix is never
written, which makes `UpdateSpriteMatrixAnchorPos` divide by a zero scale. One mismatched type
produced a hung battle, blank battlers, corrupt intro Pokémon and a `SIGFPE`, all at once.
`struct MonCoords` is two bytes here and four there, which moves every battle sprite.

**Thirteen types are widened today.** Eight are reachable from the list on `main`: `AffineAnimCmd`,
`MonCoords`, `BattleMove`, `SpeciesInfo`, `Evolution`, `TrainerMoney` and the two trainer party
structs, `TrainerMonNoItemDefaultMoves` and `TrainerMonNoItemCustomMoves`. Five more —
`BattleWindowText`, `WinCoords`, `ListMenuWindowRect`, `CreditsOverworldCmd` and
`BattleTowerPokemonTemplate` — are reachable only once the statics are extracted too. Each is
verified against the ROM as above.

The party structs are the reason the audit reads more than the symbol list. They live in `data.c`,
which is cut *whole* rather than by symbol, so nothing named them anywhere the audit was looking; a
wrong stride handed a Viridian Forest bug catcher a level 0 Charizard, found by playing. The audit
now adds every symbol defined by a `FRLG_GAME_DATA_ONLY` file, which took its coverage from 6,453
symbols to 14,339.

`tools/audit_layout.py` reads every type's size from the binary's own debug information and reports
which *extracted* symbols use one whose size is not a multiple of four. Reading sizes from a single
translation unit is not enough: an earlier version did that and reported no mismatches while
`BattleWindowText` was actively crashing the build, because the type is not in that unit's headers. The rest are listed by `tools/audit_layout.py`
and are only latent while their data stays compiled in — widening the extraction list without
consulting that audit is how this returns. Types that live only in RAM are deliberately *not*
widened: their layout is this build's own business, and changing it would change the save format.

None of this was caught by trace replay, by the entropy probe, or by comparing extracted bytes against
the ROM, because all three were measuring the data. It was caught by playing the game and by bisecting
the extraction list against a recorded play-through ([§5.3d](#53d-recorded-play-throughs)).

**A screen's callback outliving the screen's data is five of the eight** — the naming screen,
the battle transition, the trainer card, the summary screen, the storage system — the fourth and fifth
turning up in the two play-throughs after the pattern was written down here. The storage system is the
*main* callback rather than the V-blank one, which widens it: nothing clears either kind of callback
when the state it reads is freed, and the screen that replaces it is always a frame or two late. Every
screen that allocates state and installs a callback is a candidate, and there are dozens. Two ways to repair one, and which
applies is decided by whether anything tests the pointer for null:

- **Nothing does** — free without clearing, and the read lands on freed heap instead of address zero.
  Mapped, garbage, read once or twice, and it covers every dereference in that screen at once.
- **Something does** — guard the dereferences themselves, leaving the rest of the callback running,
  because the work before them is what the *next* screen depends on.

"Tests it" includes `if (ptr)`, not only `if (ptr != NULL)`. A sweep that looked for the second form
alone reported the trade scene as safe to leave dangling when `DoTradeAnim_Cable` guards its teardown
with the first — and that screen has three free sites, so the dangling repair would have freed twice.
The allocation-failure check immediately after an `Alloc` does not count; every screen has one.

There is a **cost to the first choice that was missed when it was first made**. `Free(NULL)` is a no-op
in upstream's allocator — `FreeInternal` opens with `if (p)` — so clearing the pointer is also what
makes a second teardown harmless. Leaving it in place trades a null dereference for a double free,
which merges a block into its neighbours twice and corrupts the free list silently. Where a free site
can run twice, or is itself written `if (ptr) free`, the pointer must be cleared and the readers
guarded instead. The town map is the case that made this visible: `FreeMapCursor` is reached from two
places.

Audited for the three already repaired this way: the naming screen frees from a terminal state, the
summary screen destroys its task before freeing, and the storage system frees from a one-shot teardown.
The battle transition is the one with residual risk — `IsBattleTransitionDone` is polled every frame
and frees on the frame it answers yes. A pointer freed while
something still reads it — the naming screen's cursor, and the battle transition's V-blank callback,
which survives `IsBattleTransitionDone` freeing the data it reads. And a pointer legitimately null —
the save blocks before the title screen sets them, and a map with no object events, whose script copy
ignores the count and reads sixty-four entries regardless.

The freed-pointer shape is repaired the same way each time: free without clearing, so the read lands on
freed heap rather than on address zero. Mapped, garbage, and read at most a frame or two — which is
what the hardware does with it too.

The fifth sits one level up from the others. A screen that caches a sprite destroys the old one before
creating the new, and on a first visit there is no old one — so the guard belongs in
`DestroySpriteAndFreeResources`, which 22 places call, rather than in the screen that happened to find
it. Every step of that function on a null sprite is a read or write through address zero plus a small
offset: BIOS ROM, which reads as whatever was last prefetched and ignores writes. Refusing the call is
the same nothing, done deliberately. A file may need more than one of these scripts, so they accumulate
rather than choosing between themselves — `load_save.c` takes both a layout repair and a null one.
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

**`task.c` is compiled at revision A** (`FRLG_GAME_REVA`), which is the same mechanism pointed at a
different flag. `DestroyTask` indexes `gTasks` with whatever it is given, and `FindTaskIdByFunc`
returns `TASK_NONE` (255) when the task is already gone — 10 200 bytes past a 640-byte array. On a
GBA that read lands in EWRAM and finds a zero, so nothing happens. Here it found an 8, took the
non-zero branch, and unlinked a task that does not exist: `gTasks[0].prev` was written with garbage,
and since `RunTasks` walks a linked list rather than an index range, the head became unreachable and
the running screen simply stopped being called. The title screen froze on a black frame 4 288 and
never returned to the intro.

Upstream fixed this in revision A, behind `#if REVISION >= 0xA`. `REVISION` appears exactly once in
`task.c`, so building that one file at revision A applies their own fix and changes nothing else —
preferable to a patch of ours ([ADR 0015](adr/0015-enhancement-over-preservation.md) permits the
deviation either way; taking upstream's is simply less to maintain). This is the same class as
[§4.2's](#42-overrides) null-tolerance work: the GBA has no MMU, so an out-of-range index is a read
somebody else's hardware forgave.

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

The generators read that build's **layout** from its ELF rather than assuming one: executable
sections are code, allocated non-executable sections hold data, and `tools/elfsections.py` is where
both rules live so the two generators cannot disagree about them. Hardcoding either — where `.text`
ends, or which `.rel*` sections to read — encodes the modern layout, and the matching build differs
in both: `lib_text` sits above `script_data`, and the music has its own `song_data` with its own
relocation section ([spike 0001, finding 4](spikes/0001-relocation-table.md)).

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

Those definitions are **absolute** symbols, which a position-independent executable does not rebase
when the loader moves the image — so the port links at a fixed load address
([ADR 0012](adr/0012-fixed-load-address.md)). Android and the web cannot do that, and need the
pointer-indirection alternative the ADR sets out; desktop keeps the direct load.

### 5.3 Relocating what the image points at

Loading the image is not enough, because the data in it holds pointers, and they are the addresses
the *original* build gave its own code and RAM. `0x03004518` is not a place here. The sound engine
is the first subsystem that follows one, which is what pulled this forward from phase 7.

`tools/gen_relocations.py` reads the relocations the ROM's own link left behind and emits a table
naming every pointer that has to be rewritten. It is C rather than data, because code and RAM
targets are named symbols: writing them as `&symbol` lets our linker resolve them, so nothing looks
symbols up at run time. `agb_cart_load` reads the image and applies the table once.

Of 61,142 records, **three classes are rewritten and three are deliberately left alone**
([spike 0005](spikes/0005-relocation-classes.md)):

| Class | Count | What happens |
| --- | --- | --- |
| Data | 48,146 | shifted: `agb_cart + (addr - 0x08000000)` |
| Global code or RAM | 3,081 | our symbol of that name, plus the addend |
| Interior | 6,851 | **left alone** — jump tables inside ROM functions, which have no native counterpart and which nothing reads |
| Local targets | 3,059 | **left alone** — statics have no linkable name, and every one sits in ROM data our build re-creates rather than reads |
| 16-bit | 5 | **left alone** — script constants, not pointers |

The 1,107 distinct symbols the table names are exactly the `data/*.s` symbols the binder maps into
the cart region, which is the arrangement being consistent with itself.

**The table, the bindings and the image must come from one build.** Ours come from
`pokefirered_modern`, which already carries the relocation sections. §5.1's requirement that the
manifest be byte-matching is about *shipping* — a player supplies a retail cartridge — and phase 7
regenerates the whole set together.

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

### 5.3b Verifying, and keeping what was imported

The manifest describes one exact image, so the port can say whether the file it was handed is that
image. The expected hash is computed by CMake from the very ROM the generators read
(`file(SHA1 ...)`), which is the only arrangement where the two cannot drift apart. It is checked
**before** relocation, because relocation rewrites the image and the question is about the file the
player supplied.

A rejection says what it was given. Six hashes are compiled in — names and digests, no game data —
so an unusable file gets *"that file is Pokemon LeafGreen (rev 0); this build needs Pokemon FireRed
(rev 0)"* rather than a refusal. Anything else reports the hash it saw, and a file of the wrong length
is refused before it is hashed at all.

**The relocated image is then kept, and the ROM is not needed again.** It is not a copy of the ROM:
every pointer in it has been rewritten to this build's own addresses, so it will not run in an
emulator and is not a cartridge dump. It is a derived artifact private to this install, which is what
ADR 0006 means by releasing the ROM.

The cache is keyed by the manifest's hash — a build for another title or revision is another game and
must not read this one's image — and its header carries a **layout fingerprint**: an FNV hash over
every native address the relocation table resolved. Any relink can move those, and an image relocated
against addresses that have moved is a set of pointers into nowhere, so the fingerprint is what makes
a stale cache recognisable rather than merely wrong. A version number would be a promise somebody has
to remember to keep; the fingerprint is the actual answer to "did I write this".

Import costs 26 ms, so the cache buys almost no time. What it buys is not needing the player's ROM
file to still exist, still be where it was, and still be readable, months later.

**Settings are two different things.** What the launcher writes to
`$XDG_CONFIG_HOME/frlg-native/settings.ini` are the port's own — recording, window size — which the
game has no concept of and which apply before any save is loaded. The seven the *game* keeps are
bitfields inside its save, so they belong to a save profile rather than to the install, and they are
reached through the game rather than by the launcher parsing that format.

Every setting has an `FRLG_` variable that came first, and **the environment still wins**. A setting
that could override one would make a recorded run depend on what somebody had clicked, which is the
thing the lockstep clock exists to prevent — the golden and audio harnesses set `FRLG_NO_RECORD`, and
they must keep meaning it.

`--options` and `--set-option` reach the second kind. Writing one touches only the newest copy of
`SaveBlock2` — the game reads the slot with the highest counter, and the older slot is the backup that
exists so a bad write is survivable — recomputes that sector's checksum, and renames the result over
the original. A sector whose checksum does not verify is one the game discards, which would undo the
change silently and look like the save going bad on its own.

**A launcher asks the game about itself.** One binary is one title, and it is the only thing that
knows which ROM its manifest describes or whether that ROM has been imported — so the launcher does
not carry a second copy of any of it. `--describe` prints `title`, `sha1`, `cache` and `imported`;
`--import <rom>` imports without playing and prints `ok=yes` or `ok=no` with an `error=` a person can
read. Both answer before a window, a device or a session is opened, because they are questions about
the install rather than a run of the game. `key=value` lines, which a shell and a `GSubprocess` read
equally well and which need no parser on either side.

### 5.3c Not compiling the data in

Binding only ever reached symbols the linker called **unresolved**, and those are the ones from
`data/*.s` that no host assembler can build. Everything the decompilation defines in C resolved
locally and was compiled straight into the binary — which is ADR 0006's *developer* data path, and
nothing announced that the project was still on it.

`FRLG_GAME_DATA_FROM_ROM=ON` is the shipping path. `FRLG_GAME_DATA_ONLY` names translation units that
contain **only data**; they are not compiled, their symbols are therefore unresolved, and the existing
machinery binds each into the cart region. No new mechanism — the old one simply never saw them.

| | ROM chunks in the binary | size |
| --- | --- | --- |
| data compiled in | 49 of 200 | 19.4 MB |
| `graphics.c` alone excluded | 22 of 200 | 15.5 MB |
| twelve data-only files excluded | **17 of 200** | **14.6 MB** |

The game is unchanged by it, which is the point: same frames, and byte-identical audio over a battle
trace. What remains is data defined in translation units that *also* hold code, which cannot be
dropped this way — [issue #11](https://github.com/SoftARV/frlg-native/issues/11).

Most of the data shares a file with code, which cannot be dropped that way. For those,
`FRLG_GAME_DATA_SYMBOLS` names the symbol and `tools/patch_data_definitions.py` rewrites the
definition in the preprocessed copy into a **declaration** — `const struct Evolution
gEvolutionTable[412][5] = {…};` becomes `extern const struct Evolution gEvolutionTable[412][5];`.
A declaration rather than a deletion for two reasons: some of these have no `extern` in any header,
and the bounds have to survive or code taking `ARRAY_COUNT` of one stops compiling. The symbol is then
unresolved and binds like the rest.

That is what moved the tables `required-to-function.md` names — species, moves, learnsets, evolutions,
encounters and items — out of the binary. They are small compiled (about 50 KB for all six) and large
in source, which is why the file shrinks far more than the binary.

**Only files with no code may be listed**, and it is checked rather than trusted: a file that grows a
function would have it silently dropped and its symbol would bind to whatever the ROM holds at that
address — ARM machine code this port cannot execute. `tools/check_data_only.py` reads the ROM build's
own objects, since that is the only place these files are compiled, and fails configuration.

### 5.3d Recorded play-throughs

`tests/playthrough/` holds recordings of somebody actually playing, and they exist because the
verification that came before them said the extraction was clean while battles were unplayable.

A trace and the save it started from are one artefact — a trace replayed against any other save is
meaningless. `newgame.trace` needs no save at all: it starts a new game, which makes it the better
of the two, since it depends on nothing but the ROM.

| Recording | Reaches | Needs |
| --- | --- | --- |
| `newgame.trace` | the intro, then the rival battle | nothing — starts a new game |
| `input.trace` + `start.sav` | a wild battle from a mid-game save | its own save |
| `storage.trace` + `storage-start.sav` | the storage system, cursor moved with nothing held | its own save |
| `overworld.trace` + `overworld-start.sav` | eight minutes of ordinary play, no faults | its own save |
| `loss.trace` | a new game through to losing the first rival battle | nothing — starts a new game |

What makes them worth keeping is not coverage but *what they are compared against*. Replaying one
against a build with the data compiled in gives a known-good frame; replaying it against a build
reading the same data from the cart must give that frame back, pixel for pixel. That comparison is
mechanical, so it bisects: the extraction list is halved until one entry is left. Both of the layout
mismatches in [§4.2](#42-overrides) were found that way, in a handful of builds each, after inspection
had cleared the same code five times.

**Frame 17500 of `newgame.trace` is the canonical check** — the rival's Pokémon on the field, which is
the first moment a battle sprite is drawn at all. Nothing in the earlier trace set ever rendered one.

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
great majority of use.

A channel armed with a **start timing** transfers nothing when it is armed and runs when the display
reaches the moment it names: `agb_dma_trigger` is called by the frame driver at the start of V-blank
and by the PPU at the end of every visible scanline, before the matching interrupt handler, since a
channel outranks the CPU. Channels run lowest number first, as their priority runs. A repeating
channel keeps its source where it left off — that is what feeds a scanline effect its next entry —
while a destination in reload mode goes back to where it started, which is what feeds one register.
One without the repeat bit clears its own enable bit, which is how the game tells that a transfer has
happened.

Per-scanline DMA is not a corner: the battle transitions are built on it, writing one 16-bit entry per
line into `WIN0H` from a 160-entry buffer to sweep a window across the screen, and so is everything
`scanline_effect.c` does. Without it those transitions are a black screen — which is how the gap was
found, by playing into a battle.

FIFO DMA (start timing 3) feeds the sound mixer on hardware and is not implemented: the port reads the
mixer's PCM buffer directly instead ([§6.7](#67-audio)).

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

That clock is for **playing**. A frame boundary lands wherever wall-clock time puts it, so a frame
whose work overruns the tick misses a V-blank and the run falls one frame behind a less loaded one —
which is fine at a controller and fatal to a comparison. `FRLG_LOCKSTEP` therefore advances frames
from the idle spin itself, which is the one point the game is provably doing nothing: the pipeline
rewrites that spin's empty body to call `agb_frame_idle()`, no timer is armed, and a run depends on
nothing but the game's own state. Six concurrent captures then produce byte-identical frames where
before they produced three different ones, and the golden tier runs in a tenth of the time.
[ADR 0013](adr/0013-lockstep-capture-clock.md); every capturing harness sets it, and nothing else
should.

Not every wait reaches that spin — `DoMapLoadLoop` runs its state machine to completion with a bare
one, and a step of it waits on the DMA3 manager, which only drains in the V-blank handler. The timer
therefore stays armed in lockstep as a **watchdog**, re-armed three frame periods out after each
frame, advancing the frame the game is waiting for when it fires. Firings are counted and reported,
because one during real work would mean the run depended on wall-clock time after all
([ADR 0014](adr/0014-lockstep-stall-watchdog.md)). `FRLG_LOCKSTEP=pace` additionally waits out the
frame period, which is what makes the clock playable — and so what a recorded trace is made on.

### 6.6 BIOS

`libagbsyscall.s` becomes C. The arithmetic entry points (`Div`, `Sqrt`, `ArcTan2`, `BgAffineSet`,
`ObjAffineSet`) must reproduce the BIOS's exact results including its rounding quirks, because game
logic depends on the values — unit-tested against known vectors rather than trusted.
`CpuSet`/`CpuFastSet` honour the fixed-source and 16/32-bit control bits. `LZ77UnComp`, `RLUnComp`
and `HuffUnComp` are exercised against real game assets.

`RegisterRamReset` clears the display registers, and the two affine matrices sit inside that range —
but they are the identity at rest, not zero, and the game depends on it. `AgbMain` resets every
register on the way in and the trainer pictures are still drawn on an affine BG2 a thousand frames
later without anything writing a matrix; the reference emulator's renderer holds the identity across
the same call. So the clear puts them back, through `agb_io_affine_identity()` in `memmap.c`, which
is also what the power-on state uses. See docs/spikes/0008-missing-trainer-pics.md.

### 6.7 Audio

`m4a.c`, the sequencer, **is built**, and needs no override. Two statements in it are hardware that
no host can run, and both are removed from the preprocessed copy by `tools/strip_hardware_waits.py`,
which fails the build if either is absent so a submodule bump is reported rather than silently
changing what compiles:

- `asm("swi 0x2A")` inside `MusicPlayerJumpTableCopy`, asking the BIOS to fill the dispatch table.
  **Nothing in this game calls that function** — the table is filled by `MPlayJumpTableCopy`, which
  we supply. A blanket erasure of `asm` would be wrong: `global.h` rewrites it to `__asm__`, and the
  preprocessed file carries a second one, glibc's asm label on `strerror_r`.
- `SampleFreqSet`'s spin until the display reaches scanline 159, which phase-aligns timer 0. A whole
  frame is rendered inside one signal handler here, so the game thread is suspended for the entire
  sweep and reads 0 whenever it resumes — the wait can never end. Nothing is lost: the host reads the
  mixer's PCM buffer directly rather than through timer 0.

The two buffers the mixer fills are **not a finished stereo pair**. Each feeds one of the two
direct-sound FIFOs, and `SOUNDCNT_H` says at what volume each reaches each side —
`agb_m4a_apply_output_mix` applies that before the PSG channels are added, since those carry their own
ratio. It matters because the game changes the register: `m4aSoundInit` leaves both FIFOs at full
volume and hard panned, and applying the sound option calls `SetPokemonCryStereo`, whose mono branch
puts both on both sides at **half** volume. Ignoring it left the music at twice its intended level
against the PSG channels — most of the interface's sounds — from the first menu onwards, while the
intro, which plays before the option is applied, sounded right. Reported by ear, then confirmed by
reading `SOUNDCNT_H` out of the reference at the same frame: `3302` in both.

The link also needs two of upstream's linker-script absolutes, `gNumMusicPlayers = 4` and
`gMaxLines = 0`. Keeping 1781 lines of sequencer as upstream C means it keeps receiving decomp fixes.

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

**Reversed waves** are walked from the end of the wave towards its start. The resampling is the
forward pitched path's — interpolate between neighbouring samples, carry the fraction across frames —
but the pairs are read in the other order, and the fixed-frequency variant is handled here too rather
than in a separate routine, as a step of exactly one sample. Two things differ beyond direction: the
play position is turned round **once**, on the channel's first frame, to the same distance from the
end that it was from the start; and a reversed wave **does not loop** — running out of samples ends
the note.

**Compressed waves** hold 64 samples in 33 bytes: one whole sample, then 4-bit codes selecting from
sixteen delta steps. A block is decoded in one go and cached in the channel — the resampler asks for
neighbouring samples, so nearly every request hits — and the walk is by index rather than by pointer,
because a compressed sample has no address. The channel's pointer field carries that index, converted
once. Forward and reversed are both handled, and they are **asymmetric at the start**: forward reads
the sample it is already on, reversed steps back first, exactly as the uncompressed pair are.

The running value is kept wide and truncated only as each sample is stored, so a run of large steps
wraps per sample instead of saturating.

FireRed uses one compressed instrument: **Nidorino's cry**, in the intro battle. Finding it corrected
an earlier claim here that nothing in the game was compressed — that came from scanning only the tone
tables the song table reaches directly, never the sub-tables a rhythm or key-split instrument points
at, and it took a static reading for a complete one. The runtime settled it in one line.

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

**Getting it heard.** The mixer produces the GBA's native 8-bit signed stereo at its own rate —
13,379 Hz as the game configures it — and does not call the host itself. The port installs an
`agb_audio_sink` and the mixer hands each finished frame to it, the same arrangement as the PPU,
which composes into a buffer the port presents. `agb/audio.h` is deliberately separate from
`agb/m4a.h` so a port needs none of the sequencer's types to listen.

SDL3 opens a stream in the mixer's own format and resamples to whatever the device runs at, so the
port converts nothing. The stream is reopened if the game changes the rate, which `m4aSoundMode`
can do at any time. If the queue runs long — the device stopped consuming — it is dropped rather
than allowed to grow, because falling behind is better than blocking the game thread. The `null`
backend refuses to open, and submitting to a closed device is ignored, so a platform with no audio
needs no special case above it.

Mixing is driven from the frame loop, not the audio callback, so audio stays in lockstep with game
state.

**Every run records itself.** `host_session_open` makes a directory under `$XDG_DATA_HOME/frlg-native/
sessions/` named for the moment it started, and the run fills it: `input.trace` as the keys are read,
`start.sav` copied *before* the game can touch it, and `session.log` holding both output streams. Five
runs are kept and older ones pruned.

It is on by default and `FRLG_NO_RECORD=1` turns it off, which is what the golden and audio harnesses
set — a capture is not a play session. The reason it is a default rather than a flag is that the three
files have to exist *before* anything goes wrong, and a tester cannot be asked to arrange that in
advance ([ADR 0016](adr/0016-every-session-records-itself.md)). `FRLG_INPUT_RECORD` still names its
own path and wins; a replay never records, since it would only copy the file it is reading.

The log takes both streams, not just `stderr`: the port says what it loaded on `stdout` and complains
on `stderr`, and half a log reads as though half the run is missing. Each keeps its own pipe and pump
thread so the terminal still behaves, and `stderr` is made unbuffered so a crash cannot strand the
last few lines — the ones that matter — in a buffer nothing will flush.

**A fault becomes a report rather than an exit.** `crash.c` catches `SIGSEGV`, `SIGBUS`, `SIGILL`,
`SIGFPE` and `SIGABRT` on the game thread, and the whole design is one rule: a signal handler may call
almost nothing, so it does almost nothing. Inside it, only what cannot be deferred — the fault
address, the frame number, and a backtrace of the stack about to be unwound — written with raw `write`
to a descriptor opened at startup, formatted by hand because `snprintf` is not async-signal-safe.

Then it leaves through `agb_frame_abort`, which is `siglongjmp` to the same exit point the frame limit
and the stop request use, as code 4. `agb_frame_run` returns normally, on a stack nothing damaged, and
everything unsafe — the zip, the message, the exit code — happens there. Nothing resumes: the jump
ends the run rather than surviving it. Three details make it work on real faults rather than tidy ones:
a `sigaltstack`, so a stack overflow still has room to report; a warm-up `backtrace` call at startup,
because glibc loads its unwinder on first use; and a re-entry flag, so a fault *while reporting* exits
instead of looping.

The report says what happened in a sentence, not a hex number. A fault address below `0x4000` is the
GBA's BIOS region, which is the no-MMU class this port keeps finding — nine for nine so far — so the
report says the game read through a pointer that was never set, and that hardware would have tolerated
it. `FRLG_CRASH_TEST=segv|bus|abort` faults on purpose at `FRLG_CRASH_FRAME`, because a recovery path
nobody can trigger is a recovery path nobody has tested.

The screen itself is a native message box rather than something drawn in the window: the renderer's
state after a fault is not worth relying on, and this needs no font and no new dependency to be legible
everywhere SDL runs. Three buttons — show the report, report it, close — and each of the first two
hands off to a program that outlives this one, so the dialog answers once and goes. There is no copy
button and no send button: copying a path is a promise the owning process has to stay alive to serve,
which made the dialog linger to be dismissed twice, and sending would make this a service rather than
a file ([ADR 0016](adr/0016-every-session-records-itself.md)).

`host_session_bundle` packs the session into `report.zip` — stored entries, a hand-written central
directory, no compression and no dependency, since Android and web would have to carry it too. One
file, because a folder arrives as three attachments and the missing one is always the save.

**The oracle takes input.** `mgba-capture` replays the port's own trace format, shifted by the +38
frame boot offset, so a frame that can only be reached by playing can still be compared against the
reference. That is what made [spike 0008](spikes/0008-missing-trainer-pics.md) investigable at all.

**Sound has an oracle too.** `tools/mgba_audio.c` captures PCM from mGBA running the same ROM, and
`FRLG_PCM=<path>` makes the port dump the same format — measured before the device is considered, so
it needs no sound hardware. Between them a tune can be compared rather than described.
Both tools take `FRLG_INPUT` and `FRLG_SAV`, so the reference can be driven to the same place in the
game as the port rather than only through the intro — the port's flash image is the 128K layout mGBA
expects, and it must be handed over as a writable copy or the reference loses its save memory a few
seconds in. [Spike 0007](spikes/0007-audio-against-mgba.md) is the first such comparison: the per-second level
tracks mGBA's across the whole intro, which says the right notes are playing at the right times, and
the one clear spectral difference turned out to come from the direct-sound path rather than from the
PSG that was suspected.

**The PSG channels** are the GBA's other sound system, and `platform/agb/src/psg.c` is it: two
squares, a programmable wave and noise. The m4a engine does not mix these — it drives them by writing
registers, `CgbSound` doing so every frame — so something has to turn those writes into samples.
Without it, 88 of the 812 instruments the song table reaches are silent, about 11%, which is audible
as parts of a tune missing rather than as no sound at all.

The channels are the Game Boy's carried forward, so the timings are that hardware's: a frame
sequencer at 512 Hz driving length counters at 256 Hz, channel one's sweep at 128 Hz and the
envelopes at 64 Hz. Registers are read as they stand at the start of each frame, the way the PPU
reads its own — the sequencer rewrites them that often. The trigger bit is write-only on hardware, so
it is cleared once acted on, which is what a read-back would see there.

A square's two sides are weighted by the time spent on the other, so no duty carries a standing
offset. A real channel's DAC puts out 0 to 15 and the hardware couples it through a capacitor;
swinging symmetrically about zero instead would leave one eighth duty sitting at three quarters of
the volume, and since the duty changes from note to note that offset moves — a thump under the tune,
spending the headroom the melody needs. It measured as exactly that
([spike 0007](spikes/0007-audio-against-mgba.md)).

How loud these are against the sampled channels is one constant, and it is load-bearing. A channel's
DAC spans sixteen steps, so at full volume it swings ±7.5 about its mean; the reference reaches its
mixer with that times sixteen at full settings, while a direct-sound sample arrives times four. In
the buffer the two share, where one FIFO at full volume reaches 127, that puts one channel at 30 and
all four together at 120 — and the two lower settings of `SOUNDCNT_H`'s mixing ratio halve it and
halve it again. Carrying half that left every square, sweep and noise burst **5.4 dB under the
music**, measured the same way in both engines, which is most of the game's sound effects buried
under the track they play against. Reported by ear as effects that needed the volume turned up,
then measured; `tests/test_psg.c` pins all three ratio settings.

Weighing one side against the other needs the other out of the way, on both sides of the comparison:
`FRLG_AUDIO_MUTE=direct` silences the two FIFOs and `=psg` the hardware channels, and `mgba-audio`
takes the same word for the reference. Muting through `SOUNDCNT_L`'s enable bits instead leaks — the
sequencer rewrites them as channels come and go, and a soloed square then changes when only the noise
code does.

Two deliberate departures. Sampling happens at the **software mixer's rate** rather than the
hardware's, because the result is added to that mixer's buffer; point-sampling a square is not what
the hardware does, but the host would resample one stage later regardless. And the final add
**clamps** where the software mixer's own accumulate wraps: that one reproduces the m4a engine's
register rotation, this one is the mix into the DAC, which saturates.

### 6.8 Save data

`platform/agb/src/flash.c` is 128 KiB of flash backed by a host file, byte-identical to the `.sav`
format emulators produce, so saves move in and out of mGBA and off real hardware without conversion.

**It replaces the chip, not the driver.** Upstream's `agb_flash*.c` talk to real flash through timed
command sequences — write `0xAA` here, `0x55` there, poll until the chip stops toggling — and
`ReadFlashId` runs code it has copied onto the stack, which no host target can do. They are not
built ([§4.3](#43-files-not-built)). What they drive is a chip, and that is what this supplies: the
save code above them is upstream's and cannot tell the difference.

The chip lives in the arena's SRAM region, which is exactly the 131,072 bytes of a 1 Mbit part, so a
sector is addressed the way the hardware addresses it: thirty-two sectors of 4 KiB. Two behaviours
are the chip's rather than a convenience, and the save code depends on both — an erased cell reads
`0xFF`, and programming can only *clear* bits, never set them, which is why the save code erases
before it writes.

The file is written after each sector is programmed rather than at exit, so a save survives the
program being killed. A file shorter than the chip fills what it can and leaves the rest erased,
which is what a save from a smaller part looks like. The path comes from `FRLG_SAV`, or the working
directory, until phase 7 gives it a proper home.

### 6.9 Serial and link play

Peer-to-peer link play — trades and battles — is a committed feature, so the serial layer is
designed for a real transport rather than permanently stubbed.

The game drives serial through registers and the RFU wireless adapter. Our serial layer presents
the same register behaviour and carries the payload over `host_net`. Link play is *lockstep*: both
peers must agree on every frame, which is why the deterministic fiber loop is a prerequisite rather
than a nicety, and why desync fuzzing is part of the test plan.

Until the transport lands, the layer reports "no peer connected" — a state the game already handles
gracefully.

The adapter's library is not built, and its entry points are stubs that name themselves and return
zero. One is not: `rfu_initializeAPI` hands out five pointers that the link manager reads through on
the next frame, so `platform/agb/src/rfu.c` carves the caller's buffer the way librfu carves it and
leaves it zeroed, with the link status reporting neither parent nor child. Reaching this needs no link
hardware and no menu — **walking into a Pokémon Center** starts the union-room listener, which switches
the link layer to wireless, and from then on the manager runs every frame.

## 7. The host abstraction

**Closing the window has to work**, and it needs both halves of the port. The game's main loop never
returns, so `agb_frame_stop` asks the frame driver to leave through the same non-local jump the frame
limit uses; without it the join behind the window sat on a thread that was still running the game,
which is the hang a desktop offers to force-quit out of. The wait on that join is bounded for the same
reason — a game thread wedged somewhere that never reaches a frame boundary must not keep the window
alive either.

The window also has to *have* a close button. On Wayland the compositor draws no decorations and SDL
needs libdecor to draw its own; this port is 32-bit ([ADR 0012](adr/0012-fixed-load-address.md)), so a
64-bit libdecor on the system does not help it and the window comes up borderless. When this build
cannot load libdecor and an X server is there, the host asks SDL for X11, whose window manager
decorates. Installing a 32-bit libdecor removes the need for that, and the log line says which was
used.

The two drivers also disagree about what a window size *means* — Wayland takes logical pixels and
scales them itself, X11 takes real ones — so the display's content scale is folded into the request.
On a panel asking for 200% that is the difference between a comfortable window and a postage stamp.
`FRLG_SCALE` overrides it.

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

**The offset is per row because no single one exists.** mGBA emulates an ARM7 at 16.78 MHz, and FRLG's
`WaitForVBlank` clears the interrupt flag before waiting — so a frame whose work overruns loses that
V-blank outright, and the scene takes an extra frame. Across the intro the reference loses 73 of them
and the port loses none, which is measurable directly: `gMain.intrCheck` sampled at a frame boundary
says whether the game had reached its wait. The port is not mispaced, it is faster than the machine,
and a trace is therefore portable within the port but not to the reference
([spike 0009](spikes/0009-trace-pacing-vs-mgba.md)). Matching hardware would need a cycle model, which
nothing on the roadmap requires — link play would be the first thing that did.

`mgba-capture` reads the reference's **own variables**, not only its pixels: `gMain`'s address comes
from `pokefirered_modern.map`, so `FRLG_DUMP_KEYS` can report what the reference game made of an input
and whether it was keeping up. Asking the reference what it thinks, rather than inferring it from a
screenshot, is what settled spikes 0008 and 0009.

CI gates expensive per-platform jobs behind change-detection jobs, and runs SDK-free self-tests
everywhere, so platform count stays affordable.

## 15. Conventions

- C11. No compiler extensions outside `platform/host`.
- Port code is `snake_case`, prefixed `agb_` or `host_`. The game's `PascalCase` namespace is left
  alone, so a symbol's origin is always obvious.
- Comments explain *why*, never *what*. Rationale belongs in this file; history belongs in git.
- OS conditionals only under `platform/host/` and `ports/`.
