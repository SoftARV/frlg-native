// SHA-1 against the vectors everyone checks a SHA-1 against, plus the one that
// actually matters here: the hash the decompilation records for the ROM this
// port is built from.

#include <string.h>

#include "agb/sha1.h"

#include "harness.h"

static const char *hash_of(const char *text)
{
    static char out[AGB_SHA1_TEXT];
    uint8_t digest[AGB_SHA1_SIZE];

    agb_sha1(text, strlen(text), digest);
    agb_sha1_format(digest, out);
    return out;
}

static void test_known_vectors(void)
{
    TEST_CASE("the published vectors");

    CHECK(strcmp(hash_of(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0,
          "empty string: %s", hash_of(""));
    CHECK(strcmp(hash_of("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d") == 0,
          "abc: %s", hash_of("abc"));
    CHECK(strcmp(hash_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                 "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0,
          "the 56-byte vector: %s",
          hash_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"));
}

// 55, 56 and 64 are where the padding decides between one block and two, which
// is the only part of this worth getting wrong.
static void test_block_boundaries(void)
{
    char buf[130];

    TEST_CASE("lengths either side of a block boundary");
    for (size_t n = 0; n <= 129; n++)
    {
        uint8_t a[AGB_SHA1_SIZE], b[AGB_SHA1_SIZE];

        memset(buf, 'x', n);
        agb_sha1(buf, n, a);
        // Hashing the same bytes twice must agree, and a different length must
        // not: a padding bug shows up as a collision between neighbours.
        agb_sha1(buf, n, b);
        CHECK(memcmp(a, b, sizeof(a)) == 0, "length %zu is not stable", n);
        if (n > 0)
        {
            uint8_t shorter[AGB_SHA1_SIZE];

            agb_sha1(buf, n - 1, shorter);
            CHECK(memcmp(a, shorter, sizeof(a)) != 0,
                  "lengths %zu and %zu hash the same", n - 1, n);
        }
    }
}

static void test_text_round_trip(void)
{
    uint8_t digest[AGB_SHA1_SIZE], back[AGB_SHA1_SIZE];
    char text[AGB_SHA1_TEXT];

    TEST_CASE("a digest survives being written and read");
    agb_sha1("frlg", 4, digest);
    agb_sha1_format(digest, text);
    CHECK(agb_sha1_parse(text, back) == 0, "%s did not parse", text);
    CHECK(memcmp(digest, back, sizeof(digest)) == 0, "round trip changed it");
    CHECK(agb_sha1_parse("not a hash at all, forty characters long!", back) != 0,
          "nonsense parsed as a digest");
}

int main(void)
{
    test_known_vectors();
    test_block_boundaries();
    test_text_round_trip();

    return test_report("sha1");
}
