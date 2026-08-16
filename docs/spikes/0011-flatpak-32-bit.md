# Spike 0011 — Can a 32-bit game ship in a Flatpak?

**Question:** Phase 8 ships Linux first, and Flatpak is the natural format for a launcher that is
already a GNOME application ([ADR 0017](../adr/0017-launcher-is-a-separate-program.md)). But the game
is `-m32` ([ADR 0003](../adr/0003-pointer-width.md)). Can a Flatpak carry a 32-bit binary at all, and
does it have the SDL3 that binary needs — or does SDL3 have to be built from source for i386, which
would make the manifest an order of magnitude more work?

**Status: answered — yes, and nothing has to be built from source.** The game runs inside a real
sandbox against the runtime's own 32-bit SDL3. Two things are required to get there and neither is
guessable from the documentation: **`--allow=multiarch`**, and **an ELF interpreter that exists**.

## What the runtime already provides

`org.freedesktop.Platform.Compat.i386//25.08` — 127 MB download, 305 MB installed — carries the
32-bit side of the freedesktop runtime, and it is not a stub:

```
libSDL3.so.0.2.30: ELF 32-bit LSB shared object, Intel i386, version 1 (SYSV), stripped
```

Genuine i386 SDL3, 956 libraries in all. The SDK side exists too — `org.freedesktop.Sdk.Compat.i386`
and `org.freedesktop.Sdk.Extension.toolchain-i386` — so building 32-bit inside the sandbox is
supported, though this spike did not need it.

`Compat.i386` is **not** declared as an extension point by either `org.freedesktop.Platform` or
`org.gnome.Platform`. The *application* opts into it through `add-extensions`, which matters: the
launcher can keep `org.gnome.Platform` as its runtime and still mount the 32-bit layer for the game.
The runtime choice never has to be traded against the game's word size.

## The game asks for very little

| the game needs | in `Compat.i386` |
| --- | --- |
| `libc.so.6` | yes |
| `libgcc_s.so.1` | yes |
| `libm.so.6` | yes |
| `libSDL3.so.0` | yes |

Four libraries, all present. No bundling, no vendored dependency.

The version gap is real and turned out not to matter. The runtime has SDL **3.2.30**; this machine
builds against **3.4.14**, and `platform/host/CMakeLists.txt` sets no minimum. Of the 29 SDL symbols
the binary imports, **zero** are missing from 3.2.30 — and the game then ran against it.

## The two things that are required

**`--allow=multiarch`.** Without it the sandbox's seccomp filter rejects the 32-bit syscall ABI and
the process dies on **SIGSYS** (exit 159) before printing a line. Nothing in the failure names the
cause; stdout is lost with the signal, so it presents as silence. This is the same permission Steam
and Wine need.

**An ELF interpreter that exists.** The binary hard-codes `/lib/ld-linux.so.2`, and the sandbox has
no such path — the loader is shipped, but at `/app/lib/i386-linux-gnu/ld-linux.so.2` where the
extension mounts. The result is `execve(...) = ENOENT` **on a file that is present**, which reads as
a missing binary rather than a missing loader. This spike stepped around it by invoking the loader
explicitly:

```sh
/app/lib/i386-linux-gnu/ld-linux.so.2 --library-path /app/lib/i386-linux-gnu /app/bin/frlg-native
```

Shipping wants one of: `patchelf --set-interpreter` at build time, or a wrapper script. Not decided
here.

With both in place the game imports a ROM and runs indefinitely — 106,919 audio frames in the test,
no crash.

## A wrong turn worth recording

Before building a real sandbox, this spike ran the game against the runtime's libraries the cheap
way, with `LD_LIBRARY_PATH` on the host. It died:

```
*** stack smashing detected ***: terminated        (SIGABRT, exit 134)
```

That looked like an ABI break between SDL 3.2 and 3.4 and was nearly written up as one. **It was an
artefact of the test** — host glibc against a libSDL3 built for the runtime's — and it disappears in
a real sandbox, where both come from the same runtime. The lesson is narrow and worth keeping: a
sandbox cannot be approximated by a library path, and a failure produced by a configuration nothing
ships is not evidence about the configuration that does.

## The save path moves, and now it is measured

Predicted before the spike, confirmed by it. Inside the sandbox the import wrote:

```
kept as /home/miguel/.var/app/io.github.softarv.FrlgSpike/data/frlg-native/cache/41cb23d8.cart
```

`XDG_DATA_HOME` becomes `~/.var/app/<app-id>/data`, so `~/.local/share/frlg-native` — where every
existing save and cached cart lives — is invisible to a packaged build. A player who installs the
Flatpak over an existing setup re-imports their ROM and does not find their saves.

That needs a decision, not a discovery: migrate on first run, or grant `--filesystem` access to the
old path, or both. It is independent of everything else here and is the part that touches players.

**Decided** in [ADR 0021](../adr/0021-linux-ships-as-a-flatpak.md): documented rather than automated.
Only someone who played from a build tree before installing has anything to move, and the save format
is the cartridge's either way, so moving it is a file copy. The README's "where your data lives" table
is where a player is told.

## What was still open

Written when the spike closed. Two of the three have been answered since, and saying which keeps this
page from reading as a list of things nobody did.

- **Build inside the sandbox or ship a prebuilt binary.** Both are possible; this spike shipped a
  prebuilt one. **Settled:** prebuilt, per [ADR 0021](../adr/0021-linux-ships-as-a-flatpak.md) — the
  game's build needs agbcc, an `arm-none-eabi` toolchain and a byte-matching reference build to
  generate its manifest from, none of which belong in an application manifest. It is also why
  Flathub, which builds submissions on its own infrastructure, is not reachable yet.
- **The launcher.** Only the game was packaged here. The two-binary, two-runtime arrangement is
  sound in principle — the launcher's GNOME runtime can mount `Compat.i386` for the game — but it
  has not been built. **Built, and it works.**
  `ports/desktop/flatpak/io.github.softarv.frlg.yml` carries both: the launcher as a meson module,
  the game installed as a binary behind `frlg-native-wrapper.sh`, which invokes the i386 loader by
  path for the reason this spike found.
- **Audio under the sandbox.** The run reported 773 starved and 8295 dropped audio frames, expected
  with a dummy driver under lockstep, but not yet checked with a real one. **Still open** — those
  numbers are evidence of the dummy driver and of nothing else.
