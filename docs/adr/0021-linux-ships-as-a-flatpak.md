# 0021 — Linux ships as one Flatpak carrying both binaries

**Status:** accepted
**Date:** 2026-08-15

**Builds on** [ADR 0003](0003-pointer-width.md), which makes the game 32-bit, and
[ADR 0017](0017-launcher-is-a-separate-program.md), which makes the launcher a separate GNOME
program. Those two decisions together are what makes packaging a question rather than a formality.

## Context

Phase 7 ended with a binary that carries no game data, which is the only reason anything can be
distributed at all. Phase 8 ships it, Linux first, because the launcher is already a GNOME
application and the platform the port can ship on today is the one it was written on.

The awkward part is that there are **two binaries with different word sizes**. The game is `-m32`
and links SDL3; the launcher is 64-bit and links GTK4 and libadwaita. There is no `lib32-gtk4`, so
they cannot be one binary even if that were wanted. Any packaging format has to carry both, and the
format also has to answer where a player's ROM import and saves live, and how an update reaches
them.

[Spike 0011](../spikes/0011-flatpak-32-bit.md) established that Flatpak can do this and what it
costs.

## Decision

**One Flatpak, `io.github.softarv.frlg`, carrying the launcher and the game.**

The launcher runs on `org.gnome.Platform`; the game takes its 32-bit libraries from
`org.freedesktop.Platform.Compat.i386`, which the **application** opts into rather than the runtime
declaring it. That asymmetry is the whole reason this works: the launcher keeps the runtime it wants
and the game still gets i386 libraries, so the word size never has to be traded against the runtime.

Four things in the manifest are load-bearing, and **each one fails silently when absent** — which is
the reason they are written down here rather than left to the manifest to explain:

| | what its absence looks like |
| --- | --- |
| `--allow=multiarch` | seccomp rejects the 32-bit syscall ABI; SIGSYS with stdout unflushed, so the process dies without a word |
| a wrapper invoking `ld-linux.so.2` by path | the binary hard-codes `/lib/ld-linux.so.2`, absent in the sandbox; `execve` returns ENOENT for a file that is plainly there |
| the `GL32` extension | SDL finds no renderer at all, not even software, and exits before drawing — from the launcher this looks like a window that closes and reopens |
| `FRLG_GAME_BIN` | the launcher's other lookups are a development tree and a sibling path, neither of which exists in a sandbox |

**The game is installed, not built, by the manifest.** Its build needs `agbcc`, an `arm-none-eabi`
toolchain, and a byte-matching reference build to generate the symbol manifest from. That is not a
preference — it is a constraint on *where* the game can be built at all.

## Consequences

**Updates come with the format.** Phase 8 asks for an update pipeline and Flatpak is one, which is
most of why it wins over a self-contained image.

**The data directory moves**, to `~/.var/app/io.github.softarv.frlg/data/frlg-native/`. Saves are
unaffected in *format* — they are the cartridge's ([ADR 0019](0019-saves-match-the-cartridge.md)) —
so moving between a development tree and the package is a file copy and nothing else. Only someone
who played from a build tree before installing has anything to move, so this is documented rather
than automated.

**The required graphics driver is a property of the machine, not the manifest.** `GL32.default` is
Mesa and `GL32.nvidia-*` is nvidia, and the one that must be installed is the one matching the GPU
in use. A laptop that switches between integrated and discrete changes the answer *after* the app is
installed. `download-if: active-gl-driver` resolves it at install time; nothing re-resolves it when
the machine changes its mind.

**A store that builds from source is not yet reachable.** Flathub builds submissions on its own
infrastructure, and the game's build needs a toolchain and a reference build that do not belong in
an app manifest. Direct distribution — a `.flatpak` bundle, or a repository we host — works today.
Flathub needs that question answered first, and this ADR does not answer it.

## Alternatives

**AppImage.** Self-contained and needs no runtime, which is genuinely simpler for a one-file
handover. Rejected because it gives no update mechanism, no sandbox, and no answer to the two-word-
size problem beyond bundling both architectures' libraries by hand — including a graphics driver,
which is exactly the part that must match the host and therefore cannot be bundled.

**Distribution packages.** A PKGBUILD would be the shortest path on the machine this is developed
on, and would sidestep the sandbox entirely. Rejected as the *primary* format because it reaches
only one distribution; it remains a reasonable thing to add later, and it is much easier than this
was.

**Building the game inside the manifest.** Rejected: `agbcc` and the reference build make it an
order of magnitude more work than installing a binary, and it would have to be solved anyway before
any source-building store, where it can be faced on its own terms.
