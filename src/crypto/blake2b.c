/**
 * BLAKE2b Implementation
 * 
 * BLAKE2b is a cryptographic hash function faster than SHA-256
 * and optimized for 64-bit platforms.
 * 
 * Based on RFC 7693: https://tools.ietf.org/html/rfc7693
 * Reference: https://github.com/BLAKE2/BLAKE2
 */

#include <stdio.h>
#include <string.h>

#include "buckets.h"
#include "buckets_crypto.h"

/* BLAKE2b IV (initialization vector) - first 64 bits of fractional parts of sqrt(primes) */
static const u64 blake2b_iv[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

/* Rotation constants */
static const u8 blake2b_sigma[12][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13 , 0 },
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 }
};

/* Rotate right */
static inline u64 rotr64(u64 w, unsigned c)
{
    return (w >> c) | (w << (64 - c));
}

/* Little-endian byte reading */
static inline u64 load64(const void *src)
{
    const u8 *p = (const u8 *)src;
    return ((u64)p[0] <<  0) |
           ((u64)p[1] <<  8) |
           ((u64)p[2] << 16) |
           ((u64)p[3] << 24) |
           ((u64)p[4] << 32) |
           ((u64)p[5] << 40) |
           ((u64)p[6] << 48) |
           ((u64)p[7] << 56);
}

/* Little-endian byte writing */
static inline void store64(void *dst, u64 w)
{
    u8 *p = (u8 *)dst;
    p[0] = (u8)(w >>  0);
    p[1] = (u8)(w >>  8);
    p[2] = (u8)(w >> 16);
    p[3] = (u8)(w >> 24);
    p[4] = (u8)(w >> 32);
    p[5] = (u8)(w >> 40);
    p[6] = (u8)(w >> 48);
    p[7] = (u8)(w >> 56);
}

/* BLAKE2b mixing function G */
#define G(r, i, a, b, c, d)                     \
    do {                                        \
        a = a + b + m[blake2b_sigma[r][2*i+0]]; \
        d = rotr64(d ^ a, 32);                  \
        c = c + d;                              \
        b = rotr64(b ^ c, 24);                  \
        a = a + b + m[blake2b_sigma[r][2*i+1]]; \
        d = rotr64(d ^ a, 16);                  \
        c = c + d;                              \
        b = rotr64(b ^ c, 63);                  \
    } while (0)

/* BLAKE2b round function */
#define ROUND(r)                     \
    do {                             \
        G(r, 0, v[0], v[4], v[ 8], v[12]); \
        G(r, 1, v[1], v[5], v[ 9], v[13]); \
        G(r, 2, v[2], v[6], v[10], v[14]); \
        G(r, 3, v[3], v[7], v[11], v[15]); \
        G(r, 4, v[0], v[5], v[10], v[15]); \
        G(r, 5, v[1], v[6], v[11], v[12]); \
        G(r, 6, v[2], v[7], v[ 8], v[13]); \
        G(r, 7, v[3], v[4], v[ 9], v[14]); \
    } while (0)

/* Compression function */
static void blake2b_compress(buckets_blake2b_ctx_t *ctx, const u8 block[BUCKETS_BLAKE2B_BLOCKBYTES])
{
    u64 m[16];
    u64 v[16];
    size_t i;

    /* Load message block */
    for (i = 0; i < 16; i++) {
        m[i] = load64(block + i * sizeof(u64));
    }

    /* Initialize work vector */
    for (i = 0; i < 8; i++) {
        v[i] = ctx->h[i];
        v[i + 8] = blake2b_iv[i];
    }

    /* Mix in the byte counters and finalization flag */
    v[12] ^= ctx->t[0];
    v[13] ^= ctx->t[1];
    v[14] ^= ctx->f[0];
    v[15] ^= ctx->f[1];

    /* 12 rounds of mixing */
    ROUND(0);
    ROUND(1);
    ROUND(2);
    ROUND(3);
    ROUND(4);
    ROUND(5);
    ROUND(6);
    ROUND(7);
    ROUND(8);
    ROUND(9);
    ROUND(10);
    ROUND(11);

    /* Update state */
    for (i = 0; i < 8; i++) {
        ctx->h[i] ^= v[i] ^ v[i + 8];
    }
}

int buckets_blake2b_init(buckets_blake2b_ctx_t *ctx, size_t outlen)
{
    buckets_blake2b_param_t P;

    if (!ctx || !outlen || outlen > BUCKETS_BLAKE2B_OUTBYTES) {
        return -1;
    }

    /* Setup parameters */
    memset(&P, 0, sizeof(P));
    P.digest_length = (u8)outlen;
    P.key_length = 0;
    P.fanout = 1;
    P.depth = 1;

    return buckets_blake2b_init_param(ctx, &P);
}

int buckets_blake2b_init_key(buckets_blake2b_ctx_t *ctx, size_t outlen,
                              const void *key, size_t keylen)
{
    buckets_blake2b_param_t P;

    if (!ctx || !outlen || outlen > BUCKETS_BLAKE2B_OUTBYTES) {
        return -1;
    }

    if (!key && keylen > 0) {
        return -1;
    }

    if (keylen > BUCKETS_BLAKE2B_KEYBYTES) {
        return -1;
    }

    /* Setup parameters */
    memset(&P, 0, sizeof(P));
    P.digest_length = (u8)outlen;
    P.key_length = (u8)keylen;
    P.fanout = 1;
    P.depth = 1;

    if (buckets_blake2b_init_param(ctx, &P) < 0) {
        return -1;
    }

    /* Process key as first block if provided */
    if (keylen > 0) {
        u8 block[BUCKETS_BLAKE2B_BLOCKBYTES];
        memset(block, 0, BUCKETS_BLAKE2B_BLOCKBYTES);
        memcpy(block, key, keylen);
        buckets_blake2b_update(ctx, block, BUCKETS_BLAKE2B_BLOCKBYTES);
        /* Secure erase */
        memset(block, 0, BUCKETS_BLAKE2B_BLOCKBYTES);
    }

    return 0;
}

int buckets_blake2b_init_param(buckets_blake2b_ctx_t *ctx,
                                const buckets_blake2b_param_t *param)
{
    const u8 *p;
    size_t i;

    if (!ctx || !param) {
        return -1;
    }

    /* Initialize state with IV */
    memcpy(ctx->h, blake2b_iv, sizeof(ctx->h));

    /* XOR in parameter block */
    p = (const u8 *)param;
    for (i = 0; i < 8; i++) {
        ctx->h[i] ^= load64(p + sizeof(u64) * i);
    }

    ctx->t[0] = 0;
    ctx->t[1] = 0;
    ctx->f[0] = 0;
    ctx->f[1] = 0;
    ctx->buflen = 0;
    ctx->outlen = param->digest_length;

    return 0;
}

int buckets_blake2b_update(buckets_blake2b_ctx_t *ctx,
                            const void *data, size_t datalen)
{
    const u8 *in = (const u8 *)data;
    size_t fill;

    if (!ctx) {
        return -1;
    }

    if (datalen == 0) {
        return 0;
    }

    if (!data) {
        return -1;
    }

    while (datalen > 0) {
        /* How much can we add to current buffer? */
        fill = BUCKETS_BLAKE2B_BLOCKBYTES - ctx->buflen;

        if (datalen > fill) {
            /* Fill buffer completely and compress */
            memcpy(ctx->buf + ctx->buflen, in, fill);
            ctx->t[0] += BUCKETS_BLAKE2B_BLOCKBYTES;
            if (ctx->t[0] < BUCKETS_BLAKE2B_BLOCKBYTES) {
                ctx->t[1]++; /* Carry */
            }
            blake2b_compress(ctx, ctx->buf);
            ctx->buflen = 0;
            in += fill;
            datalen -= fill;
        } else {
            /* Partial fill */
            memcpy(ctx->buf + ctx->buflen, in, datalen);
            ctx->buflen += datalen;
            break;
        }
    }

    return 0;
}

int buckets_blake2b_final(buckets_blake2b_ctx_t *ctx, void *out, size_t outlen)
{
    u8 buffer[BUCKETS_BLAKE2B_OUTBYTES];
    size_t i;

    if (!ctx || !out) {
        return -1;
    }

    if (outlen < ctx->outlen) {
        return -1;
    }

    /* Pad final block with zeros */
    if (ctx->buflen < BUCKETS_BLAKE2B_BLOCKBYTES) {
        memset(ctx->buf + ctx->buflen, 0,
               BUCKETS_BLAKE2B_BLOCKBYTES - ctx->buflen);
    }

    /* Update byte counter */
    ctx->t[0] += ctx->buflen;
    if (ctx->t[0] < ctx->buflen) {
        ctx->t[1]++; /* Carry */
    }

    /* Set finalization flag */
    ctx->f[0] = (u64)-1;

    /* Final compression */
    blake2b_compress(ctx, ctx->buf);

    /* Output hash */
    for (i = 0; i < 8; i++) {
        store64(buffer + sizeof(u64) * i, ctx->h[i]);
    }

    memcpy(out, buffer, ctx->outlen);
    memset(buffer, 0, sizeof(buffer));
    return 0;
}

int buckets_blake2b(void *out, size_t outlen,
                    const void *data, size_t datalen,
                    const void *key, size_t keylen)
{
    buckets_blake2b_ctx_t ctx;

    if (keylen > 0) {
        if (buckets_blake2b_init_key(&ctx, outlen, key, keylen) < 0) {
            return -1;
        }
    } else {
        if (buckets_blake2b_init(&ctx, outlen) < 0) {
            return -1;
        }
    }

    if (buckets_blake2b_update(&ctx, data, datalen) < 0) {
        return -1;
    }

    if (buckets_blake2b_final(&ctx, out, outlen) < 0) {
        return -1;
    }

    /* Secure erase context */
    memset(&ctx, 0, sizeof(ctx));
    return 0;
}

int buckets_blake2b_256(void *out, const void *data, size_t datalen)
{
    return buckets_blake2b(out, BUCKETS_BLAKE2B_256_OUTBYTES,
                           data, datalen, NULL, 0);
}

int buckets_blake2b_512(void *out, const void *data, size_t datalen)
{
    return buckets_blake2b(out, BUCKETS_BLAKE2B_512_OUTBYTES,
                           data, datalen, NULL, 0);
}

int buckets_blake2b_hex(char *out, size_t outlen,
                        const void *data, size_t datalen)
{
    u8 hash[BUCKETS_BLAKE2B_OUTBYTES];
    size_t i;

    if (!out || outlen > BUCKETS_BLAKE2B_OUTBYTES) {
        return -1;
    }

    if (buckets_blake2b(hash, outlen, data, datalen, NULL, 0) < 0) {
        return -1;
    }

    /* Convert to hex */
    for (i = 0; i < outlen; i++) {
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    }

    memset(hash, 0, sizeof(hash));
    return 0;
}

bool buckets_blake2b_verify(const void *a, const void *b, size_t len)
{
    const u8 *aa = (const u8 *)a;
    const u8 *bb = (const u8 *)b;
    u8 result = 0;
    size_t i;

    if (!a || !b || len == 0) {
        return false;
    }

    /* Constant-time comparison */
    for (i = 0; i < len; i++) {
        result |= aa[i] ^ bb[i];
    }

    return result == 0;
}

int buckets_blake2b_selftest(void)
{
    /* Test vector: empty string */
    const u8 expected[BUCKETS_BLAKE2B_OUTBYTES] = {
        0x78, 0x6a, 0x02, 0xf7, 0x42, 0x01, 0x59, 0x03,
        0xc6, 0xc6, 0xfd, 0x85, 0x25, 0x52, 0xd2, 0x72,
        0x91, 0x2f, 0x47, 0x40, 0xe1, 0x58, 0x47, 0x61,
        0x8a, 0x86, 0xe2, 0x17, 0xf7, 0x1f, 0x54, 0x19,
        0xd2, 0x5e, 0x10, 0x31, 0xaf, 0xee, 0x58, 0x53,
        0x13, 0x89, 0x64, 0x44, 0x93, 0x4e, 0xb0, 0x4b,
        0x90, 0x3a, 0x68, 0x5b, 0x14, 0x48, 0xb7, 0x55,
        0xd5, 0x6f, 0x70, 0x1a, 0xfe, 0x9b, 0xe2, 0xce
    };

    u8 hash[BUCKETS_BLAKE2B_OUTBYTES];

    if (buckets_blake2b_512(hash, "", 0) < 0) {
        return -1;
    }

    if (!buckets_blake2b_verify(hash, expected, BUCKETS_BLAKE2B_OUTBYTES)) {
        buckets_error("BLAKE2b self-test failed");
        return -1;
    }

    buckets_info("BLAKE2b self-test passed");
    return 0;
}
