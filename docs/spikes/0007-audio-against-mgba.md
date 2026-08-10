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

**Not established:** whether our direct-sound treble is wrong at all. The GBA's own PCM is 8-bit at
about 13 kHz and its DAC produces exactly this kind of content; mGBA's `blip_buf` resampling
deliberately removes it. Ours may be the more faithful of the two. Settling that needs a comparison
against hardware, not against another emulator.
