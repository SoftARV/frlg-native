# 0018 — Match the cartridge's structure layout for types the cart supplies

**Status:** accepted
**Date:** 2026-08-14

## Context

The cartridge was compiled by `agbcc`, which rounds every structure's size up to a multiple of four.
No modern compiler does this — not on x86, and not on ARM either; `arm-none-eabi-gcc` agrees with our
x86 build, so this is agbcc's rule rather than the target's.

A type whose natural size is not already a multiple of four therefore has two layouts: a narrow one in
this build, and a wider one in the ROM. While all game data was compiled in, only our layout existed
and nothing was wrong. [ADR 0006](0006-the-binary-ships-no-game-data.md) changed that: data now comes
out of the player's cartridge, laid out the way agbcc laid it out, and is read by code that believes
the narrow layout. Every table of such a type is misparsed from its **second** element onward.

The first element is always right, which is why this survived every check we had. The extracted bytes
are identical to the ROM's, the relocations are correct, the decompressor is correct, and the entropy
probe reports the data is gone from the binary. All of them measure the data. None measures the stride
the reader uses.

`union AffineAnimCmd` is six bytes here and eight in the cart. One mismatched type produced: a wild
battle that loops the send-out animation forever, blank battler and trainer sprites, corrupt Pokémon
in the intro, and a `SIGFPE` in `UpdateSpriteMatrixAnchorPos` dividing by a matrix that was never
written. `struct MonCoords` is two against four, which moves every battle sprite. Forty-five types
have the property.

## Decision

**Types whose data the cart supplies are given the cartridge's layout**, by carrying
`__attribute__((aligned(4)))` in the preprocessed copy — `tools/patch_struct_layout.py`, applied to
every game source, with a `_Static_assert` on the resulting size so a regression is a compile error
rather than a hung battle.

**Types that live only in RAM keep this build's layout.** Their layout is nobody's business but ours,
and widening them would change the save format for no benefit.

The list of widened types is explicit, not a rule applied wholesale. Each entry is justified against
the ROM's own symbol sizes: `gMonFrontPicCoords` is 0x6e0 over 440 entries, four bytes each, where
this build would give two.

## Consequences

Our layout for these types now differs from what our compiler would choose, which is the cost. It is
paid where it buys something: agreement with the cartridge.

Compiled-in builds are unaffected in behaviour — they were self-consistent before and are
self-consistent now, verified by the reference frames being unchanged after the widening.

**Adding a type to the extraction list now requires consulting `tools/audit_layout.py`.** A type on
that list whose data starts coming from the cart, without being widened, reproduces this class exactly
— and reproduces it quietly, since the first element will look right.

The alternatives were considered and rejected. Repacking the data at import time keeps our ABI clean
but requires the importer to know every affected type and its extent, and stops the cart image being a
byte copy of the ROM. Excluding affine tables from extraction fixes the symptom while leaving game
data in the shipped binary, which is what ADR 0006 exists to remove.

## What this says about verification

Both mismatches were found by bisecting the extraction list against a recorded play-through
([§5.3d](../ARCHITECTURE.md#53d-recorded-play-throughs)), after five separate inspections of the same
code had each concluded it was correct. The inspections were not sloppy; they were checking the wrong
thing. A byte-level measurement cannot see a stride, and a scripted trace that never renders a battle
sprite cannot see a battle sprite.

Recorded play-throughs are therefore part of the verification set, not a debugging convenience.
