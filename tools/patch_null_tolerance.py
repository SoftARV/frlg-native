#!/usr/bin/env python3
"""Repair reads that only a machine without memory protection tolerates.

The Game Boy Advance has no MMU. Every address in its map is readable, address
zero included -- that region is BIOS ROM -- so a read through a null pointer
returns garbage rather than killing the program. Upstream code is entitled to
that, and in at least one place it relies on it.

`naming_screen.c` frees its own state inside `RunTasks()`, and `CB2_NamingScreen`
then calls `AnimateSprites()` in the same frame. The cursor's callback reads
`sNamingScreen->currentPage` through the pointer that was just set to null. On
hardware that reads BIOS ROM at offset 0x1e22, the cursor jumps once, the main
callback changes and it never runs again -- invisible. Here it is a segfault, and
it is what stopped a player naming their character.

Freeing without clearing the pointer leaves the read hitting freed heap instead
of address zero: still garbage, still read exactly once, and still overwritten by
the next allocation, but mapped. Nothing tests the pointer for null except the
check immediately after the allocation that sets it, so nothing depends on it
being cleared.

The alternative -- mapping a readable page at address zero so the whole class
behaves as it does on hardware -- is not available: `vm.mmap_min_addr` forbids it
without privileges we should not want.

Matched exactly and required to be found, so a submodule bump is reported rather
than quietly restoring the crash. See docs/ARCHITECTURE.md 4.3.
"""

import argparse
import os
import re
import sys

# Matched after preprocessing, where FREE_AND_SET_NULL has expanded into a block
# whose halves are separated by line directives -- hence a pattern rather than a
# string. The free stays; only the assignment goes.
EDITS = {
    # The save blocks' pointers start null and are not set until the title
    # screen, but LoadGameSave runs at boot -- so the sector copy writes through
    # null plus the sector's offset. On hardware those writes land on the BIOS
    # region and are ignored; the real load happens later and works. Here they
    # fault, and the game cannot boot with a save present at all.
    #
    # Pointing them at their own objects makes that early copy land where it
    # belongs instead of nowhere. Offset zero is one of the offsets
    # SetSaveBlocksPointers itself picks, so it is a state the game already
    # handles.
    "load_save.c": [
        (
            "the save block pointers' null start",
            re.compile(
                r"struct SaveBlock1 \*gSaveBlock1Ptr =\s*(?:#[^\n]*\n\s*)*\(\(void \*\)0\)"
            ),
            "struct SaveBlock1 *gSaveBlock1Ptr = &gSaveBlock1",
        ),
        (
            "the second save block pointer's null start",
            re.compile(
                r"struct SaveBlock2 \*gSaveBlock2Ptr =\s*(?:#[^\n]*\n\s*)*\(\(void \*\)0\)"
            ),
            "struct SaveBlock2 *gSaveBlock2Ptr = &gSaveBlock2",
        ),
        (
            "the storage pointer's null start",
            re.compile(
                r"struct PokemonStorage \*gPokemonStoragePtr =\s*(?:#[^\n]*\n\s*)*\(\(void \*\)0\)"
            ),
            "struct PokemonStorage *gPokemonStoragePtr = &gPokemonStorage",
        ),
    ],
    # A map with no object events has a null objectEvents pointer, and the loop
    # copying their scripts ignores the count and reads sixty-four entries
    # regardless. On hardware that reads the BIOS region and copies garbage into
    # templates nothing goes on to use, because the count is zero. Here it faults,
    # and continuing a saved game cannot get past it.
    #
    # Guarding the loop leaves those templates holding whatever they held instead
    # of holding garbage -- both unused, and neither read.
    "overworld.c": [
        (
            "the object event script copy from a map with none",
            re.compile(
                r"(?P<keep>const struct ObjectEventTemplate \* src = "
                r"gMapHeader\.events->objectEvents;\n"
                r"    struct ObjectEventTemplate \* savObjTemplates = "
                r"gSaveBlock1Ptr->objectEventTemplates;\n\n)"
                r"    for \(i = 0; i < 64; i\+\+\)"
            ),
            r"\g<keep>    for (i = 0; src != ((void *)0) && i < 64; i++)",
        ),
    ],
    # IsBattleTransitionDone frees the transition's data and destroys its task,
    # but leaves the transition's own V-blank callback installed. The next
    # V-blank dereferences the pointer it just cleared -- once, before the battle
    # sets its own callback. On hardware that reads BIOS ROM and writes a frame of
    # garbage into WININ, WINOUT and WIN0V, behind a screen that is already black;
    # here it is a segfault at the start of every battle.
    #
    # Freeing without clearing leaves the read on freed heap: mapped, garbage, and
    # read at most a frame or two. Nothing tests the pointer for null -- it is
    # assigned exactly twice, at its definition and at its allocation.
    # A screen that caches a sprite and re-creates it destroys the old one first,
    # and on the first visit there is no old one. The summary screen's marking
    # sprite is the instance that turned up -- PokeSum_CreateMonMarkingsSprite
    # destroys unconditionally, then null-checks what it creates -- but the shape
    # belongs to the wrapper rather than to that screen, and 22 places call it.
    #
    # On hardware every step of it is a read or write through address zero plus a
    # small offset: BIOS ROM, which reads as whatever the BIOS last prefetched and
    # ignores writes. The tag that comes back frees nothing, and DestroySprite's
    # write goes nowhere. Refusing a null sprite is the same nothing, done on
    # purpose.
    # The trainer card's V-blank callback outlives the card: CloseTrainerCard
    # frees the data and the callback runs on until the next screen installs its
    # own. Opening the card once and closing it is enough.
    #
    # Only the two dereferences are guarded, not the callback: the three calls
    # before them -- OAM, the sprite copy queue, the palette buffer -- run on
    # hardware in those frames too, and the next screen is relying on them. What
    # the hardware does with the rest is write a counter into the BIOS region,
    # where writes are ignored, and read a flag back as whatever was prefetched.
    #
    # The pointer cannot be left dangling the way the naming screen's is:
    # GetCardType tests it for null and answers differently.
    # The summary screen's V-blank callback outlives the screen the same way the
    # trainer card's does -- CB2 frees the data, the callback runs on until the
    # next screen installs its own, and it reads a flag through the pointer.
    #
    # Here the pointer can simply be left where it was: nothing tests it for null
    # except the check immediately after the allocation that sets it, so a read
    # lands on freed heap -- mapped, garbage, read once or twice -- rather than on
    # address zero. That covers every dereference in the file's callbacks rather
    # than the one that happened to be reached.
    "pokemon_summary_screen.c": [
        (
            "the summary screen's read through its freed state",
            re.compile(
                r"\{ if \(sMonSummaryScreen !=\s*"
                r"(?:#[^\n]*\n\s*)*\(\(void \*\)0\)\s*"
                r"(?:#[^\n]*\n\s*)*\) \{ Free\(sMonSummaryScreen\); \(sMonSummaryScreen\) =\s*"
                r"(?:#[^\n]*\n\s*)*\(\(void \*\)0\)\s*"
                r"(?:#[^\n]*\n\s*)*; \} \}"
            ),
            "{ if (sMonSummaryScreen != ((void *)0)) { Free(sMonSummaryScreen); } }",
        ),
    ],
    "trainer_card.c": [
        (
            "the time colon's blink through freed state",
            re.compile(
                r"(?P<keep>static void BlinkTimeColon\(void\)\n\{\n)"
                r"    if \(\+\+sTrainerCardDataPtr->timeColonBlinkTimer > 60\)"
            ),
            r"\g<keep>    if (sTrainerCardDataPtr == ((void *)0))\n        return;\n\n"
            r"    if (++sTrainerCardDataPtr->timeColonBlinkTimer > 60)",
        ),
        (
            "the scanline copy's flag read through freed state",
            re.compile(r"    if \(sTrainerCardDataPtr->allowDMACopy\)"),
            "    if (sTrainerCardDataPtr != ((void *)0) && sTrainerCardDataPtr->allowDMACopy)",
        ),
    ],
    "sprite.c": [
        (
            "the destroy-and-free wrapper's null sprite",
            re.compile(
                r"(?P<keep>void DestroySpriteAndFreeResources\(struct Sprite \*sprite\)\n\{\n)"
                r"    FreeSpriteTiles\(sprite\);"
            ),
            r"\g<keep>    if (sprite == ((void *)0))\n        return;\n\n    FreeSpriteTiles(sprite);",
        ),
    ],
    "battle_transition.c": [
        (
            "the battle transition's read through its freed state",
            re.compile(
                r"\{ Free\(sTransitionData\); sTransitionData =\s*"
                r"(?:#[^\n]*\n\s*)*\(\(void \*\)0\)\s*"
                r"(?:#[^\n]*\n\s*)*; \}"
            ),
            "{ Free(sTransitionData); }",
        ),
    ],
    "naming_screen.c": [
        (
            "the naming screen's read through its freed state",
            re.compile(
                r"\{ Free\(sNamingScreen\); sNamingScreen =\s*"
                r"(?:#[^\n]*\n\s*)*\(\(void \*\)0\)\s*"
                r"(?:#[^\n]*\n\s*)*; \}"
            ),
            "{ Free(sNamingScreen); }",
        ),
    ],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    args = ap.parse_args()

    edits = EDITS.get(os.path.basename(args.file))
    if edits is None:
        sys.exit(f"patch_null_tolerance: nothing known about {args.file}")

    text = open(args.file).read()

    for what, pattern, new in edits:
        found = len(pattern.findall(text))
        if found != 1:
            sys.exit(
                f"patch_null_tolerance: expected exactly one occurrence of {what} "
                f"in {args.file}, found {found}. Upstream has changed it; see "
                "docs/ARCHITECTURE.md 4.3."
            )
        text = pattern.sub(new, text, count=1)

    open(args.file, "w").write(text)


if __name__ == "__main__":
    main()
