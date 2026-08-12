#!/usr/bin/env python3
"""Golden-screenshot harness.

Captures frames from the port and compares them against goldens with two
independent thresholds ([ADR 0008](../docs/adr/0008-testing-strategy.md)):

  tolerance  the largest per-channel difference absorbed as harmless
  budget     how many pixels may differ beyond that before the frame fails

The split is what makes the tier survivable. A frame that got globally half a
shade darker trips neither; a frame where one sprite moved trips the budget even
though every differing pixel is far outside the tolerance.

Goldens are never committed. They are frames of a copyrighted ROM, and this
project ships no game data ([ADR 0010](../docs/adr/0010-goldens-are-generated.md)),
so each machine blesses its own from a build it trusts.

usage:
  golden.py --binary PATH [--goldens DIR] [--out DIR] [--bless] [--frames LIST]
  golden.py --self-test
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppm_to_png import read_ppm, write_png  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MANIFEST = os.path.join(ROOT, "tests", "golden", "manifest.txt")


class Frame:
    def __init__(self, number, tolerance, budget, reference, name):
        self.number = number
        self.tolerance = tolerance
        self.budget = budget
        # The mGBA frame showing the same moment, or None where our pacing has
        # drifted too far for one to exist.
        self.reference = reference
        self.name = name


def read_manifest(path):
    frames = []
    with open(path) as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            number, tolerance, budget, reference, name = line.split(None, 4)
            frames.append(Frame(int(number), int(tolerance), int(budget),
                                None if reference == "-" else int(reference), name))
    return frames


def compare(golden, actual, tolerance):
    """Pixels differing by more than `tolerance` on any channel, and a mask.

    Returns (count, mask) where mask has one byte per pixel: 255 where the pixel
    is over tolerance, and the scaled channel error where it is under.
    """
    gw, gh, grgb = golden
    aw, ah, argb_ = actual
    if (gw, gh) != (aw, ah):
        raise ValueError(f"size mismatch: golden {gw}x{gh}, actual {aw}x{ah}")

    count = 0
    mask = bytearray(gw * gh)
    for i in range(gw * gh):
        worst = 0
        for c in range(3):
            worst = max(worst, abs(grgb[i * 3 + c] - argb_[i * 3 + c]))
        if worst > tolerance:
            count += 1
            mask[i] = 255
        elif worst:
            mask[i] = min(254, worst * 8)
    return count, bytes(mask)


def write_diff(path, golden, actual, mask):
    """Golden, actual and the difference, side by side."""
    width, height, grgb = golden
    _, _, argb_ = actual
    panel = width * 3
    out = bytearray(panel * height * 3)

    for y in range(height):
        row = y * panel * 3
        src = y * width * 3
        out[row : row + width * 3] = grgb[src : src + width * 3]
        out[row + width * 3 : row + width * 6] = argb_[src : src + width * 3]
        for x in range(width):
            value = mask[y * width + x]
            at = row + width * 6 + x * 3
            # Over tolerance is flagged red; under it fades to grey.
            out[at] = 255 if value == 255 else value
            out[at + 1] = 0 if value == 255 else value
            out[at + 2] = 0 if value == 255 else value

    write_png(path, panel, height, bytes(out), scale=2)


def capture(binary, frame, out_dir):
    path = os.path.join(out_dir, f"frame{frame.number}.ppm")
    # Lockstep, or the comparison is against a moving target: with a wall-clock
    # frame timer a capture on a loaded machine misses a V-blank and lands a
    # frame behind one on an idle machine, and every animated pixel differs.
    # It is also some eight times faster, which is most of this tier's runtime.
    # No window: a capture has no use for one, and opening a real one makes the
    # tier depend on the desktop being in a mood to create it. The audio check
    # has always run this way.
    env = dict(os.environ, FRLG_SHOT=path, FRLG_LOCKSTEP="1")
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    # Stall detection off: the harness has its own timeout and a false positive
    # here would look like a rendering failure.
    result = subprocess.run(
        [binary, str(frame.number), "0"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=600)
    if result.returncode != 0 or not os.path.exists(path):
        tail = result.stderr.decode(errors="replace").strip().splitlines()[-12:]
        raise RuntimeError(
            f"frame {frame.number} ({frame.name}) did not capture; "
            f"exit {result.returncode}\n  " + "\n  ".join(tail))
    return path


def bless_from_reference(args, frames):
    """Bless from mGBA running the reference ROM -- the only source that makes a
    golden mean "matches the ROM" rather than "matches yesterday"."""
    referenced = [f for f in frames if f.reference is not None]
    skipped = [f for f in frames if f.reference is None]

    if not referenced:
        raise SystemExit("no frame in the manifest has a reference frame")

    os.makedirs(args.goldens, exist_ok=True)
    subprocess.run([args.reference_tool, args.rom, args.out]
                   + [str(f.reference) for f in referenced],
                   check=True, stdout=subprocess.DEVNULL, timeout=1800)

    for frame in referenced:
        src = os.path.join(args.out, f"frame{frame.reference}.ppm")
        shutil.copyfile(src, os.path.join(args.goldens, f"frame{frame.number}.ppm"))
        print(f"  blessed frame {frame.number:<5} from mGBA frame "
              f"{frame.reference:<5} {frame.name}")
    for frame in skipped:
        print(f"  SKIPPED frame {frame.number:<5} no reference frame: {frame.name}")
    return 0


def run(args):
    frames = read_manifest(args.manifest)
    if args.frames:
        wanted = {int(n) for n in args.frames.split(",")}
        frames = [f for f in frames if f.number in wanted]
        if not wanted - {f.number for f in frames} == set():
            raise SystemExit(f"no such frame in {args.manifest}")

    os.makedirs(args.out, exist_ok=True)
    os.makedirs(args.goldens, exist_ok=True)
    failures = []

    for frame in frames:
        shot = capture(args.binary, frame, args.out)
        golden_path = os.path.join(args.goldens, f"frame{frame.number}.ppm")

        if args.bless:
            shutil.copyfile(shot, golden_path)
            print(f"  blessed frame {frame.number:<5} {frame.name}")
            continue

        if not os.path.exists(golden_path):
            failures.append(f"frame {frame.number} has no golden; run with --bless")
            print(f"  MISSING frame {frame.number:<5} {frame.name}")
            continue

        golden = read_ppm(golden_path)
        actual = read_ppm(shot)
        count, mask = compare(golden, actual, frame.tolerance)

        if count > frame.budget:
            diff = os.path.join(args.out, f"frame{frame.number}-diff.png")
            write_diff(diff, golden, actual, mask)
            failures.append(
                f"frame {frame.number} ({frame.name}): {count} pixels differ by more "
                f"than {frame.tolerance}, budget {frame.budget}\n    diff: {diff}")
            print(f"  FAIL    frame {frame.number:<5} {frame.name} "
                  f"({count} over budget {frame.budget})")
        else:
            print(f"  ok      frame {frame.number:<5} {frame.name} "
                  f"({count} of {frame.budget} allowed)")

    if failures:
        print("\ngolden: FAILED")
        for f in failures:
            print(f"  {f}")
        return 1
    print(f"\ngolden: {'blessed' if args.bless else 'ok'}, {len(frames)} frames")
    return 0


def self_test():
    """The thresholds decide whether the tier is useful, so prove them."""
    width, height = 4, 1
    base = bytes([100, 100, 100] * (width * height))

    def img(rgb):
        return (width, height, rgb)

    checks = []

    # Identical frames differ nowhere.
    count, _ = compare(img(base), img(base), 0)
    checks.append(("identical frames", count == 0))

    # A shift within tolerance is absorbed on every channel.
    shifted = bytes(min(255, v + 2) for v in base)
    count, _ = compare(img(base), img(shifted), 2)
    checks.append(("shift inside tolerance", count == 0))

    # The same shift one step outside it counts every pixel.
    count, _ = compare(img(base), img(shifted), 1)
    checks.append(("shift outside tolerance", count == width * height))

    # Tolerance is per channel, not a sum: one channel over is enough.
    one = bytearray(base)
    one[1] = 140
    count, _ = compare(img(base), img(bytes(one)), 8)
    checks.append(("single channel over", count == 1))

    # And the worst channel decides, not the first.
    two = bytearray(base)
    two[0] = 101
    two[2] = 200
    count, _ = compare(img(base), img(bytes(two)), 4)
    checks.append(("worst channel decides", count == 1))

    # A size mismatch is an error, not a diff.
    try:
        compare((2, 1, bytes(6)), (3, 1, bytes(9)), 0)
        checks.append(("size mismatch rejected", False))
    except ValueError:
        checks.append(("size mismatch rejected", True))

    bad = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}")
    print(f"\ngolden self-test: {'FAILED' if bad else 'ok'}")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description="golden-screenshot harness")
    ap.add_argument("--binary", help="the port executable to capture from")
    ap.add_argument("--goldens", default=os.path.join(ROOT, "tests", "golden", "images"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "golden"))
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--frames", help="comma-separated subset of frame numbers")
    ap.add_argument("--bless", action="store_true",
                    help="replace the goldens with what the port renders now")
    ap.add_argument("--bless-reference", action="store_true",
                    help="replace the goldens with mGBA's frames of the reference ROM")
    ap.add_argument("--rom", help="reference ROM, for --bless-reference")
    ap.add_argument("--reference-tool", help="the mgba-capture binary")
    ap.add_argument("--self-test", action="store_true",
                    help="check the comparison thresholds and exit")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if args.bless_reference:
        if not (args.rom and args.reference_tool):
            ap.error("--bless-reference needs --rom and --reference-tool")
        os.makedirs(args.out, exist_ok=True)
        return bless_from_reference(args, read_manifest(args.manifest))
    if not args.binary:
        ap.error("--binary is required unless --self-test is given")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
