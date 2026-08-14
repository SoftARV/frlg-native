# 0019 — A save this port writes is a save a cartridge could have written

**Status:** accepted
**Supersedes:** the "types that live only in RAM keep this build's layout" clause of
[ADR 0018](0018-match-the-cartridges-structure-layout.md). The rest of 0018 stands.

**Date:** 2026-08-15

## Context

ADR 0018 widened the types whose data the cart supplies, and drew the line there:

> Types that live only in RAM keep this build's layout. Their layout is nobody's business but ours,
> and widening them would change the save format for no benefit.

That reasoning had a hole in it. The save blocks are not "only in RAM" — they are written to a file
that is meant to be the same 128 KiB flash image a Game Boy Advance writes. Nine structures inside
them are narrower here than in the cartridge, and the difference showed:

| | ours | cartridge |
| --- | --- | --- |
| `SaveBlock1` | 15,528 | 15,720 |
| `SaveBlock2` | 3,872 | 3,876 |
| `PokemonStorage` | 33,744 | 33,744 |

Nothing about play was wrong, because a save we wrote is a save we could read. What was wrong is
everything outside that circle: a save lifted from a real cartridge or an emulator would be misread,
and a save this port wrote would not load anywhere else. The importer that already exists — SHA-1,
cache, first-boot flow — quietly only ever worked on our own files.

Importing a save is not a nice-to-have. It is how somebody with a cartridge they have played for
twenty years starts using this port without starting again.

## Decision

**The save blocks use the cartridge's layout.** Eight structures inside them are widened the same way
[ADR 0018](0018-match-the-cartridges-structure-layout.md) widens cart-supplied types:
`Time`, `Mail`, `DayCareMail`, `QuestLogObjectEvent`, `LinkBattleRecords`, `RamScriptData`,
`FameCheckerSaveData` and `WonderNewsMetadata`.

`ExternalEventFlags` is deliberately *not* widened. Upstream already marks it
`__attribute__((packed))`, which agbcc honours, so it is 21 bytes in both worlds; widening it would
push every field after it out of place. This is the same trap `LevelUpMove` set in 0018, and it was
caught the same way — by the patcher refusing a definition that did not match its expected shape
rather than by a crash.

**The verification is the total, not the type.** Per-type reasoning is what makes this class
dangerous. Here there is a much stronger check available: the save blocks must come out at exactly
15,720 and 3,876, the sizes the reference build gives them. Widening the wrong set does not reach
those numbers.

## Consequences

**Every save written before this is unreadable, and had to be migrated.** Not degraded — a save from
the old layout segfaults the new build within a few thousand frames, because a misread field becomes
a null pointer. `tools/migrate_save.py` rewrites an old save into the new layout: it derives the
field mapping by reading both binaries with `pahole`, matching SaveBlock members by name, and
recursing wherever a member changed size, then reassembles the sectors and recomputes their
checksums. Nothing about the mapping is written down by hand, so it cannot drift from the structs.

That migration is verified the strongest way available: the migrated save, replayed against the same
recorded trace on the new build, produces a frame **byte-identical** to the one the old build
produced from the old save. Same position, same party, same everything.

**From here, importing a real save is a file copy.** So is exporting one. The launcher's import path
needs no translation layer, which is the reason this was worth breaking saves for.

**A save is now a compatibility surface.** Changing any structure reachable from `SaveBlock1`,
`SaveBlock2` or `PokemonStorage` changes the format, and `tools/audit_layout.py` will not catch it —
that tool watches types the *cart* supplies. The check that catches this one is the pair of sizes
above, and it belongs in the test suite rather than in somebody's memory.
