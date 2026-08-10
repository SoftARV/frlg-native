# Spike 0006 — The optimised build is silent

**Question:** host audio works. The Debug build plays the game's music; the optimised build plays
nothing. What differs?

**Status: reproduced and scoped, not diagnosed.** Recorded now because the scoping rules out the
obvious answers, and because it exposes a gap in what the test tiers actually cover.

## Reproduction

Deterministic, both drivers dummied so the two runs differ only in build type:

```sh
cmake --build --preset linux-debug
cmake --build --preset linux-release
for b in linux-debug linux-release; do
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ./build/$b/ports/desktop/frlg-native 2400 30 | grep 'first non-silent'
done
```

| Build | Type | Audio | Frame 2400 |
| --- | --- | --- | --- |
| `linux-debug` | `Debug` (`-O0`) | non-silent | 166 colours |
| `linux-release` | `RelWithDebInfo` (`-O2`) | **silent** | 166 colours |

The port reports the first frame in which any sample is non-zero, so this measures what the mixer
produced, not what the device did with it. Both builds open the stream and submit frames at
13,379 Hz; the optimised one submits nothing but zeros.

## What it is not

- **Not `NDEBUG`.** `RelWithDebInfo` defines it, but `include/config.h` defines it unconditionally
  anyway, so the compiler's copy changes nothing.
- **Not strict aliasing.** The obvious suspect, given how much of the m4a translation type-puns
  deliberately. Rebuilding the whole tree with `-fno-strict-aliasing` changes nothing, so the flag
  was reverted rather than left in on the theory that it might help with something else.
- **Not rendering.** Both builds produce an identical frame 2400. Whatever diverges is confined to
  the sound path.

## Where to look next

The sound engine is reached from two contexts on one thread: the game calls into it normally, and
`SoundMain` runs inside the V-blank signal handler. Upstream says so itself, of the field that
guards exactly that:

> This field should be volatile but isn't. This could potentially cause race conditions.

At `-O0` a global is reloaded on every access and the guard works by accident. At `-O2` the compiler
may keep `SoundInfo.ident` — or any other sound state — in a register across the point where the
signal arrives, which would make the lock read stale and `SoundMain` bail every frame. That is the
first hypothesis to test, and it is testable: count entries to `SoundMain` against early returns in
both builds.

If that is it, the fix is a deliberate decision rather than a patch: make the shared state volatile
in our own code, or route the mixer off the signal handler. It should not be reached for before the
measurement.

## The gap this exposes

**The golden tier only ever runs the `headless` preset, which is a `Debug` build.** No test tier has
ever exercised optimised code. The port could have been miscompiling something for phases and no
test would have said so — this was found by listening, not by testing. Rendering happens to agree
here, which is luck rather than coverage.

Running the goldens against an optimised build as well is the obvious answer, and cheap: the golden
harness already takes a binary path.
