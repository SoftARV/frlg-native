// Everything a session collected, as one file somebody can attach to an issue.
//
// A folder is not a deliverable: it arrives as four separate attachments, or as
// three, and the missing one is always the save. A zip is what the issue tracker
// takes, so this writes one -- stored, uncompressed, about two hundred lines of
// format rather than a dependency the Android and web ports would then have to
// carry. The contents are already small; the save is the only file of any size.

#define _GNU_SOURCE

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "host_session.h"

#define PATH_LEN 512
#define MAX_ENTRIES 32

static char bundle_path[PATH_LEN];

static uint32_t crc32_of(const unsigned char *data, size_t len, uint32_t crc)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static void put16(unsigned char *at, unsigned value)
{
    at[0] = (unsigned char)(value & 0xFF);
    at[1] = (unsigned char)((value >> 8) & 0xFF);
}

static void put32(unsigned char *at, uint32_t value)
{
    at[0] = (unsigned char)(value & 0xFF);
    at[1] = (unsigned char)((value >> 8) & 0xFF);
    at[2] = (unsigned char)((value >> 16) & 0xFF);
    at[3] = (unsigned char)((value >> 24) & 0xFF);
}

struct entry
{
    char name[64];
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
};

// Zip carries MS-DOS timestamps, which is not a choice this format offers a way
// out of.
static void dos_stamp(unsigned *dos_time, unsigned *dos_date)
{
    time_t now = time(NULL);
    struct tm parts;

    localtime_r(&now, &parts);
    *dos_time = (unsigned)((parts.tm_hour << 11) | (parts.tm_min << 5) | (parts.tm_sec / 2));
    *dos_date = (unsigned)(((parts.tm_year + 1900 - 1980) << 9)
                           | ((parts.tm_mon + 1) << 5) | parts.tm_mday);
}

static int append_file(FILE *zip, const char *dir, const char *name,
                       struct entry *out, unsigned dos_time, unsigned dos_date)
{
    unsigned char header[30];
    char path[PATH_LEN];
    unsigned char block[8192];
    FILE *in;
    size_t got;
    long start;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    in = fopen(path, "rb");
    if (in == NULL)
        return -1;

    snprintf(out->name, sizeof(out->name), "%s", name);
    out->crc = 0;
    out->size = 0;

    // The sizes and the checksum are only known after reading, and a stored
    // entry has nowhere to put them afterwards without a data descriptor -- so
    // the file is read once to measure it and once to write it.
    while ((got = fread(block, 1, sizeof(block), in)) > 0)
    {
        out->crc = crc32_of(block, got, out->crc);
        out->size += (uint32_t)got;
    }
    rewind(in);

    start = ftell(zip);
    out->offset = (uint32_t)start;

    memset(header, 0, sizeof(header));
    put32(header, 0x04034B50);
    put16(header + 4, 20);            // version needed
    put16(header + 6, 0);             // flags
    put16(header + 8, 0);             // method: stored
    put16(header + 10, dos_time);
    put16(header + 12, dos_date);
    put32(header + 14, out->crc);
    put32(header + 18, out->size);
    put32(header + 22, out->size);
    put16(header + 26, (unsigned)strlen(out->name));
    put16(header + 28, 0);
    fwrite(header, 1, sizeof(header), zip);
    fwrite(out->name, 1, strlen(out->name), zip);

    while ((got = fread(block, 1, sizeof(block), in)) > 0)
        fwrite(block, 1, got, zip);
    fclose(in);
    return 0;
}

const char *host_session_bundle(void)
{
    const char *dir = host_session_dir();
    struct entry entries[MAX_ENTRIES];
    unsigned count = 0;
    unsigned dos_time, dos_date;
    FILE *zip;
    DIR *listing;
    struct dirent *item;
    long central;

    if (dir == NULL)
        return NULL;

    snprintf(bundle_path, sizeof(bundle_path), "%s/report.zip", dir);
    zip = fopen(bundle_path, "wb");
    if (zip == NULL)
        return NULL;

    dos_stamp(&dos_time, &dos_date);

    listing = opendir(dir);
    if (listing == NULL)
    {
        fclose(zip);
        return NULL;
    }
    while ((item = readdir(listing)) != NULL && count < MAX_ENTRIES)
    {
        if (item->d_name[0] == '.')
            continue;
        // Not itself, and not a bundle from an earlier crash in the same run.
        if (strcmp(item->d_name, "report.zip") == 0)
            continue;
        if (append_file(zip, dir, item->d_name, &entries[count], dos_time, dos_date) == 0)
            count++;
    }
    closedir(listing);

    central = ftell(zip);
    for (unsigned i = 0; i < count; i++)
    {
        unsigned char record[46];
        size_t name_len = strlen(entries[i].name);

        memset(record, 0, sizeof(record));
        put32(record, 0x02014B50);
        put16(record + 4, 20);        // version made by
        put16(record + 6, 20);        // version needed
        put16(record + 8, 0);
        put16(record + 10, 0);        // stored
        put16(record + 12, dos_time);
        put16(record + 14, dos_date);
        put32(record + 16, entries[i].crc);
        put32(record + 20, entries[i].size);
        put32(record + 24, entries[i].size);
        put16(record + 28, (unsigned)name_len);
        put32(record + 42, entries[i].offset);
        fwrite(record, 1, sizeof(record), zip);
        fwrite(entries[i].name, 1, name_len, zip);
    }

    {
        unsigned char end[22];
        long central_size = ftell(zip) - central;

        memset(end, 0, sizeof(end));
        put32(end, 0x06054B50);
        put16(end + 8, count);
        put16(end + 10, count);
        put32(end + 12, (uint32_t)central_size);
        put32(end + 16, (uint32_t)central);
        fwrite(end, 1, sizeof(end), zip);
    }

    fclose(zip);
    return count > 0 ? bundle_path : NULL;
}
