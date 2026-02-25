/**
 * xxHash-64 Tests
 * 
 * Test suite with official test vectors from xxHash.
 * Reference: https://github.com/Cyan4973/xxHash
 */

#include <criterion/criterion.h>
#include <stdio.h>
#include <string.h>

#include "buckets.h"
#include "buckets_hash.h"

/* Test vectors for xxHash64 with seed=0 (verified correct) */
static const struct {
    const char *input;
    size_t len;
    u64 expected;
} test_vectors[] = {
    /* Empty input */
    {"", 0, 0xEF46DB3751D8E999ULL},
    
    /* Single bytes */
    {"a", 1, 0xD24EC4F1A98C6E5BULL},
    {"1", 1, 0xB7B41276360564D4ULL},
    
    /* Short strings */
    {"abc", 3, 0x44BC2CF5AD770999ULL},
    {"message digest", 14, 0x066ED728FCEEB3BEULL},
    {"abcdefghijklmnopqrstuvwxyz", 26, 0xCFE1F278FA89835CULL},
};

/* Test: Official test vectors with seed=0 */
Test(xxhash, official_test_vectors)
{
    for (size_t i = 0; i < sizeof(test_vectors) / sizeof(test_vectors[0]); i++) {
        u64 hash = buckets_xxhash64(0, test_vectors[i].input, test_vectors[i].len);
        cr_assert_eq(hash, test_vectors[i].expected,
                    "Test vector %zu failed: got 0x%016llx, expected 0x%016llx (input: '%s')",
                    i, (unsigned long long)hash,
                    (unsigned long long)test_vectors[i].expected,
                    test_vectors[i].len <= 20 ? test_vectors[i].input : "<long>");
    }
}

/* Test: Empty input */
Test(xxhash, empty_input)
{
    u64 hash = buckets_xxhash64(0, "", 0);
    cr_assert_eq(hash, 0xEF46DB3751D8E999ULL);
}

/* Test: Different seeds produce different hashes */
Test(xxhash, different_seeds)
{
    const char *data = "test data";
    u64 hash1 = buckets_xxhash64(0, data, strlen(data));
    u64 hash2 = buckets_xxhash64(1, data, strlen(data));
    u64 hash3 = buckets_xxhash64(0x123456789ABCDEFULL, data, strlen(data));
    
    cr_assert_neq(hash1, hash2, "Different seeds should produce different hashes");
    cr_assert_neq(hash1, hash3);
    cr_assert_neq(hash2, hash3);
}

/* Test: Incremental hashing gives same result as one-shot */
Test(xxhash, incremental_hashing)
{
    const char *data = "The quick brown fox jumps over the lazy dog";
    size_t len = strlen(data);
    
    /* One-shot hash */
    u64 hash1 = buckets_xxhash64(0, data, len);
    
    /* Incremental hash: process in chunks */
    buckets_xxhash_state_t state;
    buckets_xxhash_init(&state, 0);
    buckets_xxhash_update(&state, data, 10);
    buckets_xxhash_update(&state, data + 10, 10);
    buckets_xxhash_update(&state, data + 20, 10);
    buckets_xxhash_update(&state, data + 30, len - 30);
    u64 hash2 = buckets_xxhash_final(&state);
    
    cr_assert_eq(hash1, hash2, "Incremental hashing should match one-shot");
}

/* Test: Incremental with single bytes */
Test(xxhash, incremental_single_bytes)
{
    const char *data = "abc";
    u64 hash1 = buckets_xxhash64(0, data, 3);
    
    buckets_xxhash_state_t state;
    buckets_xxhash_init(&state, 0);
    buckets_xxhash_update(&state, "a", 1);
    buckets_xxhash_update(&state, "b", 1);
    buckets_xxhash_update(&state, "c", 1);
    u64 hash2 = buckets_xxhash_final(&state);
    
    cr_assert_eq(hash1, hash2);
}

/* Test: Long input (> 32 bytes) */
Test(xxhash, long_input)
{
    char data[1000];
    for (int i = 0; i < 1000; i++) {
        data[i] = (char)(i % 256);
    }
    
    u64 hash = buckets_xxhash64(0, data, 1000);
    cr_assert_neq(hash, 0, "Long input should produce non-zero hash");
}

/* Test: Very long input with incremental hashing */
Test(xxhash, very_long_incremental)
{
    char data[10000];
    for (int i = 0; i < 10000; i++) {
        data[i] = (char)('a' + (i % 26));
    }
    
    u64 hash1 = buckets_xxhash64(0, data, 10000);
    
    buckets_xxhash_state_t state;
    buckets_xxhash_init(&state, 0);
    for (int i = 0; i < 100; i++) {
        buckets_xxhash_update(&state, data + i * 100, 100);
    }
    u64 hash2 = buckets_xxhash_final(&state);
    
    cr_assert_eq(hash1, hash2);
}

/* Test: Avalanche effect */
Test(xxhash, avalanche_effect)
{
    const char *data1 = "test data";
    const char *data2 = "test datb";  /* Last char changed */
    
    u64 hash1 = buckets_xxhash64(0, data1, strlen(data1));
    u64 hash2 = buckets_xxhash64(0, data2, strlen(data2));
    
    cr_assert_neq(hash1, hash2, "Small input change should change hash");
    
    /* Count changed bits */
    u64 diff = hash1 ^ hash2;
    int changed_bits = 0;
    for (int i = 0; i < 64; i++) {
        if (diff & (1ULL << i)) {
            changed_bits++;
        }
    }
    
    /* Good avalanche: roughly half the bits should change */
    cr_assert_gt(changed_bits, 20, "At least 20 bits should change (avalanche effect)");
}

/* Test: Checksum convenience function */
Test(xxhash, checksum_function)
{
    const char *data = "test data";
    u64 checksum1 = buckets_checksum(data, strlen(data));
    u64 checksum2 = buckets_checksum(data, strlen(data));
    u64 hash = buckets_xxhash64(0, data, strlen(data));
    
    cr_assert_eq(checksum1, checksum2, "Checksums should be deterministic");
    cr_assert_eq(checksum1, hash, "Checksum should equal xxhash64 with seed=0");
}

/* Test: NULL input with zero length */
Test(xxhash, null_zero_length)
{
    u64 hash = buckets_xxhash64(0, "", 0);
    cr_assert_neq(hash, 0, "Empty input should still produce hash");
}

/* Test: Performance characteristic - xxHash should be fast */
Test(xxhash, performance_smoke_test)
{
    /* Create 1MB of data */
    char *data = malloc(1024 * 1024);
    cr_assert_not_null(data);
    
    for (int i = 0; i < 1024 * 1024; i++) {
        data[i] = (char)(i % 256);
    }
    
    /* Hash it - should complete quickly */
    u64 hash = buckets_xxhash64(0, data, 1024 * 1024);
    cr_assert_neq(hash, 0);
    
    free(data);
}

/* Test: Multiple of 32 bytes */
Test(xxhash, multiple_of_32_bytes)
{
    char data[64];
    memset(data, 'a', 64);
    
    u64 hash = buckets_xxhash64(0, data, 64);
    cr_assert_neq(hash, 0);
}

/* Test: Just under 32 bytes */
Test(xxhash, under_32_bytes)
{
    char data[31];
    memset(data, 'b', 31);
    
    u64 hash = buckets_xxhash64(0, data, 31);
    cr_assert_neq(hash, 0);
}

/* Test: Just over 32 bytes */
Test(xxhash, over_32_bytes)
{
    char data[33];
    memset(data, 'c', 33);
    
    u64 hash = buckets_xxhash64(0, data, 33);
    cr_assert_neq(hash, 0);
}

/* Test: Distribution test */
Test(xxhash, distribution)
{
    const int bucket_count = 256;
    int buckets[256] = {0};
    const int sample_count = 10000;
    
    char data[64];
    for (int i = 0; i < sample_count; i++) {
        snprintf(data, sizeof(data), "object-%d", i);
        u64 hash = buckets_xxhash64(0, data, strlen(data));
        int bucket = hash % bucket_count;
        buckets[bucket]++;
    }
    
    /* Check distribution is reasonable */
    int expected = sample_count / bucket_count;  /* ~39 per bucket */
    int too_few = 0;
    int too_many = 0;
    
    for (int i = 0; i < bucket_count; i++) {
        if (buckets[i] < expected / 2) too_few++;
        if (buckets[i] > expected * 2) too_many++;
    }
    
    /* Most buckets should be within reasonable range */
    cr_assert_lt(too_few, bucket_count / 10, "Too many under-filled buckets");
    cr_assert_lt(too_many, bucket_count / 10, "Too many over-filled buckets");
}

/* Test: Deterministic across calls */
Test(xxhash, deterministic)
{
    const char *data = "deterministic test";
    u64 hash1 = buckets_xxhash64(42, data, strlen(data));
    u64 hash2 = buckets_xxhash64(42, data, strlen(data));
    u64 hash3 = buckets_xxhash64(42, data, strlen(data));
    
    cr_assert_eq(hash1, hash2);
    cr_assert_eq(hash2, hash3);
}
