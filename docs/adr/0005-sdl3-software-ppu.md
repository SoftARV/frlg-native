# ADR 0005 — SDL3 backend, software PPU

**Status:** accepted

## Context

Two independent choices, both about where portability comes from: what talks to the operating
system, and how pixels get produced.

## Decision

**SDL3** is the single backend for desktop, Android and iOS, behind `host.h`.

The **PPU is a software scanline renderer** producing an RGBA framebuffer. GPU work — upscaling,
filters, presentation — happens in the host layer, above the framebuffer.

## Consequences

- One backend covers every target platform; a new platform is mostly build configuration.
- SDL3 over SDL2: supported mobile story, modern GPU abstraction, sane audio streams. Installed
  here at 3.4.14, in both 64- and 32-bit form.
- Software rendering makes the GBA's window, blending and mosaic rules straightforward to get
  *right*, and keeps `platform/agb` free of graphics-API conditionals.
- At 240×160 the renderer is nowhere near fill-rate bound, so the cost is irrelevant — and it stays
  irrelevant at widescreen resolutions.
- The framebuffer is a clean seam: enhancement work above it (shaders, upscaling) cannot introduce
  accuracy bugs below it.

## Alternatives rejected

**GPU-side PPU** — reproducing per-scanline register changes, window regions and blend modes in
shaders is a large accuracy risk, and would put a graphics API dependency inside the hardware
layer, breaking the dependency rule in `docs/ARCHITECTURE.md` §2.

**SDL2** — mature, but its mobile story and audio API are the previous generation, and mobile is an
explicit target here.
