# ADR 0007 — Embedded Lua with schema-driven registries

**Status:** accepted

## Context

Modding is a headline goal, not an afterthought. `gen1recomp` demonstrates what the ceiling looks
like when the mod surface is designed rather than accreted: their voxel 3D overworld — the thing
that got them written up by Polygon, Kotaku and Digital Foundry — is roughly 120 lines of glue over
one registry record.

The load-bearing idea in their design is not "we embed a scripting language". It is that a single
schema table is the source of truth for every registry: its merge semantics, the data path it
writes, and the validation every registration is checked against. The loader is built from that
table and the reference documentation is *generated* from it, so engine and docs cannot drift.

## Decision

Embed Lua. Define every moddable surface in one schema table that declares, per registry:

- **merge semantics** — `record`, `deep` or `compose`
- **the data path** the merge writes into
- **the value schema** every registration is validated against

Registrations come in four modes: `register`, `override`, `patch` and `remove`. Reference docs are
generated from the schema, never written by hand.

Mods act at three levels: patching game data in the cart region after import, registering hooks on
engine events, and owning **render pipelines** — display modes layered above the framebuffer, which
is the seam that makes an alternative renderer a mod rather than a fork.

Two rules are adopted verbatim because they are hard-won:

- **A callback that throws retires only its own feature**, attributed to its mod, and the frame
  falls back to vanilla. A broken mod costs a display mode, never the game.
- **Availability is re-read every frame**, so a pipeline that cannot run headless simply does not,
  and shipping one enabled is safe.

## Consequences

- Modding is a platform, not a patch format: mods carry versions, profiles and their own settings.
- Validation with typo suggestions at registration time turns the most common authoring mistake
  into an error message instead of silent nonsense.
- The schema becomes a compatibility contract. Changing a registry's shape is a breaking change for
  every mod using it, and needs the same care as any public API.
- **iOS forbids JIT**, so the interpreter must run without it there. Lua is chosen over LuaJIT for
  exactly this reason; performance-critical work stays in C, where it already is.
- A scripting runtime is an attack surface. Mods get no unrestricted filesystem or network access.

## Alternatives rejected

**Data-only overrides** — assets and tables through the VFS, no behaviour. Much smaller, and rules
out every mod worth the infrastructure.

**Native plugin ABI** — full access and native speed, but demands a toolchain per platform from
every author, and is a non-starter on iOS.

**Defer past 1.0** — the registry seams are cheap now and expensive to retrofit, since they dictate
how the engine reaches its own data.
