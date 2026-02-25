/**
 * Cryptographic Hash Functions
 * 
 * BLAKE2b implementation for object integrity and bitrot detection.
 * BLAKE2b is faster than SHA-256 (1.5-2x on 64-bit) and provides
 * strong cryptographic guarantees.
 */

#ifndef BUCKETS_CRYPTO_H
#define BUCKETS_CRYPTO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buckets.h"

/* BLAKE2b constants */
#define BUCKETS_BLAKE2B_BLOCKBYTES    128  /* Block size in bytes */
#define BUCKETS_BLAKE2B_OUTBYTES      64   /* Default output size (512 bits) */
#define BUCKETS_BLAKE2B_KEYBYTES      64   /* Max key size for MAC mode */
#define BUCKETS_BLAKE2B_SALTBYTES     16   /* Salt size */
#define BUCKETS_BLAKE2B_PERSONALBYTES 16   /* Personalization string size */

/* Common output sizes */
#define BUCKETS_BLAKE2B_256_OUTBYTES  32   /* 256-bit output */
#define BUCKETS_BLAKE2B_512_OUTBYTES  64   /* 512-bit output (default) */

/**
 * BLAKE2b context for incremental hashing
 */
typedef struct {
    u64 h[8];                                 /* Chained state (8 × 64-bit) */
    u64 t[2];                                 /* Total bytes hashed (128-bit counter) */
    u64 f[2];                                 /* Finalization flags */
    u8  buf[BUCKETS_BLAKE2B_BLOCKBYTES];     /* Input buffer */
    size_t buflen;                            /* Buffered bytes */
    size_t outlen;                            /* Output hash length */
} buckets_blake2b_ctx_t;

/**
 * BLAKE2b parameters for customization
 * Must be exactly 64 bytes (packed, no padding)
 */
typedef struct {
    u8 digest_length;                         /* Output hash length (1-64 bytes) */
    u8 key_length;                            /* Key length for MAC (0-64 bytes) */
    u8 fanout;                                /* Fanout for tree mode (0-255) */
    u8 depth;                                 /* Depth for tree mode (0-255) */
    u32 leaf_length;                          /* Leaf length for tree mode */
    u64 node_offset;                          /* Node offset for tree mode */
    u8 node_depth;                            /* Node depth for tree mode */
    u8 inner_length;                          /* Inner hash length for tree mode */
    u8 reserved[14];                          /* Reserved (must be zero) */
    u8 salt[BUCKETS_BLAKE2B_SALTBYTES];       /* Salt (16 bytes) */
    u8 personal[BUCKETS_BLAKE2B_PERSONALBYTES]; /* Personalization (16 bytes) */
} __attribute__((packed)) buckets_blake2b_param_t;

/**
 * Initialize BLAKE2b context with default parameters
 * 
 * @param ctx Context to initialize
 * @param outlen Output hash length (1-64 bytes)
 * @return 0 on success, -1 on error
 */
int buckets_blake2b_init(buckets_blake2b_ctx_t *ctx, size_t outlen);

/**
 * Initialize BLAKE2b context with key (for MAC)
 * 
 * @param ctx Context to initialize
 * @param outlen Output hash length (1-64 bytes)
 * @param key Key bytes (NULL for unkeyed hash)
 * @param keylen Key length (0-64 bytes)
 * @return 0 on success, -1 on error
 */
int buckets_blake2b_init_key(buckets_blake2b_ctx_t *ctx, size_t outlen,
                              const void *key, size_t keylen);

/**
 * Initialize BLAKE2b context with custom parameters
 * 
 * @param ctx Context to initialize
 * @param param Custom parameters
 * @return 0 on success, -1 on error
 */
int buckets_blake2b_init_param(buckets_blake2b_ctx_t *ctx,
                                const buckets_blake2b_param_t *param);

/**
 * Update BLAKE2b context with more data
 * 
 * @param ctx Context to update
 * @param data Input data
 * @param datalen Length of input data
 * @return 0 on success, -1 on error
 */
int buckets_blake2b_update(buckets_blake2b_ctx_t *ctx,
                            const void *data, size_t datalen);

/**
 * Finalize BLAKE2b hash and output result
 * 
 * @param ctx Context to finalize
 * @param out Output buffer (must be at least ctx->outlen bytes)
 * @param outlen Output buffer size
 * @return 0 on success, -1 on error
 */
int buckets_blake2b_final(buckets_blake2b_ctx_t *ctx, void *out, size_t outlen);

/**
 * One-shot BLAKE2b hash (simple interface)
 * 
 * Hash data in a single call without maintaining context.
 * 
 * @param out Output buffer (must be at least outlen bytes)
 * @param outlen Output hash length (1-64 bytes)
 * @param data Input data
 * @param datalen Length of input data
 * @param key Key for MAC (NULL for unkeyed hash)
 * @param keylen Key length (0-64 bytes)
 * @return 0 on success, -1 on error
 * 
 * Example:
 *   u8 hash[BUCKETS_BLAKE2B_OUTBYTES];
 *   buckets_blake2b(hash, sizeof(hash), data, datalen, NULL, 0);
 */
int buckets_blake2b(void *out, size_t outlen,
                    const void *data, size_t datalen,
                    const void *key, size_t keylen);

/**
 * BLAKE2b-256 (256-bit output) - convenience wrapper
 * 
 * @param out Output buffer (must be at least 32 bytes)
 * @param data Input data
 * @param datalen Length of input data
 * @return 0 on success, -1 on error
 */
int buckets_blake2b_256(void *out, const void *data, size_t datalen);

/**
 * BLAKE2b-512 (512-bit output) - convenience wrapper
 * 
 * @param out Output buffer (must be at least 64 bytes)
 * @param data Input data
 * @param datalen Length of input data
 * @return 0 on success, -1 on error
 */
int buckets_blake2b_512(void *out, const void *data, size_t datalen);

/**
 * BLAKE2b hash as hexadecimal string
 * 
 * Convenience function that hashes data and converts to hex string.
 * 
 * @param out Output buffer (must be at least outlen*2 + 1 bytes)
 * @param outlen Output hash length (1-64 bytes)
 * @param data Input data
 * @param datalen Length of input data
 * @return 0 on success, -1 on error
 * 
 * Example:
 *   char hexhash[65]; // 32 bytes * 2 + null terminator
 *   buckets_blake2b_hex(hexhash, 32, data, datalen);
 */
int buckets_blake2b_hex(char *out, size_t outlen,
                        const void *data, size_t datalen);

/**
 * Compare two BLAKE2b hashes in constant time
 * 
 * Timing-safe comparison to prevent timing attacks.
 * 
 * @param a First hash
 * @param b Second hash
 * @param len Length of hashes (must be same for both)
 * @return true if hashes match, false otherwise
 */
bool buckets_blake2b_verify(const void *a, const void *b, size_t len);

/**
 * Self-test for BLAKE2b implementation
 * 
 * Runs test vectors to verify correctness.
 * 
 * @return 0 on success, -1 if tests fail
 */
int buckets_blake2b_selftest(void);

/* ========================================================================
 * SHA-256 (Secure Hash Algorithm 256-bit)
 * ========================================================================
 * 
 * SHA-256 wrapper using OpenSSL for S3 compatibility and industry-standard
 * hashing. Use BLAKE2b for internal operations (faster), SHA-256 for
 * external compatibility (S3 ETags, AWS checksums).
 */

/* SHA-256 constants */
#define BUCKETS_SHA256_DIGEST_LENGTH 32  /* 256 bits = 32 bytes */

/**
 * SHA-256 one-shot hash
 * 
 * Compute SHA-256 hash of data in a single call.
 * 
 * @param out Output buffer (must be at least 32 bytes)
 * @param data Input data
 * @param datalen Length of input data
 * @return 0 on success, -1 on error
 * 
 * Example:
 *   u8 hash[BUCKETS_SHA256_DIGEST_LENGTH];
 *   buckets_sha256(hash, data, datalen);
 */
int buckets_sha256(void *out, const void *data, size_t datalen);

/**
 * SHA-256 hash as hexadecimal string
 * 
 * Convenience function that hashes data and converts to hex string.
 * 
 * @param out Output buffer (must be at least 65 bytes: 32*2 + null)
 * @param data Input data
 * @param datalen Length of input data
 * @return 0 on success, -1 on error
 * 
 * Example:
 *   char hexhash[65];
 *   buckets_sha256_hex(hexhash, data, datalen);
 */
int buckets_sha256_hex(char *out, const void *data, size_t datalen);

/**
 * Compare two SHA-256 hashes in constant time
 * 
 * Timing-safe comparison to prevent timing attacks.
 * 
 * @param a First hash (32 bytes)
 * @param b Second hash (32 bytes)
 * @return true if hashes match, false otherwise
 */
bool buckets_sha256_verify(const void *a, const void *b);

/**
 * Self-test for SHA-256 implementation
 * 
 * Runs test vectors to verify correctness.
 * 
 * @return 0 on success, -1 if tests fail
 */
int buckets_sha256_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_CRYPTO_H */
