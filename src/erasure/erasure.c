/**
 * Erasure Coding Implementation
 * 
 * Reed-Solomon erasure coding using Intel ISA-L for SIMD-optimized
 * performance. Supports configurable K+M configurations with fast
 * encode/decode operations.
 */

#include <string.h>
#include <stdio.h>
#include <isa-l/erasure_code.h>

#include "buckets_erasure.h"
#include "buckets.h"

/* Initialize erasure coding context */
int buckets_ec_init(buckets_ec_ctx_t *ctx, u32 k, u32 m)
{
    if (!ctx) {
        buckets_error("NULL context");
        return -1;
    }

    if (!buckets_ec_validate_config(k, m)) {
        buckets_error("Invalid erasure coding configuration: k=%u, m=%u", k, m);
        return -1;
    }

    /* Initialize context fields */
    ctx->k = k;
    ctx->m = m;
    ctx->n = k + m;

    /* Allocate encoding matrix (m x k) */
    size_t encode_matrix_size = m * k;
    ctx->encode_matrix = buckets_malloc(encode_matrix_size);
    if (!ctx->encode_matrix) {
        buckets_error("Failed to allocate encoding matrix");
        return -1;
    }

    /* Allocate temporary matrices for decode operations */
    ctx->decode_matrix = buckets_malloc(k * k);
    ctx->invert_matrix = buckets_malloc(k * k);
    ctx->error_list = buckets_malloc(ctx->n * sizeof(u32));
    
    if (!ctx->decode_matrix || !ctx->invert_matrix || !ctx->error_list) {
        buckets_error("Failed to allocate decode matrices");
        buckets_ec_free(ctx);
        return -1;
    }

    /* Allocate Galois field tables (32 bytes per coefficient) */
    size_t gftbls_size = 32 * k * m;
    ctx->gftbls = buckets_malloc(gftbls_size);
    if (!ctx->gftbls) {
        buckets_error("Failed to allocate Galois field tables");
        buckets_ec_free(ctx);
        return -1;
    }

    /* Generate Cauchy matrix for encoding
     * Top k rows are identity (data chunks pass through)
     * Bottom m rows are Cauchy matrix (parity chunks)
     */
    u8 *full_matrix = buckets_malloc((k + m) * k);
    if (!full_matrix) {
        buckets_error("Failed to allocate full encoding matrix");
        buckets_ec_free(ctx);
        return -1;
    }

    gf_gen_cauchy1_matrix(full_matrix, k + m, k);

    /* Copy parity rows (last m rows) to encode_matrix */
    memcpy(ctx->encode_matrix, full_matrix + (k * k), m * k);
    buckets_free(full_matrix);

    /* Initialize Galois field tables for fast encoding */
    ec_init_tables(k, m, ctx->encode_matrix, ctx->gftbls);

    buckets_debug("Initialized erasure coding context: k=%u, m=%u (%.1f%% overhead)",
                  k, m, buckets_ec_overhead_pct(k, m));

    return 0;
}

/* Free erasure coding context */
void buckets_ec_free(buckets_ec_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->encode_matrix) {
        buckets_free(ctx->encode_matrix);
        ctx->encode_matrix = NULL;
    }
    if (ctx->decode_matrix) {
        buckets_free(ctx->decode_matrix);
        ctx->decode_matrix = NULL;
    }
    if (ctx->invert_matrix) {
        buckets_free(ctx->invert_matrix);
        ctx->invert_matrix = NULL;
    }
    if (ctx->gftbls) {
        buckets_free(ctx->gftbls);
        ctx->gftbls = NULL;
    }
    if (ctx->error_list) {
        buckets_free(ctx->error_list);
        ctx->error_list = NULL;
    }

    ctx->k = 0;
    ctx->m = 0;
    ctx->n = 0;
}

/* Encode data into chunks with parity */
int buckets_ec_encode(buckets_ec_ctx_t *ctx, 
                      const void *data, size_t data_size,
                      size_t chunk_size,
                      u8 **data_chunks, u8 **parity_chunks)
{
    if (!ctx || !data || !data_chunks || !parity_chunks) {
        buckets_error("NULL parameter in encode");
        return -1;
    }

    if (chunk_size == 0) {
        buckets_error("Invalid chunk size: 0");
        return -1;
    }

    /* Verify chunk size is sufficient for data */
    size_t required_size = buckets_ec_calc_chunk_size(data_size, ctx->k);
    if (chunk_size < required_size) {
        buckets_error("Chunk size %zu too small, need at least %zu", 
                      chunk_size, required_size);
        return -1;
    }

    /* Split data into k chunks */
    const u8 *src = (const u8 *)data;
    size_t bytes_per_chunk = (data_size + ctx->k - 1) / ctx->k;
    
    buckets_info("Encode: data_size=%zu, k=%u, bytes_per_chunk=%zu, chunk_size=%zu",
                data_size, ctx->k, bytes_per_chunk, chunk_size);
    
    for (u32 i = 0; i < ctx->k; i++) {
        size_t offset = i * bytes_per_chunk;
        size_t remaining = (offset < data_size) ? (data_size - offset) : 0;
        size_t copy_size = (remaining > bytes_per_chunk) ? bytes_per_chunk : remaining;
        
        buckets_info("  Chunk %u: offset=%zu, remaining=%zu, copy_size=%zu", 
                    i, offset, remaining, copy_size);
        
        /* Copy data chunk */
        if (copy_size > 0) {
            memcpy(data_chunks[i], src + offset, copy_size);
            /* Log first few bytes */
            if (copy_size >= 4) {
                buckets_info("    First 4 bytes: %02x %02x %02x %02x", 
                            data_chunks[i][0], data_chunks[i][1], 
                            data_chunks[i][2], data_chunks[i][3]);
            }
        }
        
        /* Zero-pad remainder */
        if (copy_size < chunk_size) {
            memset(data_chunks[i] + copy_size, 0, chunk_size - copy_size);
        }
    }

    /* Generate parity chunks using ISA-L */
    ec_encode_data((int)chunk_size, (int)ctx->k, (int)ctx->m,
                   ctx->gftbls, data_chunks, parity_chunks);

    buckets_debug("Encoded %zu bytes into %u+%u chunks of %zu bytes each",
                  data_size, ctx->k, ctx->m, chunk_size);

    return 0;
}

/* Decode data from available chunks */
int buckets_ec_decode(buckets_ec_ctx_t *ctx,
                      u8 **chunks, size_t chunk_size,
                      void *data, size_t data_size)
{
    if (!ctx || !chunks || !data) {
        buckets_error("NULL parameter in decode");
        return -1;
    }

    /* Count available chunks and build error list */
    u32 error_count = 0;
    u32 available_count = 0;
    
    for (u32 i = 0; i < ctx->n; i++) {
        if (chunks[i] == NULL) {
            ctx->error_list[error_count++] = i;
        } else {
            available_count++;
        }
    }

    buckets_debug("Decoding with %u available chunks, %u missing", 
                  available_count, error_count);

    /* Check if we have enough chunks to decode */
    if (available_count < ctx->k) {
        buckets_error("Not enough chunks to decode: need %u, have %u",
                      ctx->k, available_count);
        return -1;
    }

    /* If no chunks are missing, just reassemble data */
    if (error_count == 0) {
        u8 *dest = (u8 *)data;
        size_t bytes_per_chunk = (data_size + ctx->k - 1) / ctx->k;
        
        buckets_debug("Reassembling data: bytes_per_chunk=%zu, data_size=%zu, chunk_size=%zu",
                     bytes_per_chunk, data_size, chunk_size);
        
        for (u32 i = 0; i < ctx->k; i++) {
            size_t offset = i * bytes_per_chunk;
            size_t remaining = (offset < data_size) ? (data_size - offset) : 0;
            size_t copy_size = (remaining > bytes_per_chunk) ? bytes_per_chunk : remaining;
            
            buckets_debug("  Chunk %u: offset=%zu, copy_size=%zu", i, offset, copy_size);
            
            if (copy_size > 0) {
                memcpy(dest + offset, chunks[i], copy_size);
            }
        }
        return 0;
    }

    /* Reconstruct missing chunks if needed */
    if (buckets_ec_reconstruct(ctx, chunks, chunk_size, 
                               ctx->error_list, error_count) != 0) {
        buckets_error("Failed to reconstruct missing chunks");
        return -1;
    }

    /* Now reassemble the data from reconstructed chunks */
    u8 *dest = (u8 *)data;
    size_t bytes_per_chunk = (data_size + ctx->k - 1) / ctx->k;
    
    for (u32 i = 0; i < ctx->k; i++) {
        size_t offset = i * bytes_per_chunk;
        size_t remaining = (offset < data_size) ? (data_size - offset) : 0;
        size_t copy_size = (remaining > bytes_per_chunk) ? bytes_per_chunk : remaining;
        
        if (copy_size > 0) {
            memcpy(dest + offset, chunks[i], copy_size);
        }
    }

    buckets_debug("Successfully decoded %zu bytes", data_size);
    return 0;
}

/* Reconstruct missing chunks */
int buckets_ec_reconstruct(buckets_ec_ctx_t *ctx,
                           u8 **chunks, size_t chunk_size,
                           const u32 *missing_indices, u32 missing_count)
{
    if (!ctx || !chunks || !missing_indices) {
        buckets_error("NULL parameter in reconstruct");
        return -1;
    }

    if (missing_count == 0) {
        return 0;  /* Nothing to reconstruct */
    }

    if (missing_count > ctx->m) {
        buckets_error("Too many missing chunks: %u (max %u)", 
                      missing_count, ctx->m);
        return -1;
    }

    /* Build list of available chunks */
    u8 *available_chunks[BUCKETS_EC_MAX_TOTAL];
    u32 available_indices[BUCKETS_EC_MAX_TOTAL];
    u32 available_count = 0;

    for (u32 i = 0; i < ctx->n && available_count < ctx->k; i++) {
        if (chunks[i] != NULL) {
            available_chunks[available_count] = chunks[i];
            available_indices[available_count] = i;
            available_count++;
        }
    }

    if (available_count < ctx->k) {
        buckets_error("Not enough chunks to reconstruct: need %u, have %u",
                      ctx->k, available_count);
        return -1;
    }

    /* Build decode matrix from available chunks
     * This is a k×k submatrix of the full encoding matrix
     */
    u8 *full_matrix = buckets_malloc((ctx->k + ctx->m) * ctx->k);
    if (!full_matrix) {
        buckets_error("Failed to allocate full matrix");
        return -1;
    }

    /* Reconstruct full encoding matrix (identity + parity) */
    gf_gen_cauchy1_matrix(full_matrix, ctx->k + ctx->m, ctx->k);

    /* Extract rows corresponding to available chunks */
    for (u32 i = 0; i < ctx->k; i++) {
        u32 row = available_indices[i];
        memcpy(ctx->decode_matrix + (i * ctx->k),
               full_matrix + (row * ctx->k),
               ctx->k);
    }

    /* Invert the decode matrix */
    u8 *temp_matrix = buckets_malloc(ctx->k * ctx->k);
    if (!temp_matrix) {
        buckets_error("Failed to allocate temp matrix");
        buckets_free(full_matrix);
        return -1;
    }

    memcpy(temp_matrix, ctx->decode_matrix, ctx->k * ctx->k);
    
    if (gf_invert_matrix(temp_matrix, ctx->invert_matrix, ctx->k) != 0) {
        buckets_error("Failed to invert decode matrix");
        buckets_free(temp_matrix);
        buckets_free(full_matrix);
        return -1;
    }

    buckets_free(temp_matrix);

    /* Allocate temporary buffers for reconstructed chunks */
    u8 *temp_chunks[BUCKETS_EC_MAX_TOTAL];
    for (u32 i = 0; i < missing_count; i++) {
        temp_chunks[i] = buckets_malloc(chunk_size);
        if (!temp_chunks[i]) {
            buckets_error("Failed to allocate temp chunk %u", i);
            for (u32 j = 0; j < i; j++) {
                buckets_free(temp_chunks[j]);
            }
            buckets_free(full_matrix);
            return -1;
        }
    }

    /* For each missing chunk, calculate from available chunks */
    for (u32 i = 0; i < missing_count; i++) {
        u32 missing_idx = missing_indices[i];
        
        /* Get the row from the inverted matrix corresponding to missing chunk */
        u8 *recovery_row = buckets_malloc(ctx->k);
        if (!recovery_row) {
            buckets_error("Failed to allocate recovery row");
            for (u32 j = 0; j < missing_count; j++) {
                buckets_free(temp_chunks[j]);
            }
            buckets_free(full_matrix);
            return -1;
        }

        /* Calculate recovery coefficients */
        for (u32 j = 0; j < ctx->k; j++) {
            recovery_row[j] = ctx->invert_matrix[missing_idx * ctx->k + j];
        }

        /* Generate Galois field tables for this recovery */
        u8 *recovery_gftbls = buckets_malloc(32 * ctx->k);
        if (!recovery_gftbls) {
            buckets_error("Failed to allocate recovery tables");
            buckets_free(recovery_row);
            for (u32 j = 0; j < missing_count; j++) {
                buckets_free(temp_chunks[j]);
            }
            buckets_free(full_matrix);
            return -1;
        }

        ec_init_tables(ctx->k, 1, recovery_row, recovery_gftbls);

        /* Reconstruct the chunk using available chunks */
        u8 *output_chunk = temp_chunks[i];
        ec_encode_data((int)chunk_size, (int)ctx->k, 1,
                      recovery_gftbls, available_chunks, &output_chunk);

        buckets_free(recovery_row);
        buckets_free(recovery_gftbls);
    }

    /* Copy reconstructed chunks back to original array */
    for (u32 i = 0; i < missing_count; i++) {
        u32 missing_idx = missing_indices[i];
        
        /* Allocate new chunk if NULL */
        if (chunks[missing_idx] == NULL) {
            chunks[missing_idx] = buckets_malloc(chunk_size);
            if (!chunks[missing_idx]) {
                buckets_error("Failed to allocate output chunk %u", missing_idx);
                for (u32 j = 0; j < missing_count; j++) {
                    buckets_free(temp_chunks[j]);
                }
                buckets_free(full_matrix);
                return -1;
            }
        }
        
        memcpy(chunks[missing_idx], temp_chunks[i], chunk_size);
        buckets_free(temp_chunks[i]);
    }

    buckets_free(full_matrix);

    buckets_debug("Successfully reconstructed %u missing chunks", missing_count);
    return 0;
}

/* Calculate optimal chunk size for given data size */
size_t buckets_ec_calc_chunk_size(size_t data_size, u32 k)
{
    if (k == 0) {
        return 0;
    }
    
    /* Round up to nearest multiple of 16 for SIMD alignment */
    size_t chunk_size = (data_size + k - 1) / k;
    chunk_size = (chunk_size + 15) & ~15;
    
    return chunk_size;
}

/* Validate erasure coding configuration */
bool buckets_ec_validate_config(u32 k, u32 m)
{
    if (k < 1 || k > BUCKETS_EC_MAX_DATA) {
        buckets_error("Invalid k value: %u (must be 1-%u)", 
                      k, BUCKETS_EC_MAX_DATA);
        return false;
    }
    
    if (m < 1 || m > BUCKETS_EC_MAX_PARITY) {
        buckets_error("Invalid m value: %u (must be 1-%u)", 
                      m, BUCKETS_EC_MAX_PARITY);
        return false;
    }
    
    if (k + m > BUCKETS_EC_MAX_TOTAL) {
        buckets_error("Invalid k+m: %u (must be <= %u)", 
                      k + m, BUCKETS_EC_MAX_TOTAL);
        return false;
    }
    
    return true;
}

/* Get erasure coding overhead percentage */
float buckets_ec_overhead_pct(u32 k, u32 m)
{
    if (k == 0) {
        return 0.0f;
    }
    return (100.0f * m) / k;
}

/* Self-test for erasure coding implementation */
int buckets_ec_selftest(void)
{
    buckets_info("Running erasure coding self-test...");

    /* Test 4+2 configuration */
    buckets_ec_ctx_t ctx;
    if (buckets_ec_init(&ctx, 4, 2) != 0) {
        buckets_error("Failed to initialize 4+2 context");
        return -1;
    }

    /* Test data */
    const char *test_str = "Hello, Buckets! This is a test of erasure coding.";
    size_t data_size = strlen(test_str) + 1;
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
    if (buckets_ec_encode(&ctx, test_str, data_size, chunk_size,
                          data_chunks, parity_chunks) != 0) {
        buckets_error("Encode failed");
        goto cleanup;
    }

    /* Test decode with all chunks available */
    u8 *all_chunks[6];
    for (int i = 0; i < 4; i++) {
        all_chunks[i] = data_chunks[i];
    }
    for (int i = 0; i < 2; i++) {
        all_chunks[4 + i] = parity_chunks[i];
    }

    char *decoded = buckets_malloc(data_size);
    if (buckets_ec_decode(&ctx, all_chunks, chunk_size, decoded, data_size) != 0) {
        buckets_error("Decode failed");
        buckets_free(decoded);
        goto cleanup;
    }

    if (strcmp(decoded, test_str) != 0) {
        buckets_error("Decoded data doesn't match: '%s' != '%s'", 
                      decoded, test_str);
        buckets_free(decoded);
        goto cleanup;
    }
    buckets_free(decoded);

    /* Test decode with 2 missing chunks */
    all_chunks[1] = NULL;  /* Missing data chunk 1 */
    all_chunks[3] = NULL;  /* Missing data chunk 3 */

    decoded = buckets_malloc(data_size);
    if (buckets_ec_decode(&ctx, all_chunks, chunk_size, decoded, data_size) != 0) {
        buckets_error("Decode with missing chunks failed");
        buckets_free(decoded);
        goto cleanup;
    }

    if (strcmp(decoded, test_str) != 0) {
        buckets_error("Decoded data with missing chunks doesn't match");
        buckets_free(decoded);
        goto cleanup;
    }
    buckets_free(decoded);

    buckets_info("Erasure coding self-test PASSED");

cleanup:
    for (int i = 0; i < 4; i++) {
        buckets_free(data_chunks[i]);
    }
    for (int i = 0; i < 2; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_ec_free(&ctx);

    return 0;
}
