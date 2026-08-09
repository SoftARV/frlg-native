# frlg-native

A multiplatform native port of the `pret/pokefirered` decompilation of Pokémon FireRed and
LeafGreen.

This is **not an emulator and not a static recompilation.** The decompilation is complete C, so the
game is compiled directly for the host CPU and linked against a small reimplementation of the
hardware it expects — the GBA's picture unit, DMA, interrupts, BIOS, sound mixer and save flash.
The game source itself is consumed unmodified as a pinned submodule.

Because the original code runs, battle formulas, trainer AI, encounter rates, frame timing and the
original games' own quirks are correct by construction rather than by effort.

## Status

Early — the reference ROM builds and the project is scaffolded; the port itself does not run yet.
See [`docs/ROADMAP.md`](docs/ROADMAP.md).

## What you need

The app ships **no game data**. On first boot it asks for a legally obtained ROM of a supported
revision, verifies it, extracts what it needs, and releases it — the ROM is never copied into the
app's cache. See [`docs/required-to-function.md`](docs/required-to-function.md).

## Documentation

| Document | Contents |
| --- | --- |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | How every part of the project fits together |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Phased plan and current milestone |
| [`docs/new-features.md`](docs/new-features.md) | What this port adds beyond the cartridge |
| [`docs/required-to-function.md`](docs/required-to-function.md) | What is shipped, and what you supply |
| [`docs/adr/`](docs/adr/) | Why the load-bearing decisions were made |
| [`docs/BUILDING.md`](docs/BUILDING.md) | Per-platform build instructions |

## Quick start

```sh
git clone --recurse-submodules <this repo>
cd frlg-native
cmake --preset linux-debug
cmake --build --preset linux-debug
```

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
