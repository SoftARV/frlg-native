# frlg-native

A multiplatform native port of the `pret/pokefirered` decompilation of Pokémon FireRed and
LeafGreen.

This is **not an emulator and not a static recompilation.** The decompilation is complete C, so the
game is compiled directly for the host CPU and linked against a small reimplementation of the
hardware it expects — the GBA's picture unit, DMA, interrupts, BIOS, sound mixer and save flash.
The game source itself is consumed unmodified as a pinned submodule.

Because the original code runs, battle formulas, trainer AI, encounter rates, frame timing and the
original games' own quirks are correct by construction rather than by effort.

That is the floor, not the ceiling. The host is not a Game Boy Advance, and the limits the original
was built inside — a 240×160 screen, a 13 kHz sound mixer, 256 KB of work RAM — are not the game's
requirements. Most of the game will stay as it is, and a lot of it will not: **this is an enhanced
port, not a preservation project.** The first such change has landed already, a new save now
defaulting to stereo sound. What keeps that honest is the reference emulator the test suite runs
against, which says exactly what differs from the original so we can tell a change we chose from a
bug we did not.

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
  ./build/linux-release/ports/desktop/frlg-native 120000 60
```

- `FRLG_LOCKSTEP=pace` — the clock a recording needs: real time, but one frame per game step, so the
  trace replays exactly ([ADR 0013](docs/adr/0013-lockstep-capture-clock.md)).
- `FRLG_SAV` — your save. Keep it somewhere that survives; the game writes it when you choose SAVE.
- The two numbers are a frame limit and a display rate. `120000` is about thirty-three minutes.

**You do not have to set up recording, and you do not have to remember anything.** Every run keeps its
own session under `~/.local/share/frlg-native/sessions/`: what you pressed, a copy of your save as it
was when the run *began*, and everything printed. The last five runs are kept.
`FRLG_NO_RECORD=1` turns it off ([ADR 0016](docs/adr/0016-every-session-records-itself.md)).

Arrow keys move, **X** is A and **Z** is B, **Enter** is Start and **Backspace** is Select, **A** and
**S** are L and R. **Ctrl+Q** or the window's close button quits — deliberately not a bare key, since
one stray press ends a recording.

### 3. Send back what broke

When the game hits a bug it stops, tells you what happened, and writes **one file**:

```
frlg-native: the game stopped because of a bug -- SIGSEGV at 0x00000014 on frame 12480

Everything needed to reproduce it is in one file:
  ~/.local/share/frlg-native/sessions/2026-08-13-2334/report.zip
```

Attach that zip to an issue and say what you were doing — "opened the town map", "the rival switched
Pokémon". That is the whole job; the zip already holds the trace, the save the run started from, the
full log and where it stopped.

With those, any crash replays exactly — headless, in seconds, as many times as it takes to fix:

```sh
unzip -d /tmp/report ~/.local/share/frlg-native/sessions/<the one that broke>/report.zip
FRLG_LOCKSTEP=1 FRLG_SAV=/tmp/report/start.sav FRLG_INPUT=/tmp/report/input.trace \
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

## Licence

The code written for this project is **GPL-3.0-or-later** — see [LICENSE](LICENSE). That covers what
is actually ours: `platform/`, `ports/`, `launcher/`, `tools/`, `cmake/` and `docs/`.

It does not, and cannot, cover the decompiled game. `vendor/pokefirered` is a pinned submodule
carrying no licence of its own, and it is fetched by whoever builds rather than redistributed here.
Nothing in this repository grants rights over the original work, and no licence chosen here could.
