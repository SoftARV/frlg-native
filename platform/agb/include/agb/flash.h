// The cartridge's save flash, backed by a file on the host.
//
// The game's own drivers cannot be built: ReadFlashId copies Thumb code into a
// stack buffer and runs it from there, which no host target can do. What they
// drive is a 128 KiB flash chip, and that is what this replaces -- the chip,
// not the driver, so the save code above it is upstream's unchanged.

#ifndef GUARD_AGB_FLASH_H
#define GUARD_AGB_FLASH_H

#include <stdbool.h>

// Point the chip at a file. It is read if it exists and its contents become the
// chip's; otherwise the chip starts erased, as a new cartridge would. Returns
// false if the path cannot be used at all, in which case saving still works but
// nothing survives the run.
bool agb_flash_open(const char *path);

// Write the chip back if anything has changed since the last time. Called after
// each sector is programmed, so a save survives even an abrupt exit.
void agb_flash_flush(void);

void agb_flash_close(void);

#endif // GUARD_AGB_FLASH_H
