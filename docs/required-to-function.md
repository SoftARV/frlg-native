# What this port requires

The packaged app needs one user-supplied input on first boot: a legally obtained ROM of a
supported Pokémon FireRed or LeafGreen revision.

The port ships **no game data**. See [ADR 0006](adr/0006-rom-supplied-data.md) for why.

## What is shipped

- The port's own code — the virtual GBA, the host backends, the launcher.
- Compiled game logic, built from the `pret` decompilation's C source.
- A **generated manifest**: symbol names, addresses, sizes and relocation entries, produced
  mechanically by our own ROM build (`make syms` → 50,590 symbols; the linker map → 35,037 lines).

The manifest contains no ROM bytes, no graphics, no dialogue, no audio samples.

## What is not shipped

Graphics, tilesets, sprites, palettes, dialogue and every text string, music, sound effects, cries,
map layouts, species and move tables, trainer parties, encounter tables. All of it is read from the
player's ROM.

## First boot

1. The player supplies a `.gba` ROM.
2. Its SHA-1 is verified against the supported revisions. Unsupported dumps are **rejected**, not
   decoded at guessed addresses.
3. The image is loaded into the cart region and its embedded pointers relocated.
4. A private cache is written to the platform's user-data directory.
5. The ROM is released. It is **not** copied into the cache. Later launches read only the cache.

## Supported revisions

Determined by the revisions the decompilation builds and we can therefore generate a manifest for:
FireRed and LeafGreen, revisions 0 and 1, English. The exact hashes live beside the importer.

## Developer builds

A developer build compiles the data in, the way the decompilation's own ROM build does, so
renderer work can proceed without a finished importer. **Developer builds are never distributed** —
they embed ROM-derived data and exist only on a contributor's machine.

## Not required

No emulator, no BIOS dump, no Python at runtime, no network connection. Building from source
additionally needs the toolchain in [BUILDING.md](BUILDING.md).
