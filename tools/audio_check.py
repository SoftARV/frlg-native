#!/usr/bin/env python3
"""Assert that the mixer actually produces sound.

Audio is the one subsystem whose failure leaves no trace: a mixer that runs and
writes silence looks exactly like a host with nothing plugged in, and every
golden frame still matches. Both were true for a while -- see
docs/spikes/0006-release-build-silence.md.

So the port reports what it mixed on every run, whether or not a device is
listening, and this turns that report into a test. It needs no sound hardware:
the null host refuses to open a device and the measurement happens anyway.
"""

import argparse
import os
import re
import subprocess
import sys

# Over 1,200 frames of the intro the port mixes ~950 non-silent frames and peaks
# around 110. The thresholds are far below that: this is a "did the mixer do
# anything at all" test, not a fidelity one, and it must not fail because a tune
# changed.
MIN_FRAMES = 500
MIN_LOUD = 100
MIN_PEAK = 10

SUMMARY = re.compile(r"audio (\d+) frames, (\d+) non-silent, peak (\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--frames", type=int, default=1200)
    ap.add_argument("--stall", type=int, default=40)
    args = ap.parse_args()

    env = dict(os.environ)
    # Works whether the port was built against SDL or the null host.
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    # The mixer is measured by what it produced, not by when: lockstep makes the
    # run reproducible and drops it from real time to a few seconds.
    env.setdefault("FRLG_LOCKSTEP", "1")
    # Same reason as the golden harness: a measurement is not a play session.
    env.setdefault("FRLG_NO_RECORD", "1")

    run = subprocess.run([args.binary, str(args.frames), str(args.stall)],
                         capture_output=True, text=True, env=env, timeout=600)
    out = run.stdout + run.stderr

    if run.returncode != 0:
        print(out)
        sys.exit(f"audio_check: the port exited {run.returncode}")

    m = SUMMARY.search(out)
    if m is None:
        print(out)
        sys.exit("audio_check: the port printed no audio summary")

    frames, loud, peak = (int(g) for g in m.groups())
    print(f"audio_check: {frames} frames mixed, {loud} non-silent, peak {peak}")

    problems = []
    if frames < MIN_FRAMES:
        problems.append(f"only {frames} frames reached the mixer, wanted {MIN_FRAMES}")
    if loud < MIN_LOUD:
        problems.append(f"only {loud} frames were non-silent, wanted {MIN_LOUD}")
    if peak < MIN_PEAK:
        problems.append(f"peak amplitude was {peak}, wanted {MIN_PEAK}")

    if problems:
        for p in problems:
            print(f"  {p}")
        sys.exit("audio_check: the mixer is not producing sound")

    print("audio_check: ok")


if __name__ == "__main__":
    main()
