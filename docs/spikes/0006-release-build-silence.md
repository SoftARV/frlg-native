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

## Narrowing, and one hypothesis already dead

The port now reports what it mixed on every run, device or no device, which measures this directly:

| Build | Frames reaching the mixer | Non-silent | Peak |
| --- | --- | --- | --- |
| `headless` (`-O0`) | 1199 | 956 | 110 |
| `linux-debug` (`-O0`) | 1199 | 956 | 110 |
| `linux-release` (`-O2`) | **1199** | **0** | **0** |

**The first hypothesis was wrong.** It said the sound header's `ident` — which upstream notes
"should be volatile but isn't" — might read stale at `-O2` and make `SoundMain` bail every frame. It
does not: the sink is called 1,199 times in *both* builds, so `SoundMain` runs, mixes, and hands over
a buffer every frame. Only the contents differ.

And they differ absolutely, not subtly: **exact zeros, peak 0**. That is not arithmetic drifting, it
is nothing being added to a cleared buffer at all. So either no channel is ever active, or every
active channel mixes at zero volume. That points at note allocation or the envelope rather than at
the mixing loop, and the two `-O0` builds agreeing to the sample says the pipeline is otherwise
deterministic.

Next measurement, not next guess: count channels that pass `agb_m4a_envelope_step` per frame in both
builds. If it is zero at `-O2`, the fault is in `ply_note` or `MPlayMain`; if it is non-zero, it is in
the envelope's volume fold.

If that is it, the fix is a deliberate decision rather than a patch: make the shared state volatile
in our own code, or route the mixer off the signal handler. It should not be reached for before the
measurement.

## The gap this exposes, and what actually closes it

**No test tier had ever exercised optimised code**: the golden tier runs the `headless` preset, which
is a `Debug` build. That is a real gap and worth stating. But running the goldens against an optimised
build would **not** have caught this, and it is worth being precise about why: the optimised build
passes all four golden frames pixel-exactly, tolerance zero. Rendering is identical. A screenshot
cannot see silence.

What closes it is measuring the audio, which `tools/audio_check.py` now does as part of the test
suite. It needs no sound hardware — the null host refuses to open a device and the port measures what
the mixer produced regardless — and it fails on the optimised build today, which is the point.

The general lesson is the one worth keeping: **a subsystem with no observable output has no tests, no
matter how many tests it has.** The mixer had 119 passing unit tests and four passing golden frames
while producing pure silence in the build we would ship.
