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
#include <stddef.h>

void base64_encode(const uint8_t *src, size_t srclen, char *dest);
int base64_decode(const char *src, size_t srclen, uint8_t *dest);
