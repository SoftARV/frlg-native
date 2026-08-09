# ADR 0003 — 32-bit first, 64-bit ready

**Status:** accepted

## Context

`data/*.s` contains 789 `.4byte` directives, many of them symbol references: pointers embedded
directly inside data blobs. The script VM reads them back with `ScriptReadWord`
(`vendor/pokefirered/src/script.c:188`) and casts the result to a pointer. Battle scripts, event
scripts and map data all work this way.

On a 64-bit build those pointers truncate and the script engine fails at the foundations. This is
not a long tail of small fixes; it is a property of the data format.

Apple platforms are 64-bit only, so any 32-bit-only answer excludes macOS and iOS permanently.
`wasm32` and Android `armeabi-v7a` are 32-bit, so those are unaffected either way.

## Decision

Build 32-bit first, but route every read of a pointer embedded in game data through an accessor
from the first commit rather than a raw cast. While 32-bit, the accessor is the identity function.

The 64-bit migration then changes the data pipeline, not the game code: emit 32-bit offsets instead
of pointers, and make the accessor add the arena base.

## Consequences

- A playable build arrives far sooner; the host assembler consumes `data/*.s` unmodified.
- This shares one mechanism with [ADR 0006](0006-rom-supplied-data.md): the importer's relocation
  pass rewrites ROM-address pointers to host addresses in place, which works precisely because
  32-bit host pointers fit the ROM's 4-byte slots. The 64-bit migration is therefore the same piece
  of work for both decisions — regenerate the data into a native layout with wider slots.
- macOS and iOS are deferred to Phase 8. This is the real cost, and it is accepted knowingly.
- The accessor indirection must be applied with discipline from the start. If raw casts creep back
  in, this decision quietly degrades into "32-bit only" and nobody notices until Phase 8.
- 32-bit dependencies must be available on every build host. `gcc -m32` and `/usr/lib32` multilib
  are verified working here; 32-bit SDL3 is packaged (`lib32-sdl3`) but not yet installed. This is
  a standing tax of the decision: every third-party library the port gains needs a 32-bit build.

## Alternatives rejected

**64-bit clean from day one** — correct end state, but requires an `.s`-to-C converter plus a full
struct-layout and save-serialisation audit before anything can render. Weeks of pipeline work
before the first pixel, with no feedback along the way.

**32-bit forever** — simplest, and permanently excludes Apple platforms. Google Play also requires
64-bit binaries, so it would eventually cost Android too.

**Mapping the arena below 4 GiB so real pointers fit in 32 bits** — works on Linux, fails on macOS
and iOS where the low 4 GiB is reserved. Solves the problem only on the platforms that did not
have it.
