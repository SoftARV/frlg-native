# Spike 0007 — Measuring our sound against mGBA

**Question:** the PSG channels are implemented and the balance between them and direct sound was a
choice rather than something the hardware dictated. The golden tier compares our frames against mGBA
running the same ROM; can the same be done for sound, and what does it say?

**Verdict: yes, and it corrected two things I had assumed.** The music is structurally right. The
level and the balance are defensible. The one clear difference is not where I expected it, and the
change I made on that expectation was reverted.

## Method

`tools/mgba_audio.c` runs the reference ROM under libmgba and writes raw signed 16-bit stereo at a
rate the caller picks, using mGBA's own `blip_buf` to resample. `FRLG_PCM=<path>` makes the port dump
the same format from its audio sink — measured before the device is considered, so it needs no sound
hardware and works in the headless build.

```sh
build/headless/tools/mgba-audio vendor/pokefirered/pokefirered_modern.gba mgba.raw 2538 13379
FRLG_PCM=ours.raw build/headless/ports/desktop/frlg-native 2500 40
```

mGBA is aligned by **+38 frames**, the boot offset [spike 0004](0004-mgba-frame-alignment.md)
measured for video: mGBA runs the BIOS and the ROM's crt0 where the port enters `AgbMain` directly.

## What matches

Per-second RMS over 42 seconds tracks closely — every rise and fall in the same place. That is the
result worth having: the sequencer, the mixer and the PSG between them are playing the right notes at
the right times for the whole intro and title screen.

Our overall level runs about **1.3x** mGBA's. That is not necessarily a fault: mGBA's output gain is
its own convention, and nothing says the two should agree in absolute terms. It is recorded so a
future change can be seen against it.

## What does not match, and where it comes from

Comparing spectra over the title screen, with both normalised so only the *shape* is compared:

| Band | ours, PSG off | ours, PSG on | against mGBA |
| --- | --- | --- | --- |
| bass, 0–200 Hz | 1.19x | 1.68x | more |
| **mid, 200–1600 Hz** | 0.91x | **0.80x** | **less** |
| **high, 3200 Hz+** | **2.31x** | **1.97x** | **more** |

The mid band is where melodies sit and the high band is where grit does.

**The excess treble is the direct-sound path, not the PSG.** With the PSG switched off entirely it is
*worse* — 2.31x against 1.97x — because the PSG adds mid and bass energy and so lowers the high
band's share. That is the opposite of what I assumed.

## The change this reverted

On seeing 2.09x in the high band I concluded the PSG was aliasing: it point-samples square waves at
the mixer's rate, where mGBA band-limits with `blip_buf`. I added eightfold oversampling with a box
filter, which moved the figure to 1.97x — real, but small, and then the controlled measurement showed
the band is dominated by something else entirely.

It was reverted. The code did what it claimed, but it cost eight times the PSG's sample generation to
improve a band whose problem lies elsewhere, and keeping it would have left the tree carrying a
justification the evidence had already contradicted. The measurement stays here so that a future
decision about band-limiting starts from data rather than from the same guess.

## What it then found, once the arithmetic was read properly

Comparing *shares* of a normalised total is treacherous: adding energy in one band lowers every other
band's share, so "the mid band is 0.80x" did not mean the melody was quiet, it meant something else
had grown. The figure that says what a listener hears is the **melody band against the bass band**:

| | bass 0–200 | mid 200–1600 | mid ÷ bass |
| --- | --- | --- | --- |
| mGBA (reference) | 0.194 | 0.751 | **3.86** |
| ours, PSG off | 0.232 | 0.686 | 2.96 |
| ours, PSG on | 0.327 | 0.603 | **1.85** |

Switching the PSG on **halved** the melody-to-bass balance. It was adding bass, not melody — which is
what a listener reported independently as the new channels being hard to hear.

The cause was a **standing offset on every duty but one**. Each square swung symmetrically about
zero, so at one eighth duty its mean sat at three quarters of the volume, at a quarter duty at half.
A real channel's DAC puts out 0 to 15 and the hardware couples it through a capacitor, so no duty
carries an offset at all. Worse, the duty changes from note to note, so the offset moved: a thump
under the tune, spending headroom the melody needed.

Weighting each side of the square by the time spent on the other keeps the mean at zero and the swing
unchanged:

| | bass | mid | mid ÷ bass |
| --- | --- | --- | --- |
| before | 0.327 | 0.603 | 1.85 |
| **after** | **0.213** | **0.706** | **3.31** |
| mGBA | 0.194 | 0.751 | 3.86 |

From less than half the reference's balance to 86% of it, and the overall level barely moved
(1.34x to 1.29x mGBA). The remaining difference is small enough that it is no longer the obvious
thing to chase.

**Not established:** whether our direct-sound treble is wrong at all. The GBA's own PCM is 8-bit at
about 13 kHz and its DAC produces exactly this kind of content; mGBA's `blip_buf` resampling
deliberately removes it. Ours may be the more faithful of the two. Settling that needs a comparison
against hardware, not against another emulator.


## A level error the band comparison could not have caught

**Found by ear, in play, long after this spike was parked.** The port ignored `SOUNDCNT_H`'s
direct-sound volume bits, so the sampled music played at twice its intended level against the PSG
channels once the game applied the sound option.

This spike could not have found it. Every measurement here is of the **intro**, which is the one part
of the game that runs before `SetPokemonCryStereo` is called — until then `m4aSoundInit`'s
`SOUND_ALL_MIX_FULL` is in force and the port's behaviour was correct. The comparison was sound; its
sample was unrepresentative.

The register reads `3302` in the port and `3302` in the reference at the same frame, so no listening
test was needed once the question was asked properly: both FIFOs on both sides at half volume, PSG at
full, which is a mono downmix at unity gain rather than the hard-panned pair the port was handing over.

**What this says about the remaining ~14% gap** is that it should be re-measured somewhere the game
actually spends its time. `mgba-audio` cannot replay an input trace, so the oracle stops where the
intro does — that is the thing to fix before trusting another number from it.


## The noise channel was aliasing, and per-channel comparison is not available

**Chased after a player reported that the sound for fleeing a battle was not playing.** It was: the
effect starts on SE1, claims a CGB channel — `type=0C`, the noise channel, fixed pitch — and its
envelope decays from 10 the way it should. Every step of the sequencer's side was correct.

What was wrong is what that channel *sounds* like. The noise clock runs at up to 262 kHz against the
13.4 kHz this is mixed at, so one output sample spans many shift-register states. The port took the
last of them, at full amplitude, where the hardware's output is the average over the interval and the
reference's resampler band-limits it. Point-sampling a signal twenty times above Nyquist keeps energy
that should have cancelled, which is a channel both louder and harsher than it should be — and the
drums and the flee effect share it, so the effect had nothing to stand out against.

Averaging over each sample, weighted by how long each state lasted, is the fix. Below the mix rate it
reduces to what it replaced.

**Measured on the full mix against the reference**, per-band ratios over the intro:

| | before | after |
| --- | --- | --- |
| bass 40–250 | 1.152 | 1.122 |
| low-mid 250–900 | 1.321 | 1.245 |
| high-mid 0.9–2.5k | 1.230 | 1.186 |
| treble 2.5–6k | 1.239 | 1.196 |
| **spread** | **0.169** | **0.123** |

A uniform ratio across bands is what "same spectrum, different scale" looks like, and the scale is a
constant between two tools rather than a fault. The spread narrowing by a quarter is the shape getting
closer.

## Why this was not measured per channel

The obvious experiment — render one hardware channel at a time in both and compare — **cannot be done
with mGBA's API**, and an afternoon went into finding that out:

- `core->listAudioChannels` reports a single channel for the GBA core, not the four.
- `core->getAudioChannel(core, 0|1)` is the left and right of the *mixed* output, not a per-voice tap.
- Forcing the enable bits in `SOUNDCNT_L` every frame **leaks**: the sequencer rewrites them from
  inside the frame, so what is captured is "mostly one channel". It looks like a clean measurement and
  is not — the tell was a soloed square changing level when only the noise code had changed.

So per-channel numbers from that method are not evidence, and the two attempts recorded here in an
earlier draft were withdrawn for that reason. Full-mix band ratios are what this spike can honestly
compare, which is what the table above uses.


## Is the treatment system-wide? Only where a channel outruns the mix rate

Asked directly after the noise fix: does band-limiting belong on every channel, rather than being
applied one sound at a time? The answer is yes in principle and **measurably only for the noise
channel and for sweeps**, which is worth writing down so the next person does not re-derive it.

Each channel was given the same treatment — the two squares analytically, since a square is two levels
and only the time spent on each is needed, and the wave by walking the table entries a sample spans —
and then measured against the reference over the intro:

| | before | squares band-limited | wave too |
| --- | --- | --- | --- |
| bass 40–250 | 1.122 | 1.121 | 1.121 |
| low-mid 250–900 | 1.245 | 1.242 | 1.242 |
| high-mid 0.9–2.5k | 1.186 | 1.186 | 1.186 |
| treble 2.5–6k | 1.196 | 1.195 | 1.196 |

Nothing. Musical squares sit at a few hundred hertz against a 6.7 kHz Nyquist, and the wave's table
advances barely more than one entry per sample at ordinary pitches, so neither was aliasing to begin
with. The noise channel was the outlier because its clock reaches 262 kHz — twenty times the mix rate.

**Where the squares do matter is the sweeps**, which is what the reports were about. Rendering the
exclamation-bubble effect from a recorded trace, band-limiting changes **15.6% of that signal** while
leaving the music untouched — a sound that sweeps out of the audible range is exactly the case
point-sampling folds back as a squeal.

So the rule is not "band-limit everything because it is correct", it is **"band-limit anything whose
rate can exceed the mix rate"**, which is the noise channel always and the squares whenever a sweep
takes them there. Both now have tests naming that boundary rather than a level.

## What the rival theme's "missing instruments" was not

The same report was chased through the sequencer first, and four things were ruled out with
measurements worth keeping:

- **Voice count.** `maxChans` is 5 in this port and 5 in the reference, read out of `gSoundInfo` at the
  same frame. Not a limit we imposed.
- **Tracks going silent.** All ten tracks of the theme sound over a 65-second window. An earlier
  13-second window showed two of them silent throughout, which was **too short a sample** — track 1
  rests for many bars by design, and reporting it as dead was wrong.
- **Channel allocation.** 2065 notes requested over that window, **2 refused a channel** (0.1%). The
  allocation loop was also read against the original ARM line by line, including the parts that decide
  between a releasing channel and a sounding one, and it matches.
- **Dead instrument paths.** Every track, rendered on its own, produces audible output.

So nothing structural is missing, and what is left is timbre — which is where the aliasing above
belongs.


## The port was throwing away three quarters of the sound

**Found by chasing why short effects sounded "tiny".** They were not quiet; they were *gone*, along with
most of the game's brightness, and the reason is the rate the port handed audio over at.

13379 Hz is the rate the game runs its **FIFOs** at, not the rate the machine puts out. A GBA's four
hardware channels are analogue, and its FIFOs hold each sample for a whole period — so a real machine
emits square harmonics and the stair-step of that hold, both far above 6.7 kHz. Handing over a
13379 Hz stream is a perfect low-pass at exactly that line.

Measured against the reference at 48 kHz over the intro:

| | share of energy |
| --- | --- |
| below 6.69 kHz — what a 13379 Hz stream can carry | **25.1%** |
| above it — what the port was discarding | **74.9%** |

That also explains the shape of the complaints. A sweep effect climbs out of the band, so after the
noise and square band-limiting landed it correctly *vanished* rather than folding back as audible junk:
the aliasing had been standing in for the sound.

## The fix, and what it measures

The sampled side is now **held** across four output samples, which is what the FIFO does with it, and
the hardware channels are rendered at the output rate instead of the mixer's. Output is 53516 Hz.

Both at 53516 Hz, over the same window:

| band | ours | reference |
| --- | --- | --- |
| 60–400 Hz | 10.5% | 11.2% |
| 400–1500 Hz | 10.6% | 10.6% |
| 1500–4000 Hz | 3.8% | 3.9% |
| 4000–8000 Hz | 1.8% | 2.4% |
| 8000–16000 Hz | 60.6% | 58.5% |
| 16000–26000 Hz | 12.7% | 13.3% |

Every band within a couple of points, where two of them had been empty. **This spike's earlier numbers
were all measured inside the quarter of the spectrum that survived**, which is why they looked close to
the reference while the port sounded wrong: comparing 13379 Hz against 13379 Hz hid the loss on both
sides of the comparison. Any future audio comparison should be captured at the oversampled rate.

## What the balance measurements said on the way

The complaint pointed at the PSG being too quiet against the sampled side. It was not, and this is the
method that settled it — the reference *can* be muted on one side reliably, which the earlier attempt at
per-channel isolation could not manage:

`SOUNDCNT_H`'s direct-sound routing bits are written only by `m4aSoundInit` and by
`SetPokemonCryStereo`, so forcing them off every frame in `mgba-audio` does not leak, unlike the PSG's
own enable bits in `SOUNDCNT_L` which the sequencer rewrites constantly. With the reference's sampled
side muted:

- the hardware channels are **50%** of the reference's own mix, and **53%** of ours — a balance within
  7%, so nothing to fix there.


## The ledge hop: a sweep that never swept

The one effect still reported wrong after the output rate was fixed, and the fault was in the sweep
unit rather than anywhere the rate mattered.

Reading the song out of the ROM says what the sound is made of. `gSongTable[10]` is one track:

```
BC 00     KEYSH 0
BB 3C     TEMPO
BD 55     VOICE 85
BE 64     VOL 100
C0 3A     PAN
D1 33 34  a note, key 51
82        wait
BD 56     VOICE 86
D5 43 5C  a note, key 67
81 83 83  waits
B1        FINE
```

and voice 85 in its voicegroup is `09 3C 00 2E ...` — type `09` is FIX | CGB square 1, and the
pan/sweep byte `2E` is a **sweep**: time 2, downwards, shift 6. The ledge hop is a square that bends
down, which is why it was the sound that still sounded wrong when the flat ones came right.

The bug: `square_update` runs once per frame and re-reads the channel's period from the frequency
register, unconditionally. The sweep unit was advancing its own copy — and having it overwritten sixty
times a second. **The note never bent.**

On hardware the sweep writes its result *back* into the frequency register, which is exactly what makes
it survive; the register is the state. Writing it back makes the per-frame reload correct rather than
destructive. Measured on a note set up like the ledge hop's:

| frame | 0 | 2 | 4 | 6 | 8 |
| --- | --- | --- | --- | --- | --- |
| tone | 350 Hz | 308 | 277 | 251 | 231 |

Before the fix it read 350 Hz on every one of them.

**What this says about the other reports.** Three effects came right when the output rate stopped
throwing away everything above 6.7 kHz; this one needed a second, unrelated fix, and the two would have
been impossible to separate by ear. Reading the effect's data out of the ROM — what instrument, what
sweep, what notes — is what turned "sounds wrong" into a mechanism, and it costs a few minutes.
