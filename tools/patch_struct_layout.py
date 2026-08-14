#!/usr/bin/env python3
"""Give union AffineAnimCmd the width the cartridge gives it.

agbcc rounds every structure size up to a multiple of four, so a cartridge's
affine animation commands sit eight bytes apart. Our compilers make the union
six -- correctly, by their own ABI -- and reading a cartridge table with a
six-byte stride misparses every command after the first. The animation then
never reaches its END marker: it never completes, the matrix is never written,
and a sprite keeps whatever pixels were already in its buffer. union AnimCmd
needs nothing, being four bytes in both worlds already.

The definition is rewritten in the preprocessed copy because vendor's header is
pinned and never edited. A file that names the type but no longer matches the
shape below is an error rather than a silent revert to the narrow layout, so a
submodule bump is reported.
"""
import re
import sys

# Types whose data this port reads out of the cart, each confirmed against the
# ROM's own symbol size divided by the entry count in the decompilation's
# source. Membership is never assumed from the type's shape: struct LevelUpMove
# is two bytes here and two in the cart, so widening it would break the
# learnsets it is meant to protect. Verify, then add.
#
#   union  AffineAnimCmd  sAffineAnim_Scene3_Mons_Normal  0x10 / 2   = 8
#   struct MonCoords      gMonFrontPicCoords              0x6e0 / 440 = 4
#   struct BattleMove     gBattleMoves                    4260 / 355 = 12
#   struct SpeciesInfo    gSpeciesInfo                    11536 / 412 = 28
#   struct Evolution      gEvolutionTable   16480 / (412 * EVOS_PER_MON) = 8
#   struct TrainerMoney   gTrainerMoneyTable              420 / 105  = 4
#
# Types that only ever live in RAM are deliberately absent -- their layout is
# this build's business, and widening them would change the save format.
#   struct BattleWindowText  sTextOnWindowsInfo_Normal   300 / 25 entries = 12
#   struct WinCoords         sNameWindowCoords_5Players   20 / 5 entries  = 4
#   struct ListMenuWindowRect sListMenuRects_OrderedList  40 / 5 entries  = 8
#   struct CreditsOverworldCmd sOverworldCmd_PewterCity    32 / 8 = 4, /6 not whole
#   struct BattleTowerPokemonTemplate gBattleTowerLevel50Mons
#                                                       4800 / 16 = 300, /14 not whole
#
# The last five are only reachable once the statics are extracted as well; they
# were found by tools/audit_layout.py against that list rather than by a crash,
# except BattleWindowText, which crashed first and prompted the audit.
TYPES = (
    ("union", "AffineAnimCmd", 8),
    ("struct", "MonCoords", 4),
    ("struct", "BattleMove", 12),
    ("struct", "SpeciesInfo", 28),
    ("struct", "Evolution", 8),
    ("struct", "TrainerMoney", 4),
    ("struct", "BattleWindowText", 12),
    ("struct", "WinCoords", 4),
    ("struct", "ListMenuWindowRect", 8),
    ("struct", "CreditsOverworldCmd", 8),
    ("struct", "BattleTowerPokemonTemplate", 16),
    ("struct", "TrainerMonNoItemDefaultMoves", 8),
    ("struct", "TrainerMonNoItemCustomMoves", 16),

    # Inside the save blocks. These do not come from the cart at all -- they are
    # widened so that a save this port writes has the same layout as one a
    # cartridge writes, which is what makes a real save importable and ours
    # readable elsewhere. The check is not per-type here but the total: the save
    # blocks must come out at exactly the sizes the reference build gives them,
    # 15720 and 3876. See ADR 0019.
    #
    # ExternalEventFlags is deliberately absent: upstream already marks it
    # __attribute__((packed)), which agbcc honours, so it is 21 bytes in both
    # worlds and widening it would push everything after it out of place.
    ("struct", "Time", 8),
    ("struct", "Mail", 36),
    ("struct", "DayCareMail", 56),
    ("struct", "QuestLogObjectEvent", 20),
    ("struct", "LinkBattleRecords", 88),
    ("struct", "RamScriptData", 1000),
    ("struct", "FameCheckerSaveData", 4),
    ("struct", "WonderNewsMetadata", 4),
)

# The trainer party structs come from data.c, which is cut whole rather than by
# symbol, so they are reached through FRLG_GAME_DATA_ONLY and not through the
# symbol list. Confirmed the same way as the rest:
#   sParty_YoungsterJosh   24 / 3 mons = 8   (ours 6)
#   sParty_CamperLiam      32 / 2 mons = 16  (ours 14)
# A wrong stride here hands a battle the wrong species at the wrong level --
# reported from play as a level 0 Charizard on a Viridian Forest bug catcher.

ALIGNED = "__attribute__((aligned(4)))"

# Not widened -- asserted. A save this port writes has to be a save a cartridge
# could have written, and these two totals are what says so. Widening the wrong
# set of members reaches neither number, which makes this a better check than any
# per-type reasoning. See ADR 0019.
TOTALS = (
    ("struct", "SaveBlock1", 15720),
    ("struct", "SaveBlock2", 3876),
)


def widen(text, kw, name, width, path):
    """Give one type the cartridge's width, and lock it there."""
    definition = re.compile(rf"{kw}\s+{name}\s*\{{(?P<body>[^{{}}]*?)\}}\s*;", re.S)
    # A marker of our own, not a `sizeof` the game might legitimately write:
    # game code does use sizeof on several of these, and treating that as "already
    # widened" left some translation units narrow and others wide. Two definitions
    # of SaveBlock1 in one binary is worse than the layout being wrong everywhere.
    marker = f"/* frlg-widened {kw} {name} */"

    if f"{kw} {name}" not in text:
        return text, False
    if marker in text:
        return text, False   # already widened; the stage is idempotent

    patched, count = definition.subn(
        lambda m: "%s %s {%s} %s;  %s\n_Static_assert(sizeof(%s %s) == %d,\n"
                  "    \"%s must match the cartridge's %d-byte layout\");"
                  % (kw, name, m.group("body"), ALIGNED, marker, kw, name, width,
                     name, width),
        text, count=1)
    if count == 0:
        sys.exit(f"patch_struct_layout: {path} names {kw} {name} but its definition "
                 "does not match the expected shape; the pin moved and the layout "
                 "fix must be rechecked")
    return patched, True


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: patch_struct_layout.py file.c")
    path = sys.argv[1]
    with open(path, encoding="utf-8", errors="surrogateescape") as fh:
        text = fh.read()

    changed = False
    for kw, name, width in TYPES:
        text, did = widen(text, kw, name, width, path)
        changed = changed or did

    # The save blocks keep their own definitions; only their totals are pinned.
    for kw, name, total in TOTALS:
        marker = f"/* frlg-total {kw} {name} */"
        if f"{kw} {name}\n{{" not in text and f"{kw} {name} {{" not in text:
            continue
        if marker in text:
            continue
        definition = re.compile(rf"({kw}\s+{name}\s*\{{[^{{}}]*?(?:\{{[^{{}}]*\}}[^{{}}]*?)*\}}\s*;)",
                                re.S)
        patched, count = definition.subn(
            lambda m: "%s  %s\n_Static_assert(sizeof(%s %s) == %d,\n"
                      "    \"%s must match the layout a cartridge writes\");"
                      % (m.group(1), marker, kw, name, total, name),
            text, count=1)
        if count:
            text, changed = patched, True

    if changed:
        with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
            fh.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
