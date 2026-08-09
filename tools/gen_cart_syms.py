#!/usr/bin/env python3
"""Bind ROM-resident data symbols into the cart region at link time.

The game's scripts and map data live in data/*.s, which no host assembler can
build (see docs/spikes/0002-host-assembly.md). Those symbols therefore come from
the ROM image: the linker is told each one is agb_cart + its ROM offset, so game
code reaches its data through the ordinary symbol it always used.

Emits a response file of --defsym arguments for ld.
"""
import argparse
import sys

CART_BASE = 0x08000000


def read_syms(path):
    """pokefirered.sym rows are: addr binding size name"""
    out = {}
    with open(path) as fh:
        for line in fh:
            parts = line.split()
            if len(parts) != 4:
                continue
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            out.setdefault(parts[3], addr)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("symfile", help="the ROM build's .sym")
    ap.add_argument("wanted", help="newline-separated symbols to bind")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--start", required=True, help="section start, hex")
    ap.add_argument("--end", required=True, help="section end, hex")
    ap.add_argument("--array", default="agb_cart")
    # The compiler driver expands @response files itself and does not know
    # --defsym, so anything fed through it has to be wrapped for the linker.
    ap.add_argument("--driver", action="store_true",
                    help="emit -Wl,--defsym,... for a compiler driver")
    args = ap.parse_args()

    start, end = int(args.start, 16), int(args.end, 16)
    syms = read_syms(args.symfile)

    with open(args.wanted) as fh:
        wanted = [w.strip() for w in fh if w.strip()]

    bound, skipped = [], []
    for name in wanted:
        addr = syms.get(name)
        if addr is None or not (start <= addr < end):
            skipped.append(name)
            continue
        bound.append((name, addr - CART_BASE))

    with open(args.output, "w") as fh:
        for name, off in bound:
            if args.driver:
                fh.write(f"-Wl,--defsym,{name}={args.array}+0x{off:X}\n")
            else:
                fh.write(f"--defsym {name}={args.array}+0x{off:X}\n")

    print(f"bound {len(bound)} symbols into {args.array}", file=sys.stderr)
    if skipped:
        print(f"unbound (not in range): {len(skipped)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
