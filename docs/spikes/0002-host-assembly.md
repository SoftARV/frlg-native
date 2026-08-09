# Spike 0002 — Assembling the game's data on a host toolchain

**Question:** the roadmap assumed `data/*.s` — event scripts, battle scripts, map data — could be
assembled by the host assembler, since those files use only portable GAS directives (`.byte`,
`.2byte`, `.4byte`, `.string`, `.include`, `.macro`). Is that true?

**Verdict: no. No host assembler can build them, and the file that fails is the one that matters
most.** The data must come from the ROM image instead — which turned out to be both achievable and
architecturally cleaner.

## What was tried

The upstream pipeline is `preproc → cpp → preproc -ie → as`, reproduced exactly, with the vendor
tree as the working directory so `.include` resolves.

| Assembler | Result | Failure |
| --- | --- | --- |
| GNU `as --32` | 13 / 14 | `event_scripts.s` — "too many positional arguments" |
| `clang -m32` (integrated) | 12 / 14 | `event_scripts.s`, `mystery_event_msg.s` — "expected absolute expression" |

Neither builds `event_scripts.s`, and the two fail for **different, mutually exclusive reasons**.

### GNU as: macro arguments split on whitespace

```
goto_if_ge VAR_TEMP_1, (MAX_COINS + 1) - 500, CeladonCity_GameCorner_EventScript_...
```

Minimal reproduction:

```asm
.macro m3 a, b, c
.4byte \a, \b, \c
.endm
.set MAX, 9999
m3 1, (MAX + 1) - 500, 3
```

`as --32` reports "too many positional arguments"; `arm-none-eabi-as` accepts it. x86 GAS treats
whitespace as an argument separator, so `(MAX + 1) - 500` splits into three arguments. `--alternate`
does not change this. Five lines in one file trip it.

### clang: `.if` demands an absolute expression

```
.if STR_VAR_1 == STR_VAR_1
```

This sits inside a `.macro` body in an `.inc` file, which the assembler reads via `.include` — so
cpp never sees it and never substitutes the constant. GNU as evaluates `X == X` for an undefined
`X`; clang's integrated assembler requires both sides to be absolute and refuses.

## Why this settles the design

`event_scripts.s` contributes **0x66F36 — 421 KB, 83%** of the 508 KB `script_data` section. It is
not a file that can be worked around or stubbed.

The conclusion is not a setback, because [ADR 0006](../adr/0006-rom-supplied-data.md) already says
this data ships from the player's ROM rather than from our binary. The spike simply proves the
"developer data path" cannot take a shortcut here either: **`data/*.s` is never assembled, on any
platform.** That removes a whole class of portability risk — MSVC has no GAS at all, and Android,
iOS and wasm all use clang, which fails differently from GNU as.

One toolchain difference proving fatal on x86 is a strong signal about the other four targets.

## The mechanism, proven

Data symbols the host cannot build are bound into the cart region at link time:

```
cc -m32 ... -Wl,--defsym,FakeScript=agb_cart+0x123456
```

Verified end to end: the bound symbol resolves to exactly `agb_cart + 0x123456`, and a byte written
through `agb_cart` reads back through the bound symbol.

Applied for real by `tools/gen_cart_syms.py`, which reads the ROM build's `.sym` and emits one
`--defsym` per symbol in the `script_data` range:

| | |
| --- | --- |
| Unresolved symbols before | 1,295 |
| Bound into the cart region | **1,107** |
| **Unresolved after** | **176** |

The remaining 176 all come from the seven excluded files (the five ARM-assembly `.c` files, plus
`libagbsyscall.s` and `m4a_1.s`), and are the next increment of Phase 1.

A note on method: the first run of this link reported *zero* undefined symbols, which was wrong —
the compiler driver expands `@response` files itself and rejects `--defsym`, so nothing was passed
to the linker at all. Arguments for the linker need `-Wl,--defsym,...`. The generator has a
`--driver` flag for this, and a "success" that good is worth distrusting.

## Open question this raises for Phase 7

Binding works cleanly for `data/*.s` symbols because they are defined *only* in assembly we do not
build, so nothing collides. Data defined by **C initializers** in `src/` is different: the compiler
emits it into the binary from source, and excluding it needs more than a linker flag —
`-fdata-sections` plus per-symbol section stripping, or an upstream edit we have ruled out.

Full extraction is therefore proven for script and map data, and still unproven for `.c`-defined
tables. This does not block Phases 1–6.
