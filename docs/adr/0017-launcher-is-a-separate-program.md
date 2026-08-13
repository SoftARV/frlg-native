# ADR 0017 — The launcher is a separate program, and it is platform-specific

**Status:** accepted

## Context

Phase 7 gives the port an importer: verify a ROM, relocate it, keep it. Nothing in it decides *when*
to do that, which ROM, or which save to play — those are a launcher's questions, and the roadmap put
a launcher in phase 8 without saying what it would be built from.

Miguel writes GNOME applications: Vala, Blueprint, GTK4, libadwaita. That is what he wants the
launcher to be, and it is a reasonable thing to want — an author who enjoys maintaining a codebase
maintains it.

It also does not reach everywhere. GTK4 runs on Windows and macOS but libadwaita is unpolished and
non-native there and bundling the runtime is heavy; on Android, iOS and web it is not viable at all.
Phase 8 targets Windows, Android and web.

And there is a constraint that removes the choice entirely: **the game is `-m32`** (ADR 0003).
`lib32-sdl3` exists, which is why the game can link SDL at all; `lib32-gtk4` does not exist. A GTK4
launcher cannot be linked into the game binary on this machine, today, whatever anyone prefers.

## Decision

**The launcher is a separate executable, and each platform gets the shell that suits it.**

- `frlg-native` — the game. SDL only, 32-bit, portable, and it knows nothing about any launcher.
- `frlg-launcher` — GNOME. Vala, Blueprint, GTK4, libadwaita, its own Meson project, not built by the
  port's CMake build.

They communicate by **running each other**, not by linking: `--describe` asks the game what it is and
whether its ROM has been imported, `--import <rom>` imports without playing, and playing is spawning
the binary with the chosen save in the environment. `key=value` lines, which need no parser on either
side.

**The game must always run with no launcher at all.** It does today, and that is what keeps traces,
headless replays and CI independent of any of this.

## Consequences

A launcher is a platform shell, not the app. Android gets an Activity, Windows gets whatever suits
Windows, and on web the page *is* the launcher. None of that is a compromise forced by this decision;
it is what the decision recognises.

The game binary stays the authority on itself. One binary is one title — FireRed and LeafGreen are
different builds, not different data, because `GAME_VERSION` is compile-time — so a catalogue is a
launcher over several game binaries, and the launcher asks each one what it is rather than keeping a
description that can go stale.

**Game options are per-save, not per-application.** The seven the game exposes — sound, text speed,
battle style, and the rest — live as bitfields inside a checksummed save sector with two rotating
copies. The launcher will show them, but it will never parse that format: the port exposes them, the
way it exposes import, because the port already compiles the game's own structs and checksum code and
a format with two implementations in two languages has one implementation that is wrong. Port
settings, which the game has no concept of, are the opposite: global, and a plain config file.

A contributor without Vala can still build and run everything. The launcher is optional, which is
also what keeps it from becoming a dependency of the test suite.
