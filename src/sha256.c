/*
 * PRTERM - CB & Amateur Radio Terminal
 * sha256.c - FIPS 180-4 SHA-256.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "prterm_compat.h"

#include "sha256.h"

#include <string.h>

#define ROTR(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define SHR(x, n)   ((x) >> (n))

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (ROTR(x, 2)  ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x)  (ROTR(x, 6)  ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7)  ^ ROTR(x, 18) ^ SHR(x, 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))

static const unsigned int K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void pr_sha256_transform(pr_sha256_ctx *ctx, const unsigned char data[64])
{
    unsigned int m[64];
    for (int i = 0, j = 0; i < 16; i++, j += 4) {
        m[i] = ((unsigned int)data[j]     << 24) |
               ((unsigned int)data[j + 1] << 16) |
               ((unsigned int)data[j + 2] <<  8) |
               ((unsigned int)data[j + 3]);
    }
    for (int i = 16; i < 64; i++)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

    unsigned int a = ctx->state[0], b = ctx->state[1], c = ctx->state[2],
                 d = ctx->state[3], e = ctx->state[4], f = ctx->state[5],
                 g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        unsigned int t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        unsigned int t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

void pr_sha256_init(pr_sha256_ctx *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

void pr_sha256_update(pr_sha256_ctx *ctx, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = p[i];
        if (ctx->datalen == 64) {
            pr_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void pr_sha256_final(pr_sha256_ctx *ctx, unsigned char digest[PR_SHA256_DIGEST_LEN])
{
    size_t i = ctx->datalen;

    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64)
            ctx->data[i++] = 0x00;
        pr_sha256_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56)
        ctx->data[i++] = 0x00;

    ctx->bitlen += (unsigned long long)ctx->datalen * 8;
    ctx->data[63] = (unsigned char)(ctx->bitlen);
    ctx->data[62] = (unsigned char)(ctx->bitlen >> 8);
    ctx->data[61] = (unsigned char)(ctx->bitlen >> 16);
    ctx->data[60] = (unsigned char)(ctx->bitlen >> 24);
    ctx->data[59] = (unsigned char)(ctx->bitlen >> 32);
    ctx->data[58] = (unsigned char)(ctx->bitlen >> 40);
    ctx->data[57] = (unsigned char)(ctx->bitlen >> 48);
    ctx->data[56] = (unsigned char)(ctx->bitlen >> 56);
    pr_sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; i++) {
        digest[i]      = (unsigned char)((ctx->state[0] >> (24 - i * 8)) & 0xff);
        digest[i + 4]  = (unsigned char)((ctx->state[1] >> (24 - i * 8)) & 0xff);
        digest[i + 8]  = (unsigned char)((ctx->state[2] >> (24 - i * 8)) & 0xff);
        digest[i + 12] = (unsigned char)((ctx->state[3] >> (24 - i * 8)) & 0xff);
        digest[i + 16] = (unsigned char)((ctx->state[4] >> (24 - i * 8)) & 0xff);
        digest[i + 20] = (unsigned char)((ctx->state[5] >> (24 - i * 8)) & 0xff);
        digest[i + 24] = (unsigned char)((ctx->state[6] >> (24 - i * 8)) & 0xff);
        digest[i + 28] = (unsigned char)((ctx->state[7] >> (24 - i * 8)) & 0xff);
    }
}

void pr_sha256(const void *data, size_t len, unsigned char digest[PR_SHA256_DIGEST_LEN])
{
    pr_sha256_ctx ctx;
    pr_sha256_init(&ctx);
    pr_sha256_update(&ctx, data, len);
    pr_sha256_final(&ctx, digest);
}

void pr_sha256_to_hex(const unsigned char digest[PR_SHA256_DIGEST_LEN],
                      char out[PR_SHA256_HEX_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < PR_SHA256_DIGEST_LEN; i++) {
        out[i * 2]     = hex[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[PR_SHA256_HEX_LEN] = '\0';
}

void pr_sha256_hex(const void *data, size_t len, char out[PR_SHA256_HEX_LEN + 1])
{
    unsigned char digest[PR_SHA256_DIGEST_LEN];
    pr_sha256(data, len, digest);
    pr_sha256_to_hex(digest, out);
}
