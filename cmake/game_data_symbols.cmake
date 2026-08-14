# Data the game defines in C, which the player's ROM supplies instead.
#
# A VERIFIED SUBSET, not the full list. tools/gen_data_symbols.py derives the
# candidates -- 927 symbols across 45 files -- and that build is *nearly* right:
# it runs, its audio is byte-identical over a 13,500-frame battle trace, and one
# frame in three differs by 174 pixels, the player drawn on the wrong animation
# frame. Something in the object-event sprite data binds wrong. Until that is
# found, only what has been compared frame for frame ships.
#
# Regenerate the candidates with:
#
#   python3 tools/gen_data_symbols.py \
#       vendor/pokefirered/pokefirered.elf vendor/pokefirered/pokefirered.sym \
#       vendor/pokefirered/src -o /tmp/candidates.cmake \
#       --defined-in build/retail/ports/desktop/frlg-native \
#       --also gSpeciesInfo,gItems
#
# The reference binary must be one with the data still compiled in, or the
# filter drops everything already bound. See issue #11.
set(FRLG_GAME_DATA_SYMBOLS
    "item.c=gItems"
    "pokemon.c=gSpeciesInfo,gBattleMoves,gLevelUpLearnsets,gEvolutionTable"
    "wild_encounter.c=gWildMonHeaders")
