/**
 * SipHash-2-4 Tests
 * 
 * Test suite with official test vectors from the SipHash paper.
 * Reference: https://131002.net/siphash/
 */

#include <criterion/criterion.h>
#include <stdio.h>
#include <string.h>

#include "buckets.h"
#include "buckets_hash.h"

/* Official test vectors from SipHash paper */
/* Key: 00 01 02 ... 0f */
static const u64 test_key0 = 0x0706050403020100ULL;
static const u64 test_key1 = 0x0f0e0d0c0b0a0908ULL;

/* Expected outputs for inputs of length 0 to 15 bytes */
static const u64 test_vectors[] = {
    0x726fdb47dd0e0e31ULL, /*  0 bytes */
    0x74f839c593dc67fdULL, /*  1 byte  */
    0x0d6c8009d9a94f5aULL, /*  2 bytes */
    0x85676696d7fb7e2dULL, /*  3 bytes */
    0xcf2794e0277187b7ULL, /*  4 bytes */
    0x18765564cd99a68dULL, /*  5 bytes */
    0xcbc9466e58fee3ceULL, /*  6 bytes */
    0xab0200f58b01d137ULL, /*  7 bytes */
    0x93f5f5799a932462ULL, /*  8 bytes */
    0x9e0082df0ba9e4b0ULL, /*  9 bytes */
    0x7a5dbbc594ddb9f3ULL, /* 10 bytes */
    0xf4b32f46226bada7ULL, /* 11 bytes */
    0x751e8fbc860ee5fbULL, /* 12 bytes */
    0x14ea5627c0843d90ULL, /* 13 bytes */
    0xf723ca908e7af2eeULL, /* 14 bytes */
    0xa129ca6149be45e5ULL, /* 15 bytes */
};

/* Test: Official test vectors */
Test(siphash, official_test_vectors)
{
    u8 input[16];
    
    /* Generate input: 00 01 02 03 04 ... */
    for (int i = 0; i < 16; i++) {
        input[i] = (u8)i;
    }
    
    /* Test each length from 0 to 15 */
    for (int len = 0; len < 16; len++) {
        u64 hash = buckets_siphash(test_key0, test_key1, input, len);
        cr_assert_eq(hash, test_vectors[len],
                    "Test vector mismatch at length %d: got 0x%016llx, expected 0x%016llx",
                    len, (unsigned long long)hash, (unsigned long long)test_vectors[len]);
    }
}

/* Test: Empty input */
Test(siphash, empty_input)
{
    u64 hash = buckets_siphash(test_key0, test_key1, "", 0);
    cr_assert_eq(hash, test_vectors[0]);
}

/* Test: Single byte */
Test(siphash, single_byte)
{
    u8 input[] = {0x00};
    u64 hash = buckets_siphash(test_key0, test_key1, input, 1);
    cr_assert_eq(hash, test_vectors[1]);
}

/* Test: 8-byte aligned input */
Test(siphash, eight_byte_input)
{
    u8 input[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    u64 hash = buckets_siphash(test_key0, test_key1, input, 8);
    cr_assert_eq(hash, test_vectors[8]);
}

/* Test: Incremental hashing gives same result as one-shot */
Test(siphash, incremental_hashing)
{
    u8 input[16];
    for (int i = 0; i < 16; i++) {
        input[i] = (u8)i;
    }
    
    /* One-shot hash */
    u64 hash1 = buckets_siphash(test_key0, test_key1, input, 15);
    
    /* Incremental hash: process in chunks */
    buckets_siphash_state_t state;
    buckets_siphash_init(&state, test_key0, test_key1);
    buckets_siphash_update(&state, input, 5);
    buckets_siphash_update(&state, input + 5, 5);
    buckets_siphash_update(&state, input + 10, 5);
    u64 hash2 = buckets_siphash_final(&state);
    
    cr_assert_eq(hash1, hash2);
    cr_assert_eq(hash1, test_vectors[15]);
}

/* Test: Different keys produce different hashes */
Test(siphash, different_keys)
{
    const char *data = "test data";
    u64 hash1 = buckets_siphash(test_key0, test_key1, data, strlen(data));
    u64 hash2 = buckets_siphash(0x1234567890abcdefULL, 0xfedcba0987654321ULL, data, strlen(data));
    
    cr_assert_neq(hash1, hash2, "Different keys should produce different hashes");
}

/* Test: Small changes in input produce different hashes */
Test(siphash, avalanche_effect)
{
    const char *data1 = "test data";
    const char *data2 = "test datb";  /* Last char changed 'a' -> 'b' */
    
    u64 hash1 = buckets_siphash(test_key0, test_key1, data1, strlen(data1));
    u64 hash2 = buckets_siphash(test_key0, test_key1, data2, strlen(data2));
    
    cr_assert_neq(hash1, hash2, "Small input changes should change hash");
}

/* Test: Object to set mapping */
Test(siphash, object_to_set_mapping)
{
    /* Sample deployment ID (16 bytes) */
    u8 deployment_id[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };
    
    const char *object = "bucket/object.txt";
    i32 set_count = 16;
    
    i32 set_idx = buckets_hash_object_to_set(object, deployment_id, set_count);
    cr_assert_geq(set_idx, 0, "Set index should be >= 0");
    cr_assert_lt(set_idx, set_count, "Set index should be < set_count");
}

/* Test: Same object always maps to same set */
Test(siphash, deterministic_placement)
{
    u8 deployment_id[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };
    
    const char *object = "mybucket/myobject";
    i32 set_count = 8;
    
    i32 set1 = buckets_hash_object_to_set(object, deployment_id, set_count);
    i32 set2 = buckets_hash_object_to_set(object, deployment_id, set_count);
    i32 set3 = buckets_hash_object_to_set(object, deployment_id, set_count);
    
    cr_assert_eq(set1, set2);
    cr_assert_eq(set2, set3);
}

/* Test: Different objects distribute across sets */
Test(siphash, distribution_across_sets)
{
    u8 deployment_id[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };
    
    i32 set_count = 8;
    i32 set_counts[8] = {0};
    
    /* Hash 1000 objects */
    char object[64];
    for (int i = 0; i < 1000; i++) {
        snprintf(object, sizeof(object), "bucket/object-%d", i);
        i32 set = buckets_hash_object_to_set(object, deployment_id, set_count);
        cr_assert_geq(set, 0);
        cr_assert_lt(set, set_count);
        set_counts[set]++;
    }
    
    /* Check distribution is reasonable (each set should have some objects) */
    for (int i = 0; i < set_count; i++) {
        cr_assert_gt(set_counts[i], 0, "Set %d should have at least one object", i);
        /* With good distribution, each set should have roughly 125 ± 50 objects */
        cr_assert_gt(set_counts[i], 50, "Set %d has too few objects: %d", i, set_counts[i]);
        cr_assert_lt(set_counts[i], 200, "Set %d has too many objects: %d", i, set_counts[i]);
    }
}

/* Test: UUID string to SipHash key */
Test(siphash, uuid_string_to_key)
{
    const char *uuid = "550e8400-e29b-41d4-a716-446655440000";
    u64 k0, k1;
    
    buckets_error_t err = buckets_uuid_str_to_siphash_key(uuid, &k0, &k1);
    cr_assert_eq(err, BUCKETS_OK);
    
    /* Verify we can use the key */
    u64 hash = buckets_siphash(k0, k1, "test", 4);
    cr_assert_neq(hash, 0);
}

/* Test: Invalid UUID string */
Test(siphash, invalid_uuid_string)
{
    const char *bad_uuid = "not-a-uuid";
    u64 k0, k1;
    
    buckets_error_t err = buckets_uuid_str_to_siphash_key(bad_uuid, &k0, &k1);
    cr_assert_neq(err, BUCKETS_OK);
}

/* Test: SipHash-128 produces 128-bit output */
Test(siphash, siphash128_output)
{
    u8 input[] = {0x00, 0x01, 0x02, 0x03};
    u8 out[16];
    
    buckets_siphash128(test_key0, test_key1, input, 4, out);
    
    /* Convert to two 64-bit values for checking */
    u64 h0 = ((u64)out[0])       | ((u64)out[1] << 8)  |
             ((u64)out[2] << 16) | ((u64)out[3] << 24) |
             ((u64)out[4] << 32) | ((u64)out[5] << 40) |
             ((u64)out[6] << 48) | ((u64)out[7] << 56);
    
    u64 h1 = ((u64)out[8])        | ((u64)out[9] << 8)   |
             ((u64)out[10] << 16) | ((u64)out[11] << 24) |
             ((u64)out[12] << 32) | ((u64)out[13] << 40) |
             ((u64)out[14] << 48) | ((u64)out[15] << 56);
    
    /* Both halves should be non-zero */
    cr_assert_neq(h0, 0);
    cr_assert_neq(h1, 0);
}

/* Test: NULL input handling */
Test(siphash, null_inputs)
{
    /* NULL data with zero length should still produce a hash (empty input is valid) */
    u64 hash = buckets_siphash(0, 0, "", 0);
    cr_assert_neq(hash, 0, "Empty input should produce non-zero hash");
    
    i32 set = buckets_hash_object_to_set(NULL, NULL, 8);
    cr_assert_eq(set, -1);
}

/* Test: Zero set count */
Test(siphash, zero_set_count)
{
    u8 deployment_id[16] = {0};
    i32 set = buckets_hash_object_to_set("object", deployment_id, 0);
    cr_assert_eq(set, -1);
}

/* Test: Key generation */
Test(siphash, key_generation)
{
    u64 k0_1, k1_1, k0_2, k1_2;
    
    buckets_error_t err1 = buckets_siphash_keygen(&k0_1, &k1_1);
    buckets_error_t err2 = buckets_siphash_keygen(&k0_2, &k1_2);
    
    cr_assert_eq(err1, BUCKETS_OK);
    cr_assert_eq(err2, BUCKETS_OK);
    
    /* Generated keys should be different (with very high probability) */
    cr_assert(k0_1 != k0_2 || k1_1 != k1_2, "Generated keys should be different");
}
