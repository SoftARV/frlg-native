"""Where the ROM build put things, read from the linked ELF.

Shared because two generators need the same answer to the same question -- is
this address code or data -- and they must not be able to disagree about it.
They did once: each carried its own rule, "below where .text ends", and that is
not a test for code, it is a test for the modern layout.

The byte-matching build puts the library routines in their own `lib_text`
section which sits *above* `script_data`, so under that rule every symbol in it
was filed as ROM data and bound into the cart image. The game then called one --
`rfu_setTimerInterrupt`, from `InitRFU`, before the first frame -- and jumped
into its own data. The section flags say which sections hold code, on any
layout, which is what both scripts ask for now.
"""

import re
import subprocess


def read_sections(elf, readelf="arm-none-eabi-readelf"):
    """Section name -> (address, size, flags)."""
    out = subprocess.run([readelf, "-SW", elf], capture_output=True, text=True,
                         check=True).stdout
    sections = {}
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]{8})\s+[0-9a-f]+\s+([0-9a-f]+)"
                     r"\s+[0-9a-f]+\s+([A-Zx]*)", line)
        if m:
            sections[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16), m.group(4))
    return sections


def code_ranges(sections):
    """Every executable range, low to high. Code is not one contiguous span."""
    return sorted((addr, addr + size) for addr, size, flags in sections.values()
                  if "X" in flags and size > 0)


def is_code(ranges, addr):
    return any(lo <= addr < hi for lo, hi in ranges)


def data_relocation_sections(sections):
    """The `.rel*` sections describing pointers held in data, on any layout.

    Hardcoding this list is the same mistake as hardcoding where code ends. The
    matching build keeps the music in its own `song_data` section with its own
    `.relsong_data`, and a fixed list written against the modern layout skips it
    -- every pointer the sequencer follows stays a cartridge address, so the
    picture is perfect and the sound is gone.

    A relocation section describes data if the section it applies to is loaded
    and not executable.
    """
    out = set()
    for name in sections:
        if not name.startswith(".rel"):
            continue
        target = sections.get(name[len(".rel"):])
        if target is None:
            continue
        _, size, flags = target
        if "A" in flags and "X" not in flags and size > 0:
            out.add(name)
    return out
