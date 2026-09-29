/*
 * Note: This file updated in Sep-2026 to benefit from buffers known to be 64 byte 
 * aligned and to remove the memcpy() of 64 bytes for speed on the venerable DS.
 * It tested at about 20% faster than the original when given these constraints.
 * 
 * TeenySHA1 - a header only implementation of the SHA1 algorithm in C. Based
 * on the implementation in boost::uuid::details. Translated to C from
 * https://github.com/mohaps/TinySHA1
 *
 * SHA1 Wikipedia Page: http://en.wikipedia.org/wiki/SHA-1
 *
 * Copyright (c) 2012-25 SAURAV MOHAPATRA <mohaps@gmail.com>
 * Copyright (c) 2025    ALEXEY KUTEPOV   <reximkut@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/*
 * TeenySHA1 - a header only implementation of the SHA1 algorithm in C. Based
 * on the implementation in boost::uuid::details. Translated to C from
 * https://github.com/mohaps/TinySHA1
 *
 * SHA1 Wikipedia Page: http://en.wikipedia.org/wiki/SHA-1
 *
 * Copyright (c) 2012-25 SAURAV MOHAPATRA <mohaps@gmail.com>
 * Copyright (c) 2025    ALEXEY KUTEPOV   <reximkut@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef _TEENY_SHA1_HPP_
#define _TEENY_SHA1_HPP_

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint32_t digest32_t[5];
typedef uint8_t  digest8_t[20];

typedef struct {
    digest32_t digest;
    uint8_t block[64];
    size_t block_byte_index;
    size_t byte_count;
} SHA1;

void sha1_reset(SHA1 *sha1);
void sha1_process_block(SHA1 *sha1, const void *start, const void *end);
void sha1_process_byte(SHA1 *sha1, uint8_t octet);
void sha1_process_bytes(SHA1 *sha1, const void *data, size_t len);
const uint32_t* sha1_get_digest(SHA1 *sha1, digest32_t digest);
const uint8_t* sha1_get_digest_bytes(SHA1 *sha1, digest8_t digest);

#endif


#ifdef TEENY_SHA1_IMPLEMENTATION

static inline uint32_t sha1__rol(uint32_t x, uint32_t n)
{
    return (x << n) | (x >> (32 - n));
}

static inline uint32_t sha1__bswap(uint32_t x)
{
    return ((x & 0x000000FF) << 24) |
           ((x & 0x0000FF00) << 8)  |
           ((x & 0x00FF0000) >> 8) |
           ((x & 0xFF000000) >> 24);
}


/*
 * SHA1 compression.
 *
 * Input block is exactly 64 bytes.
 *
 * Uses a 16-word circular message schedule rather than the
 * 80-word temporary array used by the original implementation.
 *
 * IMPORTANT:
 * The input block is now supplied directly rather than copied
 * through sha1->block. This eliminates a 64-byte memcpy() for
 * every SHA1 block in the normal ROM path.
 */
static void sha1__process_block(SHA1 *sha1, const uint8_t *data)
{
    uint32_t w[16];

    uint32_t a = sha1->digest[0];
    uint32_t b = sha1->digest[1];
    uint32_t c = sha1->digest[2];
    uint32_t d = sha1->digest[3];
    uint32_t e = sha1->digest[4];

    uint32_t *p = w;

    const uint32_t *src = (const uint32_t *)data;

    int i;

    /*
     * First 16 words.
     *
     * SHA1 is big-endian. DS/ARM9 is little-endian.
     */
    for (i = 0; i < 16; i++)
        w[i] = sha1__bswap(src[i]);


    /*
     * Rounds 0-19
     */
    for (i = 0; i < 20; i++)
    {
        uint32_t f = (b & c) | (~b & d);
        uint32_t t = sha1__rol(a, 5) + f + e + 0x5A827999 + p[i & 15];

        if (i >= 16)
        {
            p[i & 15] =
                sha1__rol(
                    p[(i - 3) & 15] ^
                    p[(i - 8) & 15] ^
                    p[(i - 14) & 15] ^
                    p[i & 15], 1);

            t = sha1__rol(a, 5) + f + e + 0x5A827999 + p[i & 15];
        }

        e = d;
        d = c;
        c = sha1__rol(b, 30);
        b = a;
        a = t;
    }


    /*
     * Rounds 20-39
     */
    for (i = 20; i < 40; i++)
    {
        uint32_t f = b ^ c ^ d;

        p[i & 15] =
            sha1__rol(
                p[(i - 3) & 15] ^
                p[(i - 8) & 15] ^
                p[(i - 14) & 15] ^
                p[i & 15], 1);

        uint32_t t =
            sha1__rol(a, 5) + f + e + 0x6ED9EBA1 + p[i & 15];

        e = d;
        d = c;
        c = sha1__rol(b, 30);
        b = a;
        a = t;
    }


    /*
     * Rounds 40-59
     */
    for (i = 40; i < 60; i++)
    {
        uint32_t f = (b & c) | (b & d) | (c & d);

        p[i & 15] =
            sha1__rol(
                p[(i - 3) & 15] ^
                p[(i - 8) & 15] ^
                p[(i - 14) & 15] ^
                p[i & 15], 1);

        uint32_t t =
            sha1__rol(a, 5) + f + e + 0x8F1BBCDC + p[i & 15];

        e = d;
        d = c;
        c = sha1__rol(b, 30);
        b = a;
        a = t;
    }


    /*
     * Rounds 60-79
     */
    for (i = 60; i < 80; i++)
    {
        uint32_t f = b ^ c ^ d;

        p[i & 15] =
            sha1__rol(
                p[(i - 3) & 15] ^
                p[(i - 8) & 15] ^
                p[(i - 14) & 15] ^
                p[i & 15], 1);

        uint32_t t =
            sha1__rol(a, 5) + f + e + 0xCA62C1D6 + p[i & 15];

        e = d;
        d = c;
        c = sha1__rol(b, 30);
        b = a;
        a = t;
    }

    sha1->digest[0] += a;
    sha1->digest[1] += b;
    sha1->digest[2] += c;
    sha1->digest[3] += d;
    sha1->digest[4] += e;
}


/*
 * Reset SHA1 state.
 */
void sha1_reset(SHA1 *sha1)
{
    sha1->digest[0] = 0x67452301;
    sha1->digest[1] = 0xEFCDAB89;
    sha1->digest[2] = 0x98BADCFE;
    sha1->digest[3] = 0x10325476;
    sha1->digest[4] = 0xC3D2E1F0;

    sha1->block_byte_index = 0;
    sha1->byte_count = 0;
}


/*
 * Process one byte.
 *
 * Kept for compatibility with the original interface.
 * Your normal ROM path should go through sha1_process_bytes().
 */
void sha1_process_byte(SHA1 *sha1, uint8_t octet)
{
    sha1->block[sha1->block_byte_index++] = octet;
    ++sha1->byte_count;

    if (sha1->block_byte_index == 64)
    {
        sha1->block_byte_index = 0;
        sha1__process_block(sha1, sha1->block);
    }
}


/*
 * Process arbitrary data.
 *
 * Kept for compatibility with the original interface.
 */
void sha1_process_block(SHA1 *sha1, const void *start, const void *end)
{
    const uint8_t *begin = (const uint8_t *)start;
    const uint8_t *finish = (const uint8_t *)end;

    while (begin != finish)
    {
        sha1_process_byte(sha1, *begin++);
    }
}


/*
 * Process data.
 *
 * Full 64-byte blocks are processed directly from the source buffer.
 * This avoids copying every block into sha1->block.
 *
 * Your ROM path starts with block_byte_index == 0 and uses lengths
 * that are multiples of 64 bytes, so the fast path handles the
 * entire ROM.
 */
void sha1_process_bytes(SHA1 *sha1, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    /*
     * Fast path: process complete blocks directly from the input.
     *
     * Only use this when there isn't already a partial block pending.
     */
    if (sha1->block_byte_index == 0)
    {
        while (len >= 64)
        {
            sha1__process_block(sha1, p);

            sha1->byte_count += 64;

            p += 64;
            len -= 64;
        }
    }

    /*
     * Handle any remainder (or a partial block from a previous call)
     * using the original byte-oriented path.
     */
    while (len--)
        sha1_process_byte(sha1, *p++);
}


/*
 * Finish SHA1 and return the five 32-bit digest words.
 */
const uint32_t* sha1_get_digest(SHA1 *sha1, digest32_t digest)
{
    uint32_t bitCount = (uint32_t)(sha1->byte_count * 8);

    sha1_process_byte(sha1, 0x80);

    while (sha1->block_byte_index != 56)
    {
        sha1_process_byte(sha1, 0);
    }

    // SHA1 length is 64-bit big-endian.
    // Your ROMs are small enough that the upper 32 bits are zero.
    sha1_process_byte(sha1, 0);
    sha1_process_byte(sha1, 0);
    sha1_process_byte(sha1, 0);
    sha1_process_byte(sha1, 0);

    sha1_process_byte(sha1, (uint8_t)(bitCount >> 24));
    sha1_process_byte(sha1, (uint8_t)(bitCount >> 16));
    sha1_process_byte(sha1, (uint8_t)(bitCount >> 8));
    sha1_process_byte(sha1, (uint8_t)bitCount);

    memcpy(digest, sha1->digest, 5 * sizeof(uint32_t));

    return digest;
}


/*
 * Return standard 20-byte SHA1 representation.
 */
const uint8_t* sha1_get_digest_bytes(SHA1 *sha1, digest8_t digest)
{
    digest32_t d32;

    sha1_get_digest(sha1, d32);

    for (int i = 0; i < 5; i++)
    {
        digest[i * 4 + 0] = (uint8_t)(d32[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(d32[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(d32[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)d32[i];
    }

    return digest;
}

#endif
