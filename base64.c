/*
 * Base64 encoder/decoder conforming to RFC 4648
 * (based on Mozilla's nsprpub/lib/libc/src/base64.c)
 *
 * Copyright (C) 2010 Siemens AG
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include <inttypes.h>
#include "base64.h"

static const char base[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void encode3to4(const uint8_t *src, char *dest)
{
    uint32_t b32 = 0;
    int i, j = 18;

    for (i = 0; i < 3; i++) {
        b32 <<= 8;
        b32 |= src[i];
    }
    for (i = 0; i < 4; i++) {
        dest[i] = base[(b32 >> j) & 0x3F];
        j -= 6;
    }
}

static void encode2to4(const uint8_t *src, char *dest)
{
    dest[0] = base[(src[0] >> 2) & 0x3F];
    dest[1] = base[((src[0] & 0x03) << 4) | ((src[1] >> 4) & 0x0F)];
    dest[2] = base[(src[1] & 0x0F) << 2];
    dest[3] = '=';
}

static void encode1to4(const uint8_t *src, char *dest)
{
    dest[0] = base[(src[0] >> 2) & 0x3F];
    dest[1] = base[(src[0] & 0x03) << 4];
    dest[2] = '=';
    dest[3] = '=';
}

/*
 * Encode data in 'src' of length 'srclen' to a base64 string, saving the
 * null-terminated result in 'dest'. The size of the destition buffer must be
 * at least ((srclen + 2) / 3) * 4 + 1.
 */
void base64_encode(const uint8_t *src, size_t srclen, char *dest)
{
    while (srclen >= 3) {
        encode3to4(src, dest);
        src += 3;
        dest += 4;
        srclen -= 3;
    }
    switch (srclen) {
    case 2:
        encode2to4(src, dest);
        dest += 4;
        break;
    case 1:
        encode1to4(src, dest);
        dest += 4;
        break;
    case 0:
        break;
    }
    dest[0] = 0;
}

static int32_t codetovalue(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    } else if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    } else if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    } else if (c == '+') {
        return 62;
    } else if ( c == '/') {
        return 63;
    } else {
        return -1;
    }
}

static int decode4to3 (const char *src, uint8_t *dest)
{
    uint32_t b32 = 0;
    int32_t bits;
    int i;

    for (i = 0; i < 4; i++) {
        bits = codetovalue(src[i]);
        if (bits < 0) {
            return bits;
        }
        b32 <<= 6;
        b32 |= bits;
    }
    dest[0] = (b32 >> 16) & 0xFF;
    dest[1] = (b32 >> 8) & 0xFF;
    dest[2] = b32 & 0xFF;

    return 0;
}

static int decode3to2(const char *src, uint8_t *dest)
{
    uint32_t b32 = 0;
    int32_t bits;

    bits = codetovalue(src[0]);
    if (bits < 0) {
        return bits;
    }
    b32 = (uint32_t)bits;
    b32 <<= 6;

    bits = codetovalue(src[1]);
    if (bits < 0) {
        return bits;
    }
    b32 |= (uint32_t)bits;
    b32 <<= 4;

    bits = codetovalue(src[2]);
    if (bits < 0) {
        return bits;
    }
    b32 |= ((uint32_t)bits) >> 2;

    dest[0] = (b32 >> 8) & 0xFF;
    dest[1] = b32 & 0xFF;

    return 0;
}

static int decode2to1(const char *src, uint8_t *dest)
{
    uint32_t b32;
    int32_t bits;

    bits = codetovalue(src[0]);
    if (bits < 0) {
        return bits;
    }
    b32 = (uint32_t)bits << 2;

    bits = codetovalue(src[1]);
    if (bits < 0) {
        return bits;
    }
    b32 |= ((uint32_t)bits) >> 4;

    dest[0] = b32;

    return 0;
}

/*
 * Convert string 'src' of length 'srclen' from base64 to binary form,
 * saving the result in 'dest'. The size of the destination buffer must be at
 * least srclen * 3 / 4.
 *
 * Returns 0 on success, -1 on conversion error.
 */
int base64_decode(const char *src, size_t srclen, uint8_t *dest)
{
    int ret;

    while (srclen >= 4) {
        ret = decode4to3(src, dest);
        if (ret < 0) {
            return ret;
        }
        src += 4;
        dest += 3;
        srclen -= 4;
    }

    switch (srclen) {
    case 3:
        return decode3to2(src, dest);
    case 2:
        return decode2to1(src, dest);
    case 1:
        return -1;
    default: /* 0 */
        return 0;
    }
}
