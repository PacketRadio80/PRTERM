/*
 * PRTERM - CB & Amateur Radio Terminal
 * sha256.h - FIPS 180-4 SHA-256, used for password hashing and CSRF tokens.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PRTERM_SHA256_H
#define PRTERM_SHA256_H

#include <stddef.h>

#define PR_SHA256_DIGEST_LEN 32
#define PR_SHA256_HEX_LEN    64

typedef struct pr_sha256_ctx {
    unsigned int  state[8];
    unsigned long long bitlen;
    unsigned char data[64];
    size_t        datalen;
} pr_sha256_ctx;

void pr_sha256_init(pr_sha256_ctx *ctx);
void pr_sha256_update(pr_sha256_ctx *ctx, const void *data, size_t len);
void pr_sha256_final(pr_sha256_ctx *ctx, unsigned char digest[PR_SHA256_DIGEST_LEN]);

/* one-shot helpers */
void pr_sha256(const void *data, size_t len, unsigned char digest[PR_SHA256_DIGEST_LEN]);
void pr_sha256_hex(const void *data, size_t len, char out[PR_SHA256_HEX_LEN + 1]);
void pr_sha256_to_hex(const unsigned char digest[PR_SHA256_DIGEST_LEN],
                      char out[PR_SHA256_HEX_LEN + 1]);

#endif /* PRTERM_SHA256_H */
