// The cartridge's save flash: 128 KiB in thirty-two 4 KiB sectors, backed by a
// file on the host in the layout emulators use, so a save moves between this
// port, mGBA and a real cartridge unchanged.
//
// This replaces the chip rather than the driver. Upstream's drivers talk to real
// flash through timed command sequences -- write 0xAA here, 0x55 there, poll
// until the chip stops toggling -- and one of them runs code copied onto the
// stack, which is why they are not built. The save code above them is upstream's
// and does not know the difference.
//
// The chip lives in the arena's SRAM region, which is exactly 128 KiB, so a
// sector is addressed the same way the hardware addresses it.

#include <stdio.h>
#include <string.h>

#include "agb/flash.h"
#include "agb/memmap.h"

#include "gba/types.h"

#define SECTOR_BYTES 4096
#define SECTOR_COUNT 32
#define FLASH_BYTES (SECTOR_BYTES * SECTOR_COUNT)

// An erased flash cell reads as 0xFF, and the save code checks for that.
#define ERASED 0xFF

static char flash_path[512];
static bool flash_dirty;
static bool flash_usable;

static u8 *sector_at(u16 sector)
{
    return agb_mem.sram + ((u32)(sector % SECTOR_COUNT) * SECTOR_BYTES);
}

bool agb_flash_open(const char *path)
{
    FILE *fh;

    memset(agb_mem.sram, ERASED, FLASH_BYTES);
    flash_dirty = false;
    flash_usable = false;

    if (path == NULL || *path == '\0')
        return false;

    snprintf(flash_path, sizeof(flash_path), "%s", path);
    flash_usable = true;

    fh = fopen(flash_path, "rb");
    if (fh == NULL)
        return true; // no save yet, which is what a new cartridge looks like

    // A short file fills what it can and leaves the rest erased, so a save from
    // a smaller chip still reads.
    fread(agb_mem.sram, 1, FLASH_BYTES, fh);
    fclose(fh);
    return true;
}

void agb_flash_flush(void)
{
    FILE *fh;

    if (!flash_dirty || !flash_usable)
        return;

    fh = fopen(flash_path, "wb");
    if (fh == NULL)
        return;

    fwrite(agb_mem.sram, 1, FLASH_BYTES, fh);
    fclose(fh);
    flash_dirty = false;
}

void agb_flash_close(void)
{
    agb_flash_flush();
}

// ------------------------------------------------ what the game calls ---

// Nothing to identify: the chip is whatever we say it is, and the save code only
// checks that this succeeds. Zero is success.
u16 IdentifyFlash(void)
{
    return 0;
}

u16 ReadFlashId(void)
{
    // The maker and device of the chip upstream expects, so anything comparing
    // against MX29L010's ids agrees.
    return 0x09C2;
}

// The drivers time their command sequences off a hardware timer. Nothing here
// is timed, so there is no interrupt to install.
u16 SetFlashTimerIntr(u8 timerNum, void (**intrFunc)(void))
{
    (void)timerNum;
    if (intrFunc != NULL)
        *intrFunc = NULL;
    return 0;
}

void StartFlashTimer(u8 phase)
{
    (void)phase;
}

void StopFlashTimer(void)
{
}

void SwitchFlashBank(u8 bankNum)
{
    // The hardware pages 64 KiB at a time; the whole chip is addressable here,
    // so the sector number carries the bank and there is nothing to switch.
    (void)bankNum;
}

void ReadFlash(u16 sectorNum, u32 offset, void *dest, u32 size)
{
    const u8 *src = sector_at(sectorNum) + offset;

    if (offset >= SECTOR_BYTES)
        return;
    if (offset + size > SECTOR_BYTES)
        size = SECTOR_BYTES - offset;

    memcpy(dest, src, size);
}

u16 EraseFlashChip_MX(void)
{
    memset(agb_mem.sram, ERASED, FLASH_BYTES);
    flash_dirty = true;
    return 0;
}

u16 EraseFlashSector_MX(u16 sectorNum)
{
    memset(sector_at(sectorNum), ERASED, SECTOR_BYTES);
    flash_dirty = true;
    return 0;
}

// Flash can only clear bits, so programming a byte ands it into what is there.
// The save code erases before it writes, but something that did not would see
// the same result here as on the chip.
u16 ProgramFlashByte_MX(u16 sectorNum, u32 offset, u8 data)
{
    u8 *cell;

    if (offset >= SECTOR_BYTES)
        return 1;

    cell = sector_at(sectorNum) + offset;
    *cell = (u8)(*cell & data);
    flash_dirty = true;
    return 0;
}

u16 ProgramFlashSector_MX(u16 sectorNum, void *src)
{
    u8 *dest = sector_at(sectorNum);
    const u8 *in = src;

    for (int i = 0; i < SECTOR_BYTES; i++)
        dest[i] = (u8)(dest[i] & in[i]);

    flash_dirty = true;
    return 0;
}

u16 WaitForFlashWrite_Common(u8 phase, u8 *addr, u8 lastData)
{
    // A real driver polls the chip until it stops toggling. Ours is done by the
    // time it is asked, so the answer is whatever it wrote.
    (void)phase;
    (void)lastData;
    return *addr;
}

// The pair the game reaches through function pointers rather than by name.
u16 (*EraseFlashSector)(u16) = EraseFlashSector_MX;
u16 (*ProgramFlashByte)(u16, u32, u8) = ProgramFlashByte_MX;

// Erase and write, then check what landed. Returning non-zero tells the save
// code the sector is damaged, which sends it to another one.
u32 ProgramFlashSectorAndVerify(u16 sectorNum, u8 *src)
{
    u8 *dest = sector_at(sectorNum);

    memcpy(dest, src, SECTOR_BYTES);
    flash_dirty = true;
    agb_flash_flush();

    return memcmp(dest, src, SECTOR_BYTES) == 0 ? 0 : 1;
}

u32 ProgramFlashSectorAndVerifyNBytes(u16 sectorNum, void *dataSrc, u32 n)
{
    u8 *dest = sector_at(sectorNum);

    if (n > SECTOR_BYTES)
        n = SECTOR_BYTES;

    memset(dest, ERASED, SECTOR_BYTES);
    memcpy(dest, dataSrc, n);
    flash_dirty = true;
    agb_flash_flush();

    return memcmp(dest, dataSrc, n) == 0 ? 0 : 1;
}
