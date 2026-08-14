#!/usr/bin/env python3
"""Rewrite a save written in this port's old layout into the cartridge's.

Before ADR 0019 the port laid its save blocks out the way our own compiler
chose, which is not the way agbcc laid out the cartridge's: several structures
inside SaveBlock1 and SaveBlock2 are a byte or two narrower here, so every field
after the first of them sat at a different offset. Saves written then cannot be
read now, and a save written by a real cartridge could never be read at all.

The field mapping is derived, not written down: two binaries -- one built before
the widening and one after -- are read with pahole, their SaveBlock members are
matched by name, and wherever a member changed size the walk recurses into it
until it reaches something whose size agrees. The result is a list of copies,
which is applied to the save's sector data, and the sector checksums and sizes
are recomputed for the new layout.

usage: migrate_save.py OLD_BINARY NEW_BINARY SAVE_IN SAVE_OUT
"""
import re
import struct
import subprocess
import sys

SECTOR_DATA_SIZE = 3968
SECTOR_SIZE = 4096
NUM_SECTORS_PER_SLOT = 14
SECTORS_COUNT = 32
SECTOR_SIGNATURE = 0x08012025
FOOTER_ID = SECTOR_DATA_SIZE + (128 - 12)

SB2, SB1_START, SB1_END = 0, 1, 4

# widened members carry an alignment attribute before the semicolon
MEMBER = re.compile(
    r"^\s*(?:const\s+)?(?:(struct|union)\s+(\w+)|[\w\s]+?)\s+(\**)(\w+)((?:\[\d+\])*)"
    r"\s*(?:__attribute__\(\(.*?\)\)\s*)?;"
    r"\s*/\*\s*(\d+)\s+(\d+)\s*\*/")

# Bitfields print their position as `byte: bit` rather than a plain offset, so
# they do not match the pattern above. Skipping them silently drops real data --
# SaveBlock2 keeps the player's options in six of them. Each is copied as its
# whole storage unit; overlapping copies of the same bytes are harmless.
BITFIELD = re.compile(
    r"^\s*[\w\s]+?\s+(\w+):\d+;\s*/\*\s*(\d+):\s*\d+\s+(\d+)\s*\*/")


def members(binary, tname):
    """Members of a struct: name -> (kind, type, offset, size, count)."""
    out = subprocess.run(["pahole", "-C", tname, binary],
                         capture_output=True, text=True).stdout
    found, depth = {}, 0
    for line in out.splitlines():
        if line.startswith("struct") or line.startswith("union"):
            depth += line.count("{")
            continue
        opens, closes = line.count("{"), line.count("}")
        if depth == 1:
            m = MEMBER.match(line)
            if m:
                kind, tn, ptr, name, dims, off, size = m.groups()
                if not ptr:
                    # dims may be multi-dimensional: registeredTexts[10][21] is
                    # 210 bytes of real save data and was silently skipped when
                    # only a single dimension was matched.
                    count = 1
                    for d in re.findall(r"\[(\d+)\]", dims or ""):
                        count *= int(d)
                    found[name] = (kind, tn, int(off), int(size), count)
            else:
                b = BITFIELD.match(line)
                if b:
                    name, off, size = b.groups()
                    found[name] = (None, None, int(off), int(size), 1)
        depth += opens - closes
    return found


def only_padding_changed(old_bin, new_bin, tname):
    """True when a type gained trailing padding and nothing moved inside it.

    Widening appends padding; every member keeps its offset. Such a type is
    copied whole rather than member by member, which is both simpler and safer:
    walking members means parsing them, and bitfields do not print like other
    members. Missing the 21 bitfields at the front of QuestLogObjectEvent lost
    `active` and `invisible` for every object in the quest log, which showed up
    as missing sprites in the recap the game plays when a save is loaded.
    """
    mo, mn = members(old_bin, tname), members(new_bin, tname)
    if set(mo) != set(mn):
        return False
    return all(mo[k][2] == mn[k][2] and mo[k][3] == mn[k][3] for k in mo)


def plan(old_bin, new_bin, tname, obase, nbase, ops, depth=0):
    if depth > 8:
        sys.exit(f"migrate_save: recursion too deep at {tname}")
    mo, mn = members(old_bin, tname), members(new_bin, tname)
    for name, (kind, tn, ooff, osize, ocount) in mo.items():
        if name not in mn:
            sys.exit(f"migrate_save: {tname}.{name} is missing from the new build")
        _, ntn, noff, nsize, ncount = mn[name]
        if osize == nsize:
            ops.append((obase + ooff, nbase + noff, osize))
            continue
        if not tn:
            sys.exit(f"migrate_save: {tname}.{name} changed size but is not a struct")
        oelem, nelem = osize // ocount, nsize // ncount
        whole = only_padding_changed(old_bin, new_bin, tn)
        for i in range(ocount):
            if whole:
                ops.append((obase + ooff + i * oelem, nbase + noff + i * nelem, oelem))
            else:
                plan(old_bin, new_bin, tn, obase + ooff + i * oelem,
                     nbase + noff + i * nelem, ops, depth + 1)


def checksum(data, size):
    total = 0
    for i in range(size // 4):
        total = (total + struct.unpack_from("<I", data, i * 4)[0]) & 0xFFFFFFFF
    return ((total >> 16) + total) & 0xFFFF


def sizes_of(binary):
    out = subprocess.run(["pahole", "--sizes", binary], capture_output=True, text=True).stdout
    got = {}
    for line in out.splitlines():
        p = line.split("\t")
        if len(p) >= 2:
            try:
                got.setdefault(p[0], int(p[1]))
            except ValueError:
                pass
    return got


def chunk(total, n):
    """The offset and length SAVEBLOCK_CHUNK gives chunk n of a structure."""
    off = n * SECTOR_DATA_SIZE
    return off, max(0, min(total - off, SECTOR_DATA_SIZE)) if total >= off else 0


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__.strip().splitlines()[-1])
    old_bin, new_bin, src, dst = sys.argv[1:]

    osz, nsz = sizes_of(old_bin), sizes_of(new_bin)
    for t in ("SaveBlock1", "SaveBlock2"):
        if t not in osz or t not in nsz:
            sys.exit(f"migrate_save: {t} not found in both binaries")
    if osz["SaveBlock1"] == nsz["SaveBlock1"] and osz["SaveBlock2"] == nsz["SaveBlock2"]:
        sys.exit("migrate_save: the two binaries have the same layout; nothing to do")
    print(f"SaveBlock1 {osz['SaveBlock1']} -> {nsz['SaveBlock1']}")
    print(f"SaveBlock2 {osz['SaveBlock2']} -> {nsz['SaveBlock2']}")

    plans = {}
    for t in ("SaveBlock1", "SaveBlock2"):
        ops = []
        plan(old_bin, new_bin, t, 0, 0, ops)
        covered = sum(n for _, _, n in ops)
        print(f"  {t}: {len(ops)} copies covering {covered} of {osz[t]} old bytes")
        if covered != osz[t]:
            print(f"    note: {osz[t] - covered} bytes are padding or holes and are dropped")
        plans[t] = ops

    data = bytearray(open(src, "rb").read())
    if len(data) != SECTORS_COUNT * SECTOR_SIZE:
        sys.exit(f"migrate_save: {src} is {len(data)} bytes, expected {SECTORS_COUNT * SECTOR_SIZE}")

    moved = 0
    for slot in range(2):
        base = slot * NUM_SECTORS_PER_SLOT * SECTOR_SIZE
        # Gather this slot's sectors by the id in their footer.
        by_id = {}
        for s in range(NUM_SECTORS_PER_SLOT):
            at = base + s * SECTOR_SIZE
            sid, _, sig = struct.unpack_from("<HHI", data, at + FOOTER_ID)
            if sig == SECTOR_SIGNATURE:
                by_id[sid] = at
        if SB2 not in by_id:
            continue

        for tname, first, last in (("SaveBlock2", SB2, SB2),
                                   ("SaveBlock1", SB1_START, SB1_END)):
            if not all(i in by_id for i in range(first, last + 1)):
                continue
            old_total, new_total = osz[tname], nsz[tname]
            # Reassemble, transform, and write back chunk by chunk.
            blob = bytearray(old_total)
            for i in range(first, last + 1):
                off, size = chunk(old_total, i - first)
                if size:
                    blob[off:off + size] = data[by_id[i]:by_id[i] + size]
            fresh = bytearray(new_total)
            for src_off, dst_off, n in plans[tname]:
                fresh[dst_off:dst_off + n] = blob[src_off:src_off + n]
            for i in range(first, last + 1):
                off, size = chunk(new_total, i - first)
                at = by_id[i]
                data[at:at + SECTOR_DATA_SIZE] = bytes(SECTOR_DATA_SIZE)
                if size:
                    data[at:at + size] = fresh[off:off + size]
                struct.pack_into("<H", data, at + FOOTER_ID + 2,
                                 checksum(data[at:at + size], size))
            moved += 1

    open(dst, "wb").write(bytes(data))
    print(f"\nrewrote {moved} save block(s) across both slots -> {dst}")


if __name__ == "__main__":
    main()
