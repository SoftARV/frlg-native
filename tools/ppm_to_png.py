#!/usr/bin/env python3
"""Convert captured PPM frames to PNG.

FRLG_SHOT writes PPM because it needs no dependencies inside the port. Reviewing
frames and diffing goldens wants PNG, and zlib in the standard library is enough
to write one, so the golden-image harness needs no third-party imaging either.

usage: ppm_to_png.py IN.ppm OUT.png [--scale N]
"""
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{path}: not a binary PPM")

    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while data[pos : pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    width, height, _ = fields
    return width, height, data[pos : pos + width * height * 3]


def write_png(path, width, height, rgb, scale=1):
    raw = bytearray()
    for y in range(height):
        for _ in range(scale):
            raw.append(0)  # filter: none
            row = rgb[y * width * 3 : (y + 1) * width * 3]
            if scale == 1:
                raw += row
            else:
                for x in range(width):
                    raw += row[x * 3 : x * 3 + 3] * scale

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width * scale, height * scale, 8, 2, 0, 0, 0)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", header))
        fh.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        fh.write(chunk(b"IEND", b""))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    scale = 1
    for a in sys.argv[1:]:
        if a.startswith("--scale"):
            scale = int(a.split("=", 1)[1]) if "=" in a else 1
    if len(args) != 2:
        raise SystemExit(__doc__)

    width, height, rgb = read_ppm(args[0])
    write_png(args[1], width, height, rgb, scale)
    print(f"{args[1]}: {width * scale}x{height * scale}")


if __name__ == "__main__":
    main()
