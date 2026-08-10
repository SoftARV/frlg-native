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
