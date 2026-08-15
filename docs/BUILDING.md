# Building

## Get the source

```sh
git clone --recurse-submodules https://github.com/SoftARV/frlg-native.git
cd frlg-native
```

If you already cloned without submodules: `git submodule update --init --recursive`.

## Dependencies

The port builds **32-bit** until the Phase 11 migration ([ADR 0003](adr/0003-pointer-width.md)), so
every dependency is needed in its 32-bit form.

| Need | Why | Arch package |
| --- | --- | --- |
| C compiler with multilib | the port itself | `gcc` + `lib32-glibc` |
| CMake ≥ 3.24, Ninja | build system | `cmake`, `ninja` |
| SDL3, 32-bit | host backend (not needed for `headless`) | `lib32-sdl3`, `lib32-libpulse` |
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
the original cartridge's checksum. For behavioural reference that is fine.

## Build the byte-matching ROM

Required from Phase 7 onward, because the shipped manifest is layout-specific and only the matching
build describes the addresses a player's cartridge actually has
([spike 0001](spikes/0001-relocation-table.md)):

```sh
git clone https://github.com/pret/agbcc
cd agbcc && ./build.sh && ./install.sh /path/to/vendor/pokefirered
cd /path/to/vendor/pokefirered && make -j"$(nproc)" compare
```

`compare` verifies the result against `firered.sha1`. Adding
`LDFLAGS="-Map ../../pokefirered.map --emit-relocs"` emits the relocation table without changing a
byte of the ROM.

Upstream's `build_tools.sh` is stale: it references `tools/aif2pcm`, which no longer exists. Build
via `make tools` instead if the script fails.
