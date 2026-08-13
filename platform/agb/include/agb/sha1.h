// SHA-1, for identifying a cartridge dump.
//
// The decompilation ships a `.sha1` per revision and checks its own build
// against it; the port asks the same question of the file a player supplies.

#ifndef GUARD_AGB_SHA1_H
#define GUARD_AGB_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define AGB_SHA1_SIZE 20
#define AGB_SHA1_TEXT (AGB_SHA1_SIZE * 2 + 1)

void agb_sha1(const void *data, size_t length, uint8_t out[AGB_SHA1_SIZE]);

// Lower-case hex, the way the `.sha1` files write it.
void agb_sha1_format(const uint8_t digest[AGB_SHA1_SIZE], char out[AGB_SHA1_TEXT]);

// Reads 40 hex characters. Returns non-zero if they are not all hex.
int agb_sha1_parse(const char *text, uint8_t out[AGB_SHA1_SIZE]);

#endif // GUARD_AGB_SHA1_H
