/**
 * Erasure Coding Tests
 * 
 * Comprehensive test suite for Reed-Solomon erasure coding implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <criterion/criterion.h>

#include "buckets_erasure.h"
#include "buckets.h"

/* Test context initialization and cleanup */
Test(erasure, init_and_free)
{
    buckets_ec_ctx_t ctx;
    
    /* Test valid 4+2 configuration */
    cr_assert_eq(buckets_ec_init(&ctx, 4, 2), 0, "Should initialize 4+2 context");
    cr_assert_eq(ctx.k, 4, "k should be 4");
    cr_assert_eq(ctx.m, 2, "m should be 2");
    cr_assert_eq(ctx.n, 6, "n should be 6");
    cr_assert_neq(ctx.encode_matrix, NULL, "encode_matrix should be allocated");
    cr_assert_neq(ctx.gftbls, NULL, "gftbls should be allocated");
    
    buckets_ec_free(&ctx);
    cr_assert_eq(ctx.encode_matrix, NULL, "encode_matrix should be freed");
    cr_assert_eq(ctx.gftbls, NULL, "gftbls should be freed");
}

/* Test 8+4 configuration */
Test(erasure, init_8_4)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 8, 4), 0, "Should initialize 8+4 context");
    cr_assert_eq(ctx.k, 8, "k should be 8");
    cr_assert_eq(ctx.m, 4, "m should be 4");
    buckets_ec_free(&ctx);
}

/* Test 12+4 configuration */
Test(erasure, init_12_4)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 12, 4), 0, "Should initialize 12+4 context");
    cr_assert_eq(ctx.k, 12, "k should be 12");
    cr_assert_eq(ctx.m, 4, "m should be 4");
    buckets_ec_free(&ctx);
}

/* Test 16+4 configuration */
Test(erasure, init_16_4)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 16, 4), 0, "Should initialize 16+4 context");
    cr_assert_eq(ctx.k, 16, "k should be 16");
    cr_assert_eq(ctx.m, 4, "m should be 4");
    buckets_ec_free(&ctx);
}

/* Test NULL context */
Test(erasure, init_null_context)
{
    cr_assert_eq(buckets_ec_init(NULL, 4, 2), -1, "Should fail with NULL context");
}

/* Test invalid k */
Test(erasure, init_invalid_k)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 0, 2), -1, "Should fail with k=0");
    cr_assert_eq(buckets_ec_init(&ctx, 17, 2), -1, "Should fail with k=17");
}

/* Test invalid m */
Test(erasure, init_invalid_m)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 4, 0), -1, "Should fail with m=0");
    cr_assert_eq(buckets_ec_init(&ctx, 4, 17), -1, "Should fail with m=17");
}

/* Test configuration validation */
Test(erasure, validate_config)
{
    cr_assert(buckets_ec_validate_config(4, 2), "4+2 should be valid");
    cr_assert(buckets_ec_validate_config(8, 4), "8+4 should be valid");
    cr_assert(buckets_ec_validate_config(12, 4), "12+4 should be valid");
    cr_assert(buckets_ec_validate_config(16, 4), "16+4 should be valid");
    
    cr_assert_not(buckets_ec_validate_config(0, 2), "k=0 should be invalid");
    cr_assert_not(buckets_ec_validate_config(4, 0), "m=0 should be invalid");
    cr_assert_not(buckets_ec_validate_config(17, 2), "k=17 should be invalid");
    cr_assert_not(buckets_ec_validate_config(4, 17), "m=17 should be invalid");
}

/* Test chunk size calculation */
Test(erasure, calc_chunk_size)
{
    /* Test alignment to 16 bytes */
    cr_assert_eq(buckets_ec_calc_chunk_size(100, 4), 32, "100/4 = 25 -> 32 (aligned)");
    cr_assert_eq(buckets_ec_calc_chunk_size(128, 4), 32, "128/4 = 32 -> 32");
    cr_assert_eq(buckets_ec_calc_chunk_size(1000, 8), 128, "1000/8 = 125 -> 128");
    
    /* Test zero k */
    cr_assert_eq(buckets_ec_calc_chunk_size(100, 0), 0, "k=0 should return 0");
}

/* Test overhead calculation */
Test(erasure, overhead_pct)
{
    cr_assert_eq(buckets_ec_overhead_pct(4, 2), 50.0f, "4+2 has 50% overhead");
    cr_assert_eq(buckets_ec_overhead_pct(8, 4), 50.0f, "8+4 has 50% overhead");
    cr_assert_eq(buckets_ec_overhead_pct(12, 4), 33.333332f, "12+4 has 33.33% overhead");
    cr_assert_eq(buckets_ec_overhead_pct(16, 4), 25.0f, "16+4 has 25% overhead");
}

/* Test encode with simple data */
Test(erasure, encode_simple)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 4, 2), 0, "Should initialize context");
    
    const char *data = "Hello, World!";
    size_t data_size = strlen(data) + 1;
    size_t chunk_size = buckets_ec_calc_chunk_size(data_size, 4);
    
    /* Allocate chunks */
    u8 *data_chunks[4];
    u8 *parity_chunks[2];
    for (int i = 0; i < 4; i++) {
        data_chunks[i] = buckets_malloc(chunk_size);
    }
    for (int i = 0; i < 2; i++) {
        parity_chunks[i] = buckets_malloc(chunk_size);
    }
    
    /* Encode */
    cr_assert_eq(buckets_ec_encode(&ctx, data, data_size, chunk_size,
                                   data_chunks, parity_chunks), 0,
                 "Should encode successfully");
    
    /* Verify data chunks contain parts of original data */
    cr_assert_eq(memcmp(data_chunks[0], data, 4), 0, "Chunk 0 should contain start of data");
    
    /* Cleanup */
    for (int i = 0; i < 4; i++) {
        buckets_free(data_chunks[i]);
    }
    for (int i = 0; i < 2; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_ec_free(&ctx);
}

/* Test encode/decode round trip */
Test(erasure, encode_decode_roundtrip)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 4, 2), 0, "Should initialize context");
    
    const char *original = "The quick brown fox jumps over the lazy dog!";
    size_t data_size = strlen(original) + 1;
    size_t chunk_size = buckets_ec_calc_chunk_size(data_size, 4);
    
    /* Allocate chunks */
    u8 *data_chunks[4];
    u8 *parity_chunks[2];
    for (int i = 0; i < 4; i++) {
        data_chunks[i] = buckets_malloc(chunk_size);
    }
    for (int i = 0; i < 2; i++) {
        parity_chunks[i] = buckets_malloc(chunk_size);
    }
    
    /* Encode */
    cr_assert_eq(buckets_ec_encode(&ctx, original, data_size, chunk_size,
                                   data_chunks, parity_chunks), 0,
                 "Should encode successfully");
    
    /* Prepare for decode */
    u8 *all_chunks[6];
    for (int i = 0; i < 4; i++) {
        all_chunks[i] = data_chunks[i];
    }
    for (int i = 0; i < 2; i++) {
        all_chunks[4 + i] = parity_chunks[i];
    }
    
    /* Decode */
    char *decoded = buckets_malloc(data_size);
    cr_assert_eq(buckets_ec_decode(&ctx, all_chunks, chunk_size, 
                                   decoded, data_size), 0,
                 "Should decode successfully");
    
    /* Verify */
    cr_assert_str_eq(decoded, original, "Decoded data should match original");
    
    /* Cleanup */
    buckets_free(decoded);
    for (int i = 0; i < 4; i++) {
        buckets_free(data_chunks[i]);
    }
    for (int i = 0; i < 2; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_ec_free(&ctx);
}

/* Test decode with 1 missing data chunk */
Test(erasure, decode_missing_1_data)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 4, 2), 0, "Should initialize context");
    
    const char *original = "Testing erasure coding with one missing chunk";
    size_t data_size = strlen(original) + 1;
    size_t chunk_size = buckets_ec_calc_chunk_size(data_size, 4);
    
    /* Allocate and encode */
    u8 *data_chunks[4];
    u8 *parity_chunks[2];
    for (int i = 0; i < 4; i++) {
        data_chunks[i] = buckets_malloc(chunk_size);
    }
    for (int i = 0; i < 2; i++) {
        parity_chunks[i] = buckets_malloc(chunk_size);
    }
    
    cr_assert_eq(buckets_ec_encode(&ctx, original, data_size, chunk_size,
                                   data_chunks, parity_chunks), 0,
                 "Should encode successfully");
    
    /* Prepare for decode with missing chunk */
    u8 *all_chunks[6];
    for (int i = 0; i < 4; i++) {
        all_chunks[i] = data_chunks[i];
    }
    for (int i = 0; i < 2; i++) {
        all_chunks[4 + i] = parity_chunks[i];
    }
    
    all_chunks[1] = NULL;  /* Missing data chunk 1 */
    
    /* Decode */
    char *decoded = buckets_malloc(data_size);
    cr_assert_eq(buckets_ec_decode(&ctx, all_chunks, chunk_size,
                                   decoded, data_size), 0,
                 "Should decode with 1 missing chunk");
    
    /* Verify */
    cr_assert_str_eq(decoded, original, 
                     "Decoded data should match original with 1 missing chunk");
    
    /* Cleanup */
    buckets_free(decoded);
    for (int i = 0; i < 4; i++) {
        buckets_free(data_chunks[i]);
    }
    for (int i = 0; i < 2; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_ec_free(&ctx);
}

/* Test decode with 2 missing data chunks */
Test(erasure, decode_missing_2_data)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 4, 2), 0, "Should initialize context");
    
    const char *original = "Testing with two missing data chunks!";
    size_t data_size = strlen(original) + 1;
    size_t chunk_size = buckets_ec_calc_chunk_size(data_size, 4);
    
    /* Allocate and encode */
    u8 *data_chunks[4];
    u8 *parity_chunks[2];
    for (int i = 0; i < 4; i++) {
        data_chunks[i] = buckets_malloc(chunk_size);
    }
    for (int i = 0; i < 2; i++) {
        parity_chunks[i] = buckets_malloc(chunk_size);
    }
    
    cr_assert_eq(buckets_ec_encode(&ctx, original, data_size, chunk_size,
                                   data_chunks, parity_chunks), 0,
                 "Should encode successfully");
    
    /* Prepare for decode with missing chunks */
    u8 *all_chunks[6];
    for (int i = 0; i < 4; i++) {
        all_chunks[i] = data_chunks[i];
    }
    for (int i = 0; i < 2; i++) {
        all_chunks[4 + i] = parity_chunks[i];
    }
    
    all_chunks[0] = NULL;  /* Missing data chunk 0 */
    all_chunks[2] = NULL;  /* Missing data chunk 2 */
    
    /* Decode */
    char *decoded = buckets_malloc(data_size);
    cr_assert_eq(buckets_ec_decode(&ctx, all_chunks, chunk_size,
                                   decoded, data_size), 0,
                 "Should decode with 2 missing chunks");
    
    /* Verify */
    cr_assert_str_eq(decoded, original,
                     "Decoded data should match original with 2 missing chunks");
    
    /* Cleanup */
    buckets_free(decoded);
    for (int i = 0; i < 4; i++) {
        buckets_free(data_chunks[i]);
    }
    for (int i = 0; i < 2; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_ec_free(&ctx);
}

/* Test decode with 1 missing parity chunk */
Test(erasure, decode_missing_1_parity)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 4, 2), 0, "Should initialize context");
    
    const char *original = "Testing with missing parity chunk";
    size_t data_size = strlen(original) + 1;
    size_t chunk_size = buckets_ec_calc_chunk_size(data_size, 4);
    
    /* Allocate and encode */
    u8 *data_chunks[4];
    u8 *parity_chunks[2];
    for (int i = 0; i < 4; i++) {
        data_chunks[i] = buckets_malloc(chunk_size);
    }
    for (int i = 0; i < 2; i++) {
        parity_chunks[i] = buckets_malloc(chunk_size);
    }
    
    cr_assert_eq(buckets_ec_encode(&ctx, original, data_size, chunk_size,
                                   data_chunks, parity_chunks), 0,
                 "Should encode successfully");
    
    /* Prepare for decode with missing parity */
    u8 *all_chunks[6];
    for (int i = 0; i < 4; i++) {
        all_chunks[i] = data_chunks[i];
    }
    for (int i = 0; i < 2; i++) {
        all_chunks[4 + i] = parity_chunks[i];
    }
    
    all_chunks[4] = NULL;  /* Missing parity chunk 0 */
    
    /* Decode */
    char *decoded = buckets_malloc(data_size);
    cr_assert_eq(buckets_ec_decode(&ctx, all_chunks, chunk_size,
                                   decoded, data_size), 0,
                 "Should decode with missing parity");
    
    /* Verify */
    cr_assert_str_eq(decoded, original,
                     "Decoded data should match original with missing parity");
    
    /* Cleanup */
    buckets_free(decoded);
    for (int i = 0; i < 4; i++) {
        buckets_free(data_chunks[i]);
    }
    for (int i = 0; i < 2; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_ec_free(&ctx);
}

/* Test decode with mixed missing chunks */
Test(erasure, decode_missing_mixed)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 4, 2), 0, "Should initialize context");
    
    const char *original = "Testing with mixed missing data and parity chunks";
    size_t data_size = strlen(original) + 1;
    size_t chunk_size = buckets_ec_calc_chunk_size(data_size, 4);
    
    /* Allocate and encode */
    u8 *data_chunks[4];
    u8 *parity_chunks[2];
    for (int i = 0; i < 4; i++) {
        data_chunks[i] = buckets_malloc(chunk_size);
    }
    for (int i = 0; i < 2; i++) {
        parity_chunks[i] = buckets_malloc(chunk_size);
    }
    
    cr_assert_eq(buckets_ec_encode(&ctx, original, data_size, chunk_size,
                                   data_chunks, parity_chunks), 0,
                 "Should encode successfully");
    
    /* Prepare for decode with mixed missing */
    u8 *all_chunks[6];
    for (int i = 0; i < 4; i++) {
        all_chunks[i] = data_chunks[i];
    }
    for (int i = 0; i < 2; i++) {
        all_chunks[4 + i] = parity_chunks[i];
    }
    
    all_chunks[1] = NULL;  /* Missing data chunk 1 */
    all_chunks[5] = NULL;  /* Missing parity chunk 1 */
    
    /* Decode */
    char *decoded = buckets_malloc(data_size);
    cr_assert_eq(buckets_ec_decode(&ctx, all_chunks, chunk_size,
                                   decoded, data_size), 0,
                 "Should decode with mixed missing chunks");
    
    /* Verify */
    cr_assert_str_eq(decoded, original,
                     "Decoded data should match original with mixed missing chunks");
    
    /* Cleanup */
    buckets_free(decoded);
    for (int i = 0; i < 4; i++) {
        buckets_free(data_chunks[i]);
    }
    for (int i = 0; i < 2; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_ec_free(&ctx);
}

/* Test decode with too many missing chunks (should fail) */
Test(erasure, decode_too_many_missing)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 4, 2), 0, "Should initialize context");
    
    const char *original = "This should fail";
    size_t data_size = strlen(original) + 1;
    size_t chunk_size = buckets_ec_calc_chunk_size(data_size, 4);
    
    /* Allocate and encode */
    u8 *data_chunks[4];
    u8 *parity_chunks[2];
    for (int i = 0; i < 4; i++) {
        data_chunks[i] = buckets_malloc(chunk_size);
    }
    for (int i = 0; i < 2; i++) {
        parity_chunks[i] = buckets_malloc(chunk_size);
    }
    
    cr_assert_eq(buckets_ec_encode(&ctx, original, data_size, chunk_size,
                                   data_chunks, parity_chunks), 0,
                 "Should encode successfully");
    
    /* Prepare for decode with too many missing */
    u8 *all_chunks[6];
    for (int i = 0; i < 4; i++) {
        all_chunks[i] = data_chunks[i];
    }
    for (int i = 0; i < 2; i++) {
        all_chunks[4 + i] = parity_chunks[i];
    }
    
    all_chunks[0] = NULL;  /* Missing 3 chunks - more than m=2 */
    all_chunks[1] = NULL;
    all_chunks[2] = NULL;
    
    /* Decode should fail */
    char *decoded = buckets_malloc(data_size);
    cr_assert_eq(buckets_ec_decode(&ctx, all_chunks, chunk_size,
                                   decoded, data_size), -1,
                 "Should fail with too many missing chunks");
    
    /* Cleanup */
    buckets_free(decoded);
    for (int i = 0; i < 4; i++) {
        buckets_free(data_chunks[i]);
    }
    for (int i = 0; i < 2; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_ec_free(&ctx);
}

/* Test with larger data (1KB) */
Test(erasure, encode_decode_1kb)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 8, 4), 0, "Should initialize 8+4 context");
    
    /* Generate 1KB of test data */
    size_t data_size = 1024;
    u8 *original = buckets_malloc(data_size);
    for (size_t i = 0; i < data_size; i++) {
        original[i] = (u8)(i & 0xFF);
    }
    
    size_t chunk_size = buckets_ec_calc_chunk_size(data_size, 8);
    
    /* Allocate chunks */
    u8 *data_chunks[8];
    u8 *parity_chunks[4];
    for (int i = 0; i < 8; i++) {
        data_chunks[i] = buckets_malloc(chunk_size);
    }
    for (int i = 0; i < 4; i++) {
        parity_chunks[i] = buckets_malloc(chunk_size);
    }
    
    /* Encode */
    cr_assert_eq(buckets_ec_encode(&ctx, original, data_size, chunk_size,
                                   data_chunks, parity_chunks), 0,
                 "Should encode 1KB");
    
    /* Decode with 2 missing chunks */
    u8 *all_chunks[12];
    for (int i = 0; i < 8; i++) {
        all_chunks[i] = data_chunks[i];
    }
    for (int i = 0; i < 4; i++) {
        all_chunks[8 + i] = parity_chunks[i];
    }
    
    all_chunks[2] = NULL;
    all_chunks[6] = NULL;
    
    u8 *decoded = buckets_malloc(data_size);
    cr_assert_eq(buckets_ec_decode(&ctx, all_chunks, chunk_size,
                                   decoded, data_size), 0,
                 "Should decode 1KB with missing chunks");
    
    /* Verify */
    cr_assert_eq(memcmp(decoded, original, data_size), 0,
                 "Decoded 1KB should match original");
    
    /* Cleanup */
    buckets_free(original);
    buckets_free(decoded);
    for (int i = 0; i < 8; i++) {
        buckets_free(data_chunks[i]);
    }
    for (int i = 0; i < 4; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_ec_free(&ctx);
}

/* Test with larger data (64KB) */
Test(erasure, encode_decode_64kb)
{
    buckets_ec_ctx_t ctx;
    cr_assert_eq(buckets_ec_init(&ctx, 12, 4), 0, "Should initialize 12+4 context");
    
    /* Generate 64KB of test data */
    size_t data_size = 65536;
    u8 *original = buckets_malloc(data_size);
    for (size_t i = 0; i < data_size; i++) {
        original[i] = (u8)((i * 7 + 13) & 0xFF);
    }
    
    size_t chunk_size = buckets_ec_calc_chunk_size(data_size, 12);
    
    /* Allocate chunks */
    u8 *data_chunks[12];
    u8 *parity_chunks[4];
    for (int i = 0; i < 12; i++) {
        data_chunks[i] = buckets_malloc(chunk_size);
    }
    for (int i = 0; i < 4; i++) {
        parity_chunks[i] = buckets_malloc(chunk_size);
    }
    
    /* Encode */
    cr_assert_eq(buckets_ec_encode(&ctx, original, data_size, chunk_size,
                                   data_chunks, parity_chunks), 0,
                 "Should encode 64KB");
    
    /* Decode with 4 missing chunks */
    u8 *all_chunks[16];
    for (int i = 0; i < 12; i++) {
        all_chunks[i] = data_chunks[i];
    }
    for (int i = 0; i < 4; i++) {
        all_chunks[12 + i] = parity_chunks[i];
    }
    
    all_chunks[1] = NULL;
    all_chunks[5] = NULL;
    all_chunks[8] = NULL;
    all_chunks[11] = NULL;
    
    u8 *decoded = buckets_malloc(data_size);
    cr_assert_eq(buckets_ec_decode(&ctx, all_chunks, chunk_size,
                                   decoded, data_size), 0,
                 "Should decode 64KB with 4 missing chunks");
    
    /* Verify */
    cr_assert_eq(memcmp(decoded, original, data_size), 0,
                 "Decoded 64KB should match original");
    
    /* Cleanup */
    buckets_free(original);
    buckets_free(decoded);
    for (int i = 0; i < 12; i++) {
        buckets_free(data_chunks[i]);
    }
    for (int i = 0; i < 4; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_ec_free(&ctx);
}

/* Test self-test function */
Test(erasure, selftest)
{
    cr_assert_eq(buckets_ec_selftest(), 0, "Self-test should pass");
}
