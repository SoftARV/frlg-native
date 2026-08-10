// The save chip.
//
// This replaces the cartridge's flash rather than the driver that talks to it,
// so what matters is that it behaves like the chip the save code expects: reads
// come back as written, an erased cell is 0xFF, programming can only clear bits,
// and what was written is still there next time.

#include <stdio.h>
#include <string.h>

#include "agb/flash.h"
#include "agb/memmap.h"

#include "gba/types.h"
#include "gba/flash_internal.h"

#include "harness.h"

#define SECTOR_BYTES 4096
#define SAVE_PATH "/tmp/frlg-test-flash.sav"

static u8 buffer[SECTOR_BYTES];
static u8 pattern[SECTOR_BYTES];

static void fill_pattern(int seed)
{
    for (int i = 0; i < SECTOR_BYTES; i++)
        pattern[i] = (u8)((i * 7 + seed) & 0xFF);
}

static void reset(const char *name)
{
    TEST_CASE(name);
    remove(SAVE_PATH);
    agb_flash_open(SAVE_PATH);
    memset(buffer, 0, sizeof(buffer));
}

// A chip with no file behind it reads as erased, which is what a new cartridge
// looks like to the save code.
static void test_starts_erased(void)
{
    reset("a chip with no save file reads erased");
    ReadFlash(0, 0, buffer, SECTOR_BYTES);

    for (int i = 0; i < SECTOR_BYTES; i++)
        CHECK(buffer[i] == 0xFF, "byte %d read %02X, not erased", i, buffer[i]);
}

static void test_program_and_read_back(void)
{
    reset("a programmed sector reads back");
    fill_pattern(1);

    CHECK(ProgramFlashSectorAndVerify(3, pattern) == 0, "programming reported damage");

    ReadFlash(3, 0, buffer, SECTOR_BYTES);
    CHECK(memcmp(buffer, pattern, SECTOR_BYTES) == 0, "the sector did not read back");

    // And it did not disturb its neighbours.
    ReadFlash(2, 0, buffer, SECTOR_BYTES);
    CHECK(buffer[0] == 0xFF, "the sector before it was written too");
    ReadFlash(4, 0, buffer, SECTOR_BYTES);
    CHECK(buffer[0] == 0xFF, "the sector after it was written too");
}

// Reading part of a sector is what the save code does for footers.
static void test_partial_read(void)
{
    reset("a read at an offset returns that part");
    fill_pattern(2);
    ProgramFlashSectorAndVerify(0, pattern);

    ReadFlash(0, 100, buffer, 16);
    CHECK(memcmp(buffer, pattern + 100, 16) == 0, "the offset read came back wrong");
}

// A read must not run past the end of its sector into the next one.
static void test_read_is_bounded(void)
{
    reset("a read is clipped to its sector");
    fill_pattern(3);
    ProgramFlashSectorAndVerify(1, pattern);
    memset(buffer, 0x5A, sizeof(buffer));

    ReadFlash(1, SECTOR_BYTES - 8, buffer, 64);

    CHECK(memcmp(buffer, pattern + SECTOR_BYTES - 8, 8) == 0,
          "the tail of the sector did not read");
    CHECK(buffer[8] == 0x5A, "the read ran past the end of the sector");
}

// An offset past the end of a sector reads nothing at all. Exactly at the end is
// not enough to prove the guard earns its place -- the clip below it yields a
// size of zero there anyway. Beyond the end is where it matters: the subtraction
// underflows and asks for a copy of nearly four gigabytes.
static void test_read_past_the_end(void)
{
    reset("a read starting past the sector returns nothing");
    fill_pattern(8);
    ProgramFlashSectorAndVerify(1, pattern);
    memset(buffer, 0x5A, sizeof(buffer));

    ReadFlash(1, SECTOR_BYTES + 8, buffer, 32);

    for (int i = 0; i < 32; i++)
        CHECK(buffer[i] == 0x5A, "byte %d was written as %02X by a read past the end",
              i, buffer[i]);
}

static void test_erase(void)
{
    reset("erasing a sector returns it to 0xFF");
    fill_pattern(4);
    ProgramFlashSectorAndVerify(5, pattern);

    CHECK(EraseFlashSector(5) == 0, "the erase reported a failure");

    ReadFlash(5, 0, buffer, SECTOR_BYTES);
    for (int i = 0; i < 16; i++)
        CHECK(buffer[i] == 0xFF, "byte %d survived the erase as %02X", i, buffer[i]);
}

// Flash cells can only be cleared, never set: that is why the save code erases
// before it writes.
static void test_programming_only_clears(void)
{
    reset("programming a byte can only clear bits");
    CHECK(ProgramFlashByte(7, 0, 0xF0) == 0, "the byte program failed");
    ReadFlash(7, 0, buffer, 1);
    CHECK(buffer[0] == 0xF0, "the first write came out %02X", buffer[0]);

    // 0x0F cannot put back the bits 0xF0 cleared.
    ProgramFlashByte(7, 0, 0x0F);
    ReadFlash(7, 0, buffer, 1);
    CHECK(buffer[0] == 0x00, "a second write set bits again: %02X", buffer[0]);
}

// What was saved is there next time, which is the whole point.
static void test_persists_across_open(void)
{
    reset("a save survives closing and reopening");
    fill_pattern(9);
    ProgramFlashSectorAndVerify(11, pattern);
    agb_flash_close();

    // Scribble over the chip so a stale buffer cannot pass this.
    memset(agb_mem.sram, 0x00, 0x20000);

    CHECK(agb_flash_open(SAVE_PATH), "the save file could not be reopened");
    ReadFlash(11, 0, buffer, SECTOR_BYTES);
    CHECK(memcmp(buffer, pattern, SECTOR_BYTES) == 0, "the save did not survive");
}

// The file is the size a cartridge's flash is, so emulators and hardware read it.
static void test_file_is_a_whole_chip(void)
{
    FILE *fh;
    long size;

    reset("the save file is a whole 128 KiB chip");
    fill_pattern(5);
    ProgramFlashSectorAndVerify(0, pattern);
    agb_flash_close();

    fh = fopen(SAVE_PATH, "rb");
    CHECK(fh != NULL, "no save file was written");
    if (fh == NULL)
        return;
    fseek(fh, 0, SEEK_END);
    size = ftell(fh);
    fclose(fh);

    CHECK(size == 128 * 1024, "the save file is %ld bytes, not 128 KiB", size);
}

// A save from a smaller chip fills what it can and leaves the rest erased.
static void test_short_file_is_accepted(void)
{
    FILE *fh = fopen(SAVE_PATH, "wb");

    TEST_CASE("a short save file is padded with erased cells");
    fill_pattern(6);
    fwrite(pattern, 1, 64, fh);
    fclose(fh);

    CHECK(agb_flash_open(SAVE_PATH), "the short file was refused");
    ReadFlash(0, 0, buffer, 64);
    CHECK(memcmp(buffer, pattern, 64) == 0, "the part that was there did not load");
    ReadFlash(0, 64, buffer, 16);
    CHECK(buffer[0] == 0xFF, "the rest was not erased, it is %02X", buffer[0]);
}

int main(void)
{
    test_starts_erased();
    test_program_and_read_back();
    test_partial_read();
    test_read_is_bounded();
    test_read_past_the_end();
    test_erase();
    test_programming_only_clears();
    test_persists_across_open();
    test_file_is_a_whole_chip();
    test_short_file_is_accepted();

    remove(SAVE_PATH);
    return test_report("flash");
}
