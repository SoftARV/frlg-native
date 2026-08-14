# Data the game defines in C, which the player's ROM supplies instead.
#
# A VERIFIED SUBSET. tools/gen_data_symbols.py derives the full candidate list
# from the ROM build's symbol table and the decompilation's sources -- 922
# symbols across 44 files -- and applying all of it builds cleanly and then
# crashes at frame 807 on a null pointer. One of them must not be bound, and
# which one is not yet known.
#
# What is here is what has been played through and compared frame for frame and
# sample for sample. Regenerate the candidates with:
#
#   python3 tools/gen_data_symbols.py \
#       vendor/pokefirered/pokefirered.elf vendor/pokefirered/pokefirered.sym \
#       vendor/pokefirered/src -o /tmp/candidates.cmake \
#       --defined-in build/norom/ports/desktop/frlg-native
#
# See https://github.com/SoftARV/frlg-native/issues/11.
set(FRLG_GAME_DATA_SYMBOLS
    "item.c=gItems"
    "pokemon.c=gSpeciesInfo,gBattleMoves,gLevelUpLearnsets,gEvolutionTable"
    "wild_encounter.c=gWildMonHeaders")
