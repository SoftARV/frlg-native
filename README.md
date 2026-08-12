# frlg-native

A multiplatform native port of the `pret/pokefirered` decompilation of Pokémon FireRed and
LeafGreen.

This is **not an emulator and not a static recompilation.** The decompilation is complete C, so the
game is compiled directly for the host CPU and linked against a small reimplementation of the
hardware it expects — the GBA's picture unit, DMA, interrupts, BIOS, sound mixer and save flash.
The game source itself is consumed unmodified as a pinned submodule.

Because the original code runs, battle formulas, trainer AI, encounter rates, frame timing and the
original games' own quirks are correct by construction rather than by effort.

## Status: experimental

**It plays, and it breaks.** The intro, the overworld, saving, the Pokémon Center, the PC, the
summary screen, the town map and battles all work; the furthest recorded session is about
twenty-three minutes. Every one of those screens crashed the first time somebody opened it, and the
next unvisited screen is the next crash — that is the current state of the project, not a temporary
embarrassment.

The reason is worth knowing before you play: the GBA has no memory protection, so upstream code that
reads through a null pointer gets harmless garbage from the BIOS region. Here the same read is a
segfault. Nine of those have been found so far, every one by playing rather than by reading, and each
was only reachable once the thing before it worked. **Which is why testing is the most useful thing
anyone can do for this project right now.**

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for where it is going.

## Playing it, and sending back what breaks

### 1. Build it

Dependencies, all in **32-bit** form, are listed in [`docs/BUILDING.md`](docs/BUILDING.md). On Arch:

```sh
sudo pacman -S --needed gcc lib32-glibc cmake ninja lib32-sdl3 lib32-libpulse \
                        arm-none-eabi-gcc arm-none-eabi-binutils arm-none-eabi-newlib libpng
```

```sh
git clone --recurse-submodules https://github.com/SoftARV/frlg-native.git
cd frlg-native

# the reference ROM: the port links against its symbols, so this comes first
cd vendor/pokefirered && ./build_tools.sh && make -j"$(nproc)" modern && cd ../..

cmake --preset linux-release
cmake --build --preset linux-release
```

No ROM of your own is needed for this: the reference ROM is built from the decompilation's own
sources, which is what step one does. See [Legal](#legal).

### 2. Play it

```sh
mkdir -p ~/.local/share/frlg-native
FRLG_LOCKSTEP=pace \
FRLG_SAV=~/.local/share/frlg-native/play.sav \
FRLG_INPUT_RECORD=/tmp/session.trace \
  ./build/linux-release/ports/desktop/frlg-native 120000 60
```

- `FRLG_LOCKSTEP=pace` — the clock a recording needs: real time, but one frame per game step, so the
  trace replays exactly ([ADR 0013](docs/adr/0013-lockstep-capture-clock.md)).
- `FRLG_SAV` — your save. Keep it somewhere that survives; the game writes it when you choose SAVE.
- `FRLG_INPUT_RECORD` — every keypress, one line per frame on which it changed.
- The two numbers are a frame limit and a display rate. `120000` is about thirty-three minutes.

Arrow keys move, **X** is A and **Z** is B, **Enter** is Start and **Backspace** is Select, **A** and
**S** are L and R. **Esc** or the window's close button quits.

**Before each session, copy your save**: a trace only replays against the save the run *started*
with, and playing changes it.

```sh
cp ~/.local/share/frlg-native/play.sav /tmp/before-session.sav
```

### 3. Send back what broke

When it crashes it prints a backtrace and exits 3. What makes a report actionable:

| | |
| --- | --- |
| the trace | `/tmp/session.trace` |
| the save it started from | `/tmp/before-session.sav` |
| the terminal output | the backtrace, and the last few lines before it |
| what you were doing | "opened the town map", "the rival switched Pokémon" |

With the trace and that save, any crash replays exactly — headless, in seconds, as many times as it
takes to fix:

```sh
FRLG_LOCKSTEP=1 FRLG_SAV=/tmp/before-session.sav FRLG_INPUT=/tmp/session.trace \
  ./build/headless/ports/desktop/frlg-native 90000 0
```

That needs the `headless` preset, which wants no SDL at all:

```sh
cmake --preset headless && cmake --build --preset headless
```

Issues go to [the tracker](https://github.com/SoftARV/frlg-native/issues) — the
"Something broke while playing" template asks for exactly the four things above.

**Sound and visuals count too.** Several real bugs were found by ear — effects too quiet, music
missing instruments, a sound that would not bend — and none of them showed up in any measurement
until somebody said it sounded wrong. If something looks or sounds off, say so even vaguely; that is
usually enough to find it.

## Documentation

| Document | Contents |
| --- | --- |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | How every part of the project fits together |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Phased plan and current milestone |
| [`docs/BUILDING.md`](docs/BUILDING.md) | Per-platform build instructions |
| [`docs/adr/`](docs/adr/) | Why the load-bearing decisions were made |
| [`docs/spikes/`](docs/spikes/) | Investigations, including the ones that were wrong |
| [`docs/required-to-function.md`](docs/required-to-function.md) | What is shipped, and what you supply |

## Legal

This project ships no assets, dialogue, audio or data from the original games. It builds from the
`pret` decompilation, which reconstructs the game's *source code*, and reads game content from a
ROM you supply and must legally own.

Note that the decompilation repository itself does contain ROM-derived assets. Developer builds
compile those in and **must not be distributed**; only builds using the first-boot importer are
distributable.

Nintendo, Game Freak and The Pokémon Company own the trademarks and the original work. This is an
unaffiliated preservation and portability project, with no association with, or endorsement by,
any of them.
