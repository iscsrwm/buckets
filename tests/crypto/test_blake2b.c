/**
 * BLAKE2b Test Suite
 * 
 * Tests BLAKE2b implementation with official test vectors from RFC 7693
 * and the BLAKE2 reference implementation.
 */

#include <criterion/criterion.h>
#include <string.h>

#include "buckets.h"
#include "buckets_crypto.h"

/* Test: Empty string (512-bit) */
Test(blake2b, empty_string_512)
{
    u8 hash[BUCKETS_BLAKE2B_OUTBYTES];
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

    cr_assert_eq(buckets_blake2b_512(hash, "", 0), 0);
    cr_assert(buckets_blake2b_verify(hash, expected, BUCKETS_BLAKE2B_OUTBYTES));
}

/* Test: Empty string (256-bit) */
Test(blake2b, empty_string_256)
{
    u8 hash[BUCKETS_BLAKE2B_256_OUTBYTES];
    const u8 expected[BUCKETS_BLAKE2B_256_OUTBYTES] = {
        0x0e, 0x57, 0x51, 0xc0, 0x26, 0xe5, 0x43, 0xb2,
        0xe8, 0xab, 0x2e, 0xb0, 0x60, 0x99, 0xda, 0xa1,
        0xd1, 0xe5, 0xdf, 0x47, 0x77, 0x8f, 0x77, 0x87,
        0xfa, 0xab, 0x45, 0xcd, 0xf1, 0x2f, 0xe3, 0xa8
    };

    cr_assert_eq(buckets_blake2b_256(hash, "", 0), 0);
    cr_assert(buckets_blake2b_verify(hash, expected, BUCKETS_BLAKE2B_256_OUTBYTES));
}

/* Test: "abc" */
Test(blake2b, abc)
{
    u8 hash[BUCKETS_BLAKE2B_OUTBYTES];
    const u8 expected[BUCKETS_BLAKE2B_OUTBYTES] = {
        0xba, 0x80, 0xa5, 0x3f, 0x98, 0x1c, 0x4d, 0x0d,
        0x6a, 0x27, 0x97, 0xb6, 0x9f, 0x12, 0xf6, 0xe9,
        0x4c, 0x21, 0x2f, 0x14, 0x68, 0x5a, 0xc4, 0xb7,
        0x4b, 0x12, 0xbb, 0x6f, 0xdb, 0xff, 0xa2, 0xd1,
        0x7d, 0x87, 0xc5, 0x39, 0x2a, 0xab, 0x79, 0x2d,
        0xc2, 0x52, 0xd5, 0xde, 0x45, 0x33, 0xcc, 0x95,
        0x18, 0xd3, 0x8a, 0xa8, 0xdb, 0xf1, 0x92, 0x5a,
        0xb9, 0x23, 0x86, 0xed, 0xd4, 0x00, 0x99, 0x23
    };

    cr_assert_eq(buckets_blake2b_512(hash, "abc", 3), 0);
    cr_assert(buckets_blake2b_verify(hash, expected, BUCKETS_BLAKE2B_OUTBYTES));
}

/* Test: Longer string */
Test(blake2b, long_string)
{
    const char *data = "The quick brown fox jumps over the lazy dog";
    u8 hash[BUCKETS_BLAKE2B_OUTBYTES];

    /* Just verify it hashes successfully and is deterministic */
    cr_assert_eq(buckets_blake2b_512(hash, data, strlen(data)), 0);
    
    /* Hash again and verify deterministic */
    u8 hash2[BUCKETS_BLAKE2B_OUTBYTES];
    cr_assert_eq(buckets_blake2b_512(hash2, data, strlen(data)), 0);
    cr_assert(buckets_blake2b_verify(hash, hash2, BUCKETS_BLAKE2B_OUTBYTES));
}

/* Test: Incremental hashing */
Test(blake2b, incremental)
{
    buckets_blake2b_ctx_t ctx;
    u8 hash[BUCKETS_BLAKE2B_OUTBYTES];
    const u8 expected[BUCKETS_BLAKE2B_OUTBYTES] = {
        0xba, 0x80, 0xa5, 0x3f, 0x98, 0x1c, 0x4d, 0x0d,
        0x6a, 0x27, 0x97, 0xb6, 0x9f, 0x12, 0xf6, 0xe9,
        0x4c, 0x21, 0x2f, 0x14, 0x68, 0x5a, 0xc4, 0xb7,
        0x4b, 0x12, 0xbb, 0x6f, 0xdb, 0xff, 0xa2, 0xd1,
        0x7d, 0x87, 0xc5, 0x39, 0x2a, 0xab, 0x79, 0x2d,
        0xc2, 0x52, 0xd5, 0xde, 0x45, 0x33, 0xcc, 0x95,
        0x18, 0xd3, 0x8a, 0xa8, 0xdb, 0xf1, 0x92, 0x5a,
        0xb9, 0x23, 0x86, 0xed, 0xd4, 0x00, 0x99, 0x23
    };

    /* Hash "abc" in parts: "a" + "b" + "c" */
    cr_assert_eq(buckets_blake2b_init(&ctx, BUCKETS_BLAKE2B_OUTBYTES), 0);
    cr_assert_eq(buckets_blake2b_update(&ctx, "a", 1), 0);
    cr_assert_eq(buckets_blake2b_update(&ctx, "b", 1), 0);
    cr_assert_eq(buckets_blake2b_update(&ctx, "c", 1), 0);
    cr_assert_eq(buckets_blake2b_final(&ctx, hash, BUCKETS_BLAKE2B_OUTBYTES), 0);

    cr_assert(buckets_blake2b_verify(hash, expected, BUCKETS_BLAKE2B_OUTBYTES));
}

/* Test: Keyed hashing (MAC mode) */
Test(blake2b, keyed_hash)
{
    const u8 key[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    u8 hash[BUCKETS_BLAKE2B_OUTBYTES];

    /* Verify keyed hash works and is deterministic */
    cr_assert_eq(buckets_blake2b(hash, BUCKETS_BLAKE2B_OUTBYTES,
                                  "test", 4, key, sizeof(key)), 0);
    
    /* Hash again with same key */
    u8 hash2[BUCKETS_BLAKE2B_OUTBYTES];
    cr_assert_eq(buckets_blake2b(hash2, BUCKETS_BLAKE2B_OUTBYTES,
                                  "test", 4, key, sizeof(key)), 0);
    cr_assert(buckets_blake2b_verify(hash, hash2, BUCKETS_BLAKE2B_OUTBYTES));
    
    /* Different key should produce different hash */
    u8 key2[32];
    memset(key2, 0xFF, sizeof(key2));
    u8 hash3[BUCKETS_BLAKE2B_OUTBYTES];
    cr_assert_eq(buckets_blake2b(hash3, BUCKETS_BLAKE2B_OUTBYTES,
                                  "test", 4, key2, sizeof(key2)), 0);
    cr_assert_not(buckets_blake2b_verify(hash, hash3, BUCKETS_BLAKE2B_OUTBYTES));
}

/* Test: Different output sizes */
Test(blake2b, variable_output_size)
{
    u8 hash16[16];
    u8 hash32[32];
    u8 hash48[48];

    /* 16-byte hash */
    cr_assert_eq(buckets_blake2b(hash16, 16, "test", 4, NULL, 0), 0);
    
    /* 32-byte hash */
    cr_assert_eq(buckets_blake2b(hash32, 32, "test", 4, NULL, 0), 0);
    
    /* 48-byte hash */
    cr_assert_eq(buckets_blake2b(hash48, 48, "test", 4, NULL, 0), 0);

    /* Different sizes should produce different hashes */
    cr_assert_neq(memcmp(hash16, hash32, 16), 0);
    cr_assert_neq(memcmp(hash32, hash48, 32), 0);
}

/* Test: Hex output */
Test(blake2b, hex_output)
{
    char hexhash[65]; /* 32 bytes * 2 + null */
    const char *expected = "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8";

    cr_assert_eq(buckets_blake2b_hex(hexhash, 32, "", 0), 0);
    cr_assert_str_eq(hexhash, expected);
}

/* Test: Verify function (constant-time comparison) */
Test(blake2b, verify)
{
    u8 hash1[32];
    u8 hash2[32];
    u8 hash3[32];

    buckets_blake2b_256(hash1, "test", 4);
    buckets_blake2b_256(hash2, "test", 4);
    buckets_blake2b_256(hash3, "TEST", 4);

    /* Same input should match */
    cr_assert(buckets_blake2b_verify(hash1, hash2, 32));

    /* Different input should not match */
    cr_assert_not(buckets_blake2b_verify(hash1, hash3, 32));
}

/* Test: Large data (>1 block) */
Test(blake2b, large_data)
{
    char data[256];
    u8 hash[BUCKETS_BLAKE2B_OUTBYTES];

    /* Fill with pattern */
    for (int i = 0; i < 256; i++) {
        data[i] = (char)(i & 0xFF);
    }

    cr_assert_eq(buckets_blake2b_512(hash, data, sizeof(data)), 0);
    
    /* Should produce deterministic hash */
    u8 hash2[BUCKETS_BLAKE2B_OUTBYTES];
    cr_assert_eq(buckets_blake2b_512(hash2, data, sizeof(data)), 0);
    cr_assert(buckets_blake2b_verify(hash, hash2, BUCKETS_BLAKE2B_OUTBYTES));
}

/* Test: Multiple blocks (exactly 2 blocks = 256 bytes) */
Test(blake2b, two_blocks)
{
    char data[256];
    u8 hash[BUCKETS_BLAKE2B_OUTBYTES];

    memset(data, 'A', sizeof(data));

    cr_assert_eq(buckets_blake2b_512(hash, data, sizeof(data)), 0);
    
    /* Should be deterministic */
    u8 hash2[BUCKETS_BLAKE2B_OUTBYTES];
    cr_assert_eq(buckets_blake2b_512(hash2, data, sizeof(data)), 0);
    cr_assert(buckets_blake2b_verify(hash, hash2, BUCKETS_BLAKE2B_OUTBYTES));
}

/* Test: NULL input validation */
Test(blake2b, null_inputs)
{
    u8 hash[BUCKETS_BLAKE2B_OUTBYTES];
    buckets_blake2b_ctx_t ctx;

    /* NULL output */
    cr_assert_eq(buckets_blake2b(NULL, 64, "test", 4, NULL, 0), -1);
    
    /* Invalid output size */
    cr_assert_eq(buckets_blake2b(hash, 0, "test", 4, NULL, 0), -1);
    cr_assert_eq(buckets_blake2b(hash, 65, "test", 4, NULL, 0), -1);
    
    /* NULL context */
    cr_assert_eq(buckets_blake2b_init(NULL, 64), -1);
    cr_assert_eq(buckets_blake2b_update(NULL, "test", 4), -1);
    cr_assert_eq(buckets_blake2b_final(NULL, hash, 64), -1);
    
    /* NULL data with non-zero length */
    cr_assert_eq(buckets_blake2b_init(&ctx, 64), 0);
    cr_assert_eq(buckets_blake2b_update(&ctx, NULL, 10), -1);
}

/* Test: Zero-length data */
Test(blake2b, zero_length)
{
    u8 hash[BUCKETS_BLAKE2B_OUTBYTES];
    buckets_blake2b_ctx_t ctx;

    /* One-shot with zero length */
    cr_assert_eq(buckets_blake2b_512(hash, "ignored", 0), 0);
    
    /* Incremental with zero length update (should be no-op) */
    cr_assert_eq(buckets_blake2b_init(&ctx, 64), 0);
    cr_assert_eq(buckets_blake2b_update(&ctx, "test", 0), 0);
    cr_assert_eq(buckets_blake2b_final(&ctx, hash, 64), 0);
}

/* Test: Self-test */
Test(blake2b, selftest)
{
    cr_assert_eq(buckets_blake2b_selftest(), 0);
}

/* Test: Deterministic output */
Test(blake2b, deterministic)
{
    const char *data = "buckets object storage";
    u8 hash1[BUCKETS_BLAKE2B_OUTBYTES];
    u8 hash2[BUCKETS_BLAKE2B_OUTBYTES];
    u8 hash3[BUCKETS_BLAKE2B_OUTBYTES];

    /* Hash same data multiple times */
    cr_assert_eq(buckets_blake2b_512(hash1, data, strlen(data)), 0);
    cr_assert_eq(buckets_blake2b_512(hash2, data, strlen(data)), 0);
    cr_assert_eq(buckets_blake2b_512(hash3, data, strlen(data)), 0);

    /* All should match */
    cr_assert(buckets_blake2b_verify(hash1, hash2, BUCKETS_BLAKE2B_OUTBYTES));
    cr_assert(buckets_blake2b_verify(hash2, hash3, BUCKETS_BLAKE2B_OUTBYTES));
    cr_assert(buckets_blake2b_verify(hash1, hash3, BUCKETS_BLAKE2B_OUTBYTES));
}

/* Test: Different data produces different hashes */
Test(blake2b, uniqueness)
{
    u8 hash1[BUCKETS_BLAKE2B_OUTBYTES];
    u8 hash2[BUCKETS_BLAKE2B_OUTBYTES];

    cr_assert_eq(buckets_blake2b_512(hash1, "data1", 5), 0);
    cr_assert_eq(buckets_blake2b_512(hash2, "data2", 5), 0);

    cr_assert_not(buckets_blake2b_verify(hash1, hash2, BUCKETS_BLAKE2B_OUTBYTES));
}
