# frlg-native

A multiplatform native port of the `pret/pokefirered` decompilation of Pokémon FireRed and
LeafGreen.

This is **not an emulator and not a static recompilation.** The decompilation is complete C, so the
game is compiled directly for the host CPU and linked against a small reimplementation of the
hardware it expects — the GBA's picture unit, DMA, interrupts, BIOS, sound mixer and save flash.
The game source itself is consumed unmodified as a pinned submodule.

## Status

Early. See [`docs/ROADMAP.md`](docs/ROADMAP.md) for what works and what does not.

## Documentation

| Document | Contents |
| --- | --- |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | How every part of the project fits together |
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Phased plan and current milestone |
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

This repository contains no copyrighted material from the original games. It builds from the
`pret` decompilation, which reconstructs the game from source. You must own the game.
Nintendo, Game Freak and The Pokémon Company own the trademarks and the original work; this is an
unaffiliated preservation and portability project.
