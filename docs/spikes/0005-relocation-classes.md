# Spike 0005 — What each relocation record actually needs

**Question:** [spike 0001](0001-relocation-table.md) proved the relocation table can be derived and
is complete, and sorted its records into data, code and RAM. Phase 4 now needs the pass implemented,
because the sound engine is the first subsystem to follow a pointer stored *inside* ROM data. What
does each record need doing to it, and against which build?

**Verdict: implementable, and two of the five categories need nothing done to them at all.** Between
them those two are 11% of the table, and an importer that treated them like the rest would either
corrupt them or fail outright.

## Against which build

The cart region's symbols are bound from **`pokefirered_modern`** — `ports/desktop/CMakeLists.txt`
names `pokefirered_modern.sym` and `pokefirered_modern.elf`. So the image loaded, the bindings, and
the relocation table must all come from that one build, or the offsets do not describe each other.

That does not contradict spike 0001's finding 2. That finding is about **shipping**: a player
supplies a retail cartridge, so the manifest must describe the retail layout, which only the
byte-matching build reproduces. Development can and should use the modern build throughout; phase 7
regenerates the whole set — bindings included — from the byte-matching one.

No rebuild is needed either way. `pokefirered_modern.elf` already carries `.rel.rodata`,
`.relscript_data` and `.rel.data`, the three sections spike 0001 identified as the ones that matter.

## The table

61,142 records across those three sections, every one classified, nothing left over:

| Class | Count | Share | What the importer does |
| --- | --- | --- | --- |
| Data | 48,146 | 78.74% | `agb_cart + (addr - 0x08000000)` |
| Named code | 5,590 | 9.14% | address of the native function of that name |
| **Interior code** | **6,802** | **11.12%** | **nothing** — see finding 1 |
| RAM | 599 | 0.98% | address of the native variable, **plus the addend** |
| **Script constants** | **5** | **0.01%** | **nothing** — see finding 2 |

## Finding 1: one record in nine points into the middle of a ROM function

6,802 records target `.text` as a *section* rather than a named symbol, and **not one of them lands
on a symbol boundary** — every one is an interior offset, such as `ply_memacc + 94`. They are the
jump tables the original compiler emitted for `switch` statements, held in rodata and branched
through by ROM code.

They cannot be relocated: an address inside a function has no counterpart in a natively compiled
one, whose internal layout is entirely different. They also **need not** be: our compiler emits its
own jump tables into our own rodata, and nothing in our binary ever reads the ROM's. They are dead
data that happens to carry relocations.

This is the record class that would stop a naive importer, because it is the one case where "find
the native address of the target" has no answer. Recognising it as *nothing to do* is what makes the
rest tractable.

## Finding 2: five records are not pointers

The five `R_ARM_ABS16` records are script constants — `SPECIAL_GetMysteryGiftCardStat` = `0x186`,
`SPECIAL_CalculatePlayerPartyCount` = `0x83`, `SPECIAL_ValidateEReaderTrainer` = `0xF6` — assembled
into script bytecode as absolute symbols that happen to be small integers rather than addresses.
Both the width and the value say so: a 16-bit slot cannot hold a native pointer, and the value is an
index into `gSpecials`, which our build derives from the same source and numbers identically.

Filtering the pass to `R_ARM_ABS32` handles them correctly, and doing it deliberately rather than
incidentally is worth the line of code it costs.

## Finding 3: code pointers carry the Thumb bit, native ones do not

5,588 of the 5,590 named code targets have bit 0 set, because a Thumb function pointer on the GBA
does. A native address has no such convention, so the bit is dropped on the way in rather than
carried across. Every named code record has an addend of zero, so the native symbol's address is the
whole answer.

## Finding 4: RAM records need their addends

343 of the 599 RAM records store something other than their symbol's own address —
`gBattleScripting + 14`, `gBattleCommunication + 3`. Since these are `REL` rather than `RELA`
records the addend is not in the record; it is the difference between the word stored in the ROM and
the symbol's value, and it has to be carried onto the native address.

Only 63 distinct RAM symbols are involved, against 3,475 distinct code symbols.

## What implementing it needs

- A generator reading the ELF's relocations and the ROM's stored words, emitting the data offsets as
  a plain list and the code and RAM records as `{offset, &native_symbol, addend}` — the linker fills
  the native addresses in, so no runtime symbol lookup is needed.
- The ROM loaded into the cart region at startup, which nothing does yet
  ([spike 0003](0003-empty-cart-region.md)).
- The pass applied once after loading: 54,335 words rewritten, 6,807 deliberately left alone.

**Not established:** whether relocating in stages is safe. Data-only relocation would leave code
pointers reading as GBA addresses rather than the zeros they read today, which turns a null
dereference into a wild jump. The classes should probably land together.
