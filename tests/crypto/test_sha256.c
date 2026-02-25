/**
 * SHA-256 Test Suite
 * 
 * Tests SHA-256 wrapper implementation with official test vectors.
 */

#include <criterion/criterion.h>
#include <string.h>

#include "buckets.h"
#include "buckets_crypto.h"

/* Test: Empty string */
Test(sha256, empty_string)
{
    u8 hash[BUCKETS_SHA256_DIGEST_LENGTH];
    const u8 expected[BUCKETS_SHA256_DIGEST_LENGTH] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };

    cr_assert_eq(buckets_sha256(hash, "", 0), 0);
    cr_assert(buckets_sha256_verify(hash, expected));
}

/* Test: "abc" */
Test(sha256, abc)
{
    u8 hash[BUCKETS_SHA256_DIGEST_LENGTH];
    const u8 expected[BUCKETS_SHA256_DIGEST_LENGTH] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };

    cr_assert_eq(buckets_sha256(hash, "abc", 3), 0);
    cr_assert(buckets_sha256_verify(hash, expected));
}

/* Test: Longer string */
Test(sha256, long_string)
{
    const char *data = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    u8 hash[BUCKETS_SHA256_DIGEST_LENGTH];
    const u8 expected[BUCKETS_SHA256_DIGEST_LENGTH] = {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
        0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
        0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1
    };

    cr_assert_eq(buckets_sha256(hash, data, strlen(data)), 0);
    cr_assert(buckets_sha256_verify(hash, expected));
}

/* Test: One million 'a' characters */
Test(sha256, million_a)
{
    char *data = malloc(1000000);
    u8 hash[BUCKETS_SHA256_DIGEST_LENGTH];
    const u8 expected[BUCKETS_SHA256_DIGEST_LENGTH] = {
        0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92,
        0x81, 0xa1, 0xc7, 0xe2, 0x84, 0xd7, 0x3e, 0x67,
        0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97, 0x20, 0x0e,
        0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0
    };

    memset(data, 'a', 1000000);
    cr_assert_eq(buckets_sha256(hash, data, 1000000), 0);
    cr_assert(buckets_sha256_verify(hash, expected));
    free(data);
}

/* Test: Hex output */
Test(sha256, hex_output)
{
    char hexhash[65]; /* 32 bytes * 2 + null */
    const char *expected = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

    cr_assert_eq(buckets_sha256_hex(hexhash, "", 0), 0);
    cr_assert_str_eq(hexhash, expected);
}

/* Test: Verify function */
Test(sha256, verify)
{
    u8 hash1[BUCKETS_SHA256_DIGEST_LENGTH];
    u8 hash2[BUCKETS_SHA256_DIGEST_LENGTH];
    u8 hash3[BUCKETS_SHA256_DIGEST_LENGTH];

    buckets_sha256(hash1, "test", 4);
    buckets_sha256(hash2, "test", 4);
    buckets_sha256(hash3, "TEST", 4);

    /* Same input should match */
    cr_assert(buckets_sha256_verify(hash1, hash2));

    /* Different input should not match */
    cr_assert_not(buckets_sha256_verify(hash1, hash3));
}

/* Test: NULL input validation */
Test(sha256, null_inputs)
{
    u8 hash[BUCKETS_SHA256_DIGEST_LENGTH];

    /* NULL output */
    cr_assert_eq(buckets_sha256(NULL, "test", 4), -1);
    
    /* NULL data with non-zero length */
    cr_assert_eq(buckets_sha256(hash, NULL, 10), -1);
}

/* Test: Zero-length data */
Test(sha256, zero_length)
{
    u8 hash[BUCKETS_SHA256_DIGEST_LENGTH];

    /* Zero length with non-NULL pointer */
    cr_assert_eq(buckets_sha256(hash, "ignored", 0), 0);
}

/* Test: Self-test */
Test(sha256, selftest)
{
    cr_assert_eq(buckets_sha256_selftest(), 0);
}

/* Test: Deterministic output */
Test(sha256, deterministic)
{
    const char *data = "buckets object storage";
    u8 hash1[BUCKETS_SHA256_DIGEST_LENGTH];
    u8 hash2[BUCKETS_SHA256_DIGEST_LENGTH];
    u8 hash3[BUCKETS_SHA256_DIGEST_LENGTH];

    /* Hash same data multiple times */
    cr_assert_eq(buckets_sha256(hash1, data, strlen(data)), 0);
    cr_assert_eq(buckets_sha256(hash2, data, strlen(data)), 0);
    cr_assert_eq(buckets_sha256(hash3, data, strlen(data)), 0);

    /* All should match */
    cr_assert(buckets_sha256_verify(hash1, hash2));
    cr_assert(buckets_sha256_verify(hash2, hash3));
    cr_assert(buckets_sha256_verify(hash1, hash3));
}

/* Test: Different data produces different hashes */
Test(sha256, uniqueness)
{
    u8 hash1[BUCKETS_SHA256_DIGEST_LENGTH];
    u8 hash2[BUCKETS_SHA256_DIGEST_LENGTH];

    cr_assert_eq(buckets_sha256(hash1, "data1", 5), 0);
    cr_assert_eq(buckets_sha256(hash2, "data2", 5), 0);

    cr_assert_not(buckets_sha256_verify(hash1, hash2));
}

/* Test: Comparison with BLAKE2b (different algorithms produce different hashes) */
Test(sha256, compare_blake2b)
{
    const char *data = "test data";
    u8 sha_hash[BUCKETS_SHA256_DIGEST_LENGTH];
    u8 blake_hash[BUCKETS_BLAKE2B_256_OUTBYTES];

    cr_assert_eq(buckets_sha256(sha_hash, data, strlen(data)), 0);
    cr_assert_eq(buckets_blake2b_256(blake_hash, data, strlen(data)), 0);

    /* SHA-256 and BLAKE2b-256 should produce different hashes */
    cr_assert_not(buckets_sha256_verify(sha_hash, blake_hash));
}
