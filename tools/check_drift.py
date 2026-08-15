#!/usr/bin/env python3
"""Notice when upstream moves something this port depends on the contents of.

Three kinds of dependency, and they fail very differently:

  **The prelude** redefines sixteen macros it inherits from four of pret's
  headers. It is force-included ahead of every game translation unit and is
  invisible at the call site, which is its whole virtue and its whole risk: if
  pret changes one of those macros, the port keeps compiling and quietly means
  something else. Nothing else catches this. It is the reason this tool exists.

  **Forks** -- files copied into the port and edited -- stop receiving upstream
  fixes the moment they are made. That cost is accepted when the fork is made,
  but it has to stay visible afterwards, and a fork nobody remembers is a fork
  nobody re-checks.

  **Patched files** are already loud: every patch anchors on exact text with an
  expected count and fails the build when it does not match. They are recorded
  here anyway, because "what does this port depend on the contents of" is a
  question worth having one answer to.

Hashes are of the pinned submodule's files. A bump moves them all, which is why
bumping is its own commit: `--bless` re-records, and the diff is then a list of
what upstream changed under us, to read rather than to skim.

usage: check_drift.py [--bless] [--vendor DIR] [--pins FILE]
"""
import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_VENDOR = os.path.join(HERE, "..", "vendor", "pokefirered")
DEFAULT_PINS = os.path.join(HERE, "..", "cmake", "upstream_pins.txt")

# path relative to the vendor tree -> why the port cares
WATCHED = {
    # The prelude includes these to set their guards, then redefines what it
    # needs. A macro changing shape here changes what 320k lines compile to.
    "include/gba/types.h": "prelude redefines its macros",
    "include/gba/defines.h": "prelude redefines its macros",
    "include/gba/io_reg.h": "prelude redefines its macros",
    "include/gba/macro.h": "prelude redefines its macros",
    "include/gba/flash_internal.h": "prelude redefines its macros",
    "include/constants/global.h": "prelude redefines its macros",

    # Patched in the preprocessed copy. These already fail loudly on drift --
    # the patches count their anchors -- so they are here for the inventory
    # rather than for the warning.
    "src/load_save.c": "patched: linker-script assumptions",
    "src/battle_anim_normal.c": "patched: sign-extended pointer halves",
    "src/naming_screen.c": "patched: null tolerance",
    "src/overworld.c": "patched: null tolerance",
    "src/battle_transition.c": "patched: null tolerance",
    "src/sprite.c": "patched: null tolerance",
    "src/trainer_card.c": "patched: null tolerance",
    "src/pokemon_summary_screen.c": "patched: null tolerance",
    "src/pokemon_storage_system_tasks.c": "patched: null tolerance",
    "src/pokemon_storage_system_graphics.c": "patched: null tolerance",
    "src/region_map.c": "patched: null tolerance",
    "src/battle_controllers.c": "patched: null tolerance",
    "src/trade_scene.c": "patched: null tolerance",
    "src/teachy_tv.c": "patched: null tolerance",
    # Patched by a unified diff rather than by anchored insertion, because the
    # scrolling list replaces function bodies (ADR 0023). The diff refuses to
    # apply if this moves, which is loud -- but the hash says so first.
    "src/start_menu.c": "diff-patched: scrolling list and the port's entry",
}


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(65536), b""):
            h.update(block)
    return h.hexdigest()


def read_pins(path):
    pins = {}
    if not os.path.exists(path):
        return pins
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) == 2:
            pins[parts[0]] = parts[1]
    return pins


def write_pins(path, current):
    with open(path, "w") as fh:
        fh.write("# Upstream files this port depends on the contents of, and\n"
                 "# their hashes in the pinned submodule. Written by\n"
                 "# tools/check_drift.py --bless, which is run as part of\n"
                 "# bumping the pin and never on its own.\n"
                 "#\n"
                 "# A line moving here means pret changed something underneath\n"
                 "# us. See docs/ARCHITECTURE.md 4.1 and 4.2.\n")
        for rel in sorted(current):
            fh.write(f"{rel} {current[rel]}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bless", action="store_true",
                    help="re-record the hashes; part of bumping the pin")
    ap.add_argument("--vendor", default=DEFAULT_VENDOR)
    ap.add_argument("--pins", default=DEFAULT_PINS)
    args = ap.parse_args()

    current = {}
    missing = []
    for rel in WATCHED:
        path = os.path.join(args.vendor, rel)
        if not os.path.exists(path):
            missing.append(rel)
            continue
        current[rel] = digest(path)

    if missing:
        print("upstream files this port watches are not there:", file=sys.stderr)
        for rel in missing:
            print(f"  {rel} -- {WATCHED[rel]}", file=sys.stderr)
        print("\nThe submodule is not checked out, or the file was renamed "
              "upstream.", file=sys.stderr)
        return 1

    if args.bless:
        write_pins(args.pins, current)
        print(f"recorded {len(current)} upstream hashes")
        return 0

    pins = read_pins(args.pins)
    if not pins:
        print(f"no pins recorded yet: run\n"
              f"  python3 tools/check_drift.py --bless", file=sys.stderr)
        return 1

    moved = [rel for rel in current if rel in pins and pins[rel] != current[rel]]
    added = [rel for rel in current if rel not in pins]
    gone = [rel for rel in pins if rel not in current]

    if not moved and not added and not gone:
        print(f"{len(current)} upstream files unchanged since the pin was recorded")
        return 0

    if moved:
        print("upstream changed under this port:\n", file=sys.stderr)
        for rel in sorted(moved):
            print(f"  {rel}\n      {WATCHED[rel]}", file=sys.stderr)
        print("\nRead what changed before blessing it. A prelude macro that\n"
              "moved changes what the whole game compiles to, and nothing else\n"
              "will tell you:\n"
              "  git -C vendor/pokefirered log -p -- <file>", file=sys.stderr)
    for rel in sorted(added):
        print(f"  watched but never recorded: {rel}", file=sys.stderr)
    for rel in sorted(gone):
        print(f"  recorded but no longer watched: {rel}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
