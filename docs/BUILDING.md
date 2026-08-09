# Building

## Get the source

```sh
git clone --recurse-submodules <repo>
cd frlg-native
```

If you already cloned without submodules: `git submodule update --init --recursive`.

## Dependencies

The port builds **32-bit** until the Phase 8 migration ([ADR 0003](adr/0003-pointer-width.md)), so
every dependency is needed in its 32-bit form.

| Need | Why | Arch package |
| --- | --- | --- |
| C compiler with multilib | the port itself | `gcc` + `lib32-glibc` |
| CMake ≥ 3.24, Ninja | build system | `cmake`, `ninja` |
| SDL3, 32-bit | host backend (not needed for `headless`) | `lib32-sdl3` |
| `arm-none-eabi-gcc`, binutils | the reference ROM build | `arm-none-eabi-gcc`, `arm-none-eabi-binutils`, `arm-none-eabi-newlib` |
| libpng | upstream's asset tools | `libpng` |

## Build the port

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
```

Presets: `linux-debug`, `linux-release`, and `headless` — the last builds against the `null` host
backend, needs no SDL, and is what CI and the determinism harness use.

## Build the reference ROM

The ROM is the oracle: when the port draws something wrong, the ROM is what "right" means. Keep it
building.

```sh
cd vendor/pokefirered
./build_tools.sh
make -j"$(nproc)" modern
```

This produces `pokefirered_modern.gba`. It is built with modern GCC, so it does **not** reproduce
the original cartridge's checksum — byte-matching needs `agbcc`, which is a separate setup and is
not required for this project. For behavioural reference, the modern ROM is equivalent.

Upstream's `build_tools.sh` is stale: it references `tools/aif2pcm`, which no longer exists. Build
via `make tools` instead if the script fails.
