# Spike 0011 — Can a 32-bit game ship in a Flatpak?

**Question:** Phase 8 ships Linux first, and Flatpak is the natural format for a launcher that is
already a GNOME application ([ADR 0017](../adr/0017-launcher-is-a-separate-program.md)). But the game
is `-m32` ([ADR 0003](../adr/0003-pointer-width.md)). Can a Flatpak carry a 32-bit binary at all, and
does it have the SDL3 that binary needs — or does SDL3 have to be built from source for i386, which
would make the manifest an order of magnitude more work?

**Status: half answered.** The packaging question is answered and the answer is better than expected:
**nothing has to be built from source.** The runtime already ships everything the game links, i386
included. But the game **aborts on startup** when run against that runtime's SDL3, and the cause is
not yet known. The format is viable; something in the middle is not yet right.

## What the runtime already provides

`org.freedesktop.Platform.Compat.i386//25.08` — 127 MB download, 305 MB installed — carries the
32-bit side of the freedesktop runtime, and it is not a stub:

```
libSDL3.so.0.2.30: ELF 32-bit LSB shared object, Intel i386, version 1 (SYSV), stripped
```

Genuine i386 SDL3. The SDK side exists too — `org.freedesktop.Sdk.Compat.i386` for headers and
`org.freedesktop.Sdk.Extension.toolchain-i386` for the compiler — so building 32-bit inside the
sandbox is supported. This is the mechanism Steam and Wine use; it is not a hack.

`Compat.i386` is **not** declared as an extension point by either `org.freedesktop.Platform` or
`org.gnome.Platform`. It is opted into by the *application* manifest through `add-extensions`, which
matters: it means the launcher can keep `org.gnome.Platform` as its runtime and still mount the
32-bit layer for the game. The runtime choice does not have to be traded against the game's word
size, which was the outcome this spike was most afraid of.

## The game asks for very little

The port links SDL and nothing else, by design, and it shows:

| the game needs | in `Compat.i386` |
| --- | --- |
| `libc.so.6` | yes |
| `libgcc_s.so.1` | yes |
| `libm.so.6` | yes |
| `libSDL3.so.0` | yes |

Four libraries, all present. No bundling, no `--filesystem` escape hatch, no vendored dependency.

The version gap is real but not a blocker on paper. The runtime has SDL **3.2.30**; this machine
builds against **3.4.14**, and `platform/host/CMakeLists.txt` sets no minimum. Comparing the symbols
the binary actually imports against the ones the older library exports:

```
port needs      29 SDL symbols
3.2.30 provides 1208
missing         0
```

Every entry point the port uses has existed since 3.2.

## Where it fails

Running the built game with `LD_LIBRARY_PATH` pointed at the runtime's 32-bit libraries:

```
*** stack smashing detected ***: terminated        (SIGABRT, exit 134, core dumped)
```

The same binary runs correctly against the host's 3.4.14. It dies before printing anything — checked
under a pty, so this is not lost buffering — and the backtrace cannot unwind past `abort`.

**This test is not faithful, and the result must not be read as "SDL 3.2.30 breaks the port."** It
mixes this host's glibc with a libSDL3 built against the runtime's. A real sandbox would use the
runtime's glibc for both. The finding is that *this mixture* aborts; the cause is unknown.

Three candidates, in no order:

1. A behavioural or ABI difference between SDL 3.2 and 3.4 that the port depends on without knowing.
2. The glibc mixture, which no shipping configuration would ever have.
3. A latent stack bug in our own host backend that the older library happens to expose. This one
   would be the most valuable outcome, and it is the reason the abort deserves chasing rather than
   dismissing once the sandbox test passes.

## What this does not answer

- Whether the game runs in a **real** sandbox, where glibc and SDL3 come from the same runtime.
  That needs `flatpak-builder`, which is not installed here (`org.flatpak.Builder` is a flatpak).
- Whether to build the game inside the sandbox with `toolchain-i386` or build outside and ship the
  binary. The spike shows both are possible; it does not choose.
- The save-path question, which is not about 32-bit at all: a sandboxed package relocates
  `~/.local/share/frlg-native`, and existing saves have to be migrated or reached deliberately.

## What it does answer

Flatpak can carry this port, the i386 layer is real and complete for our dependency set, SDL3 does
not need building, and the launcher's GNOME runtime is not in conflict with the game's word size.
The remaining risk moved from "can this be packaged" to "why does this one configuration abort" —
a much smaller question, and one with a test that will answer it.
