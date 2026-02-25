/**
 * Erasure Coding (Reed-Solomon) API
 * 
 * Reed-Solomon erasure coding for data protection with configurable
 * redundancy. Splits data into N data chunks and K parity chunks,
 * allowing recovery from loss of up to K chunks.
 * 
 * Uses Intel ISA-L for optimized SIMD performance.
 */

#ifndef BUCKETS_ERASURE_H
#define BUCKETS_ERASURE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buckets.h"

/* Erasure coding constants */
#define BUCKETS_EC_MAX_DATA     16   /* Maximum data chunks */
#define BUCKETS_EC_MAX_PARITY   16   /* Maximum parity chunks */
#define BUCKETS_EC_MAX_TOTAL    32   /* Maximum total chunks (data + parity) */

/* Common configurations */
#define BUCKETS_EC_4_2          4, 2  /* 4 data + 2 parity (50% overhead) */
#define BUCKETS_EC_8_4          8, 4  /* 8 data + 4 parity (50% overhead) */
#define BUCKETS_EC_12_4        12, 4  /* 12 data + 4 parity (33% overhead) */
#define BUCKETS_EC_16_4        16, 4  /* 16 data + 4 parity (25% overhead) */

/**
 * Erasure coding context
 * 
 * Maintains encoding/decoding state and matrices.
 * Create once and reuse for multiple encode/decode operations.
 */
typedef struct {
    u32 k;                    /* Number of data chunks */
    u32 m;                    /* Number of parity chunks */
    u32 n;                    /* Total chunks (k + m) */
    u8 *encode_matrix;        /* Encoding matrix (k × m) */
    u8 *decode_matrix;        /* Temporary decode matrix */
    u8 *invert_matrix;        /* Temporary invert matrix */
    u8 *gftbls;               /* Galois field tables */
    u32 *error_list;          /* List of erased chunk indices */
} buckets_ec_ctx_t;

/**
 * Initialize erasure coding context
 * 
 * @param ctx Context to initialize (must be allocated by caller)
 * @param k Number of data chunks (1-16)
 * @param m Number of parity chunks (1-16)
 * @return 0 on success, -1 on error
 * 
 * Example:
 *   buckets_ec_ctx_t ctx;
 *   buckets_ec_init(&ctx, 8, 4);  // 8+4 configuration
 */
int buckets_ec_init(buckets_ec_ctx_t *ctx, u32 k, u32 m);

/**
 * Free erasure coding context
 * 
 * @param ctx Context to free
 */
void buckets_ec_free(buckets_ec_ctx_t *ctx);

/**
 * Encode data into chunks with parity
 * 
 * Splits input data into k data chunks and generates m parity chunks.
 * Each chunk will be chunk_size bytes.
 * 
 * @param ctx Erasure coding context
 * @param data Input data
 * @param data_size Size of input data
 * @param chunk_size Size of each chunk (data_size / k, rounded up)
 * @param data_chunks Output array of k data chunk buffers (must be allocated)
 * @param parity_chunks Output array of m parity chunk buffers (must be allocated)
 * @return 0 on success, -1 on error
 * 
 * Example:
 *   u8 *data_chunks[8];
 *   u8 *parity_chunks[4];
 *   for (int i = 0; i < 8; i++) data_chunks[i] = malloc(chunk_size);
 *   for (int i = 0; i < 4; i++) parity_chunks[i] = malloc(chunk_size);
 *   buckets_ec_encode(&ctx, data, data_size, chunk_size, 
 *                     data_chunks, parity_chunks);
 */
int buckets_ec_encode(buckets_ec_ctx_t *ctx, 
                      const void *data, size_t data_size,
                      size_t chunk_size,
                      u8 **data_chunks, u8 **parity_chunks);

/**
 * Decode data from available chunks
 * 
 * Reconstructs original data from any k chunks (data or parity).
 * Missing chunks are indicated by NULL pointers.
 * 
 * @param ctx Erasure coding context
 * @param chunks Array of n chunks (data + parity), NULL for missing chunks
 * @param chunk_size Size of each chunk
 * @param data Output buffer for decoded data (must be allocated)
 * @param data_size Size of output buffer
 * @return 0 on success, -1 on error (too many missing chunks)
 * 
 * Example:
 *   u8 *chunks[12];  // 8 data + 4 parity
 *   chunks[2] = NULL;  // Missing chunk 2
 *   chunks[5] = NULL;  // Missing chunk 5
 *   u8 *recovered = malloc(original_size);
 *   buckets_ec_decode(&ctx, chunks, chunk_size, recovered, original_size);
 */
int buckets_ec_decode(buckets_ec_ctx_t *ctx,
                      u8 **chunks, size_t chunk_size,
                      void *data, size_t data_size);

/**
 * Reconstruct missing chunks
 * 
 * Reconstructs specific missing data or parity chunks from available chunks.
 * More efficient than full decode when you only need specific chunks.
 * 
 * @param ctx Erasure coding context
 * @param chunks Array of n chunks (data + parity), NULL for missing chunks
 * @param chunk_size Size of each chunk
 * @param missing_indices Array of indices of missing chunks to reconstruct
 * @param missing_count Number of missing chunks
 * @return 0 on success, -1 on error
 * 
 * Example:
 *   u32 missing[] = {2, 5};  // Reconstruct chunks 2 and 5
 *   buckets_ec_reconstruct(&ctx, chunks, chunk_size, missing, 2);
 *   // chunks[2] and chunks[5] are now filled
 */
int buckets_ec_reconstruct(buckets_ec_ctx_t *ctx,
                           u8 **chunks, size_t chunk_size,
                           const u32 *missing_indices, u32 missing_count);

/**
 * Calculate optimal chunk size for given data size
 * 
 * Returns chunk size that evenly divides data across k chunks
 * with minimal padding.
 * 
 * @param data_size Size of data to encode
 * @param k Number of data chunks
 * @return Optimal chunk size
 */
size_t buckets_ec_calc_chunk_size(size_t data_size, u32 k);

/**
 * Validate erasure coding configuration
 * 
 * Checks if k and m are within valid ranges and compatible.
 * 
 * @param k Number of data chunks
 * @param m Number of parity chunks
 * @return true if valid, false otherwise
 */
bool buckets_ec_validate_config(u32 k, u32 m);

/**
 * Get erasure coding overhead percentage
 * 
 * @param k Number of data chunks
 * @param m Number of parity chunks
 * @return Overhead percentage (e.g., 50.0 for 8+4)
 */
float buckets_ec_overhead_pct(u32 k, u32 m);

/**
 * Self-test for erasure coding implementation
 * 
 * Tests encode/decode with various configurations and missing chunks.
 * 
 * @return 0 on success, -1 if tests fail
 */
int buckets_ec_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_ERASURE_H */
