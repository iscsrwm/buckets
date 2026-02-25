/**
 * SHA-256 Implementation (OpenSSL Wrapper)
 * 
 * SHA-256 is the industry-standard cryptographic hash function
 * used for S3 compatibility, ETags, and AWS checksums.
 * 
 * This is a thin wrapper around OpenSSL's optimized implementation.
 */

#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

#include "buckets.h"
#include "buckets_crypto.h"

int buckets_sha256(void *out, const void *data, size_t datalen)
{
    if (!out) {
        return -1;
    }

    if (datalen > 0 && !data) {
        return -1;
    }

    /* OpenSSL SHA256() does all the work */
    if (!SHA256((const unsigned char *)data, datalen, (unsigned char *)out)) {
        return -1;
    }

    return 0;
}

int buckets_sha256_hex(char *out, const void *data, size_t datalen)
{
    u8 hash[BUCKETS_SHA256_DIGEST_LENGTH];
    size_t i;

    if (!out) {
        return -1;
    }

    if (buckets_sha256(hash, data, datalen) < 0) {
        return -1;
    }

    /* Convert to hex */
    for (i = 0; i < BUCKETS_SHA256_DIGEST_LENGTH; i++) {
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    }

    memset(hash, 0, sizeof(hash));
    return 0;
}

bool buckets_sha256_verify(const void *a, const void *b)
{
    const u8 *aa = (const u8 *)a;
    const u8 *bb = (const u8 *)b;
    u8 result = 0;
    size_t i;

    if (!a || !b) {
        return false;
    }

    /* Constant-time comparison */
    for (i = 0; i < BUCKETS_SHA256_DIGEST_LENGTH; i++) {
        result |= aa[i] ^ bb[i];
    }

    return result == 0;
}

int buckets_sha256_selftest(void)
{
    /* Test vector: empty string */
    const u8 expected[BUCKETS_SHA256_DIGEST_LENGTH] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };

    u8 hash[BUCKETS_SHA256_DIGEST_LENGTH];

    if (buckets_sha256(hash, "", 0) < 0) {
        return -1;
    }

    if (!buckets_sha256_verify(hash, expected)) {
        buckets_error("SHA-256 self-test failed");
        return -1;
    }

    buckets_info("SHA-256 self-test passed");
    return 0;
}
