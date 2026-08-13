// SHA-1, for saying whether a ROM is the one this build describes.
//
// Written out rather than linked because it is eighty lines and the alternative
// is a dependency on every platform this port is going to: Windows, Android and
// web all have to build it too, and none of them should need a crypto library
// to answer one question about one file.
//
// Not used for anything security-bearing. It identifies a cartridge dump, which
// is what the decompilation's own `.sha1` files do.

#include <string.h>

#include "agb/sha1.h"

static uint32_t rotl(uint32_t v, int by)
{
    return (v << by) | (v >> (32 - by));
}

static void block(uint32_t state[5], const uint8_t *chunk)
{
    uint32_t w[80];
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)chunk[i * 4] << 24) | ((uint32_t)chunk[i * 4 + 1] << 16)
             | ((uint32_t)chunk[i * 4 + 2] << 8) | (uint32_t)chunk[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    for (int i = 0; i < 80; i++)
    {
        uint32_t f, k;

        if (i < 20)
        {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        {
            uint32_t t = rotl(a, 5) + f + e + k + w[i];

            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void agb_sha1(const void *data, size_t length, uint8_t out[AGB_SHA1_SIZE])
{
    static const uint32_t seed[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE,
                                     0x10325476, 0xC3D2E1F0};
    uint32_t state[5];
    const uint8_t *bytes = data;
    uint8_t tail[128];
    size_t whole = length / 64;
    size_t rest = length - whole * 64;
    size_t padded;
    uint64_t bits = (uint64_t)length * 8;

    memcpy(state, seed, sizeof(state));
    for (size_t i = 0; i < whole; i++)
        block(state, bytes + i * 64);

    // The tail is the remainder, a 1 bit, zeros, and the length in bits -- which
    // needs one more 64-byte block, or two when the remainder leaves no room.
    memcpy(tail, bytes + whole * 64, rest);
    tail[rest] = 0x80;
    padded = (rest < 56) ? 64 : 128;
    memset(tail + rest + 1, 0, padded - rest - 1 - 8);
    for (int i = 0; i < 8; i++)
        tail[padded - 1 - i] = (uint8_t)(bits >> (i * 8));
    for (size_t i = 0; i < padded / 64; i++)
        block(state, tail + i * 64);

    for (int i = 0; i < 5; i++)
    {
        out[i * 4] = (uint8_t)(state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)state[i];
    }
}

void agb_sha1_format(const uint8_t digest[AGB_SHA1_SIZE], char out[AGB_SHA1_TEXT])
{
    static const char nibble[] = "0123456789abcdef";

    for (int i = 0; i < AGB_SHA1_SIZE; i++)
    {
        out[i * 2] = nibble[digest[i] >> 4];
        out[i * 2 + 1] = nibble[digest[i] & 0xF];
    }
    out[AGB_SHA1_SIZE * 2] = '\0';
}

int agb_sha1_parse(const char *text, uint8_t out[AGB_SHA1_SIZE])
{
    for (int i = 0; i < AGB_SHA1_SIZE * 2; i++)
    {
        char c = text[i];
        int value;

        if (c >= '0' && c <= '9')
            value = c - '0';
        else if (c >= 'a' && c <= 'f')
            value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            value = c - 'A' + 10;
        else
            return -1;

        if (i % 2 == 0)
            out[i / 2] = (uint8_t)(value << 4);
        else
            out[i / 2] |= (uint8_t)value;
    }
    return 0;
}
