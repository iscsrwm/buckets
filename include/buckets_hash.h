/**
 * Hashing Algorithms for Buckets
 * 
 * This module provides hash functions for object placement and data integrity:
 * - SipHash-2-4: Keyed hash for consistent object placement
 * - xxHash-64: High-speed hash for checksums (Week 6)
 * - BLAKE2b: Cryptographic hash for integrity (Week 8)
 */

#ifndef BUCKETS_HASH_H
#define BUCKETS_HASH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "buckets.h"

/* ============================================================================
 * SipHash-2-4 - Keyed Hash for Consistent Placement
 * ============================================================================
 * 
 * SipHash is a fast, cryptographically strong PRF (pseudo-random function)
 * designed for hash tables and other short-input use cases.
 * 
 * Properties:
 * - Fast: ~1.5 cycles/byte on modern CPUs
 * - Secure: Resistant to hash-flooding DoS attacks
 * - Keyed: Uses 128-bit secret key for domain separation
 * - Deterministic: Same input + key = same output
 * 
 * Parameters: SipHash-c-d where:
 * - c = compression rounds (2 for SipHash-2-4)
 * - d = finalization rounds (4 for SipHash-2-4)
 * 
 * Reference: https://131002.net/siphash/
 */

/**
 * SipHash state structure
 * 
 * Internal state for incremental hashing.
 */
typedef struct buckets_siphash_state {
    u64 v0, v1, v2, v3;     /* State variables */
    u64 k0, k1;             /* 128-bit key (two 64-bit words) */
    u64 total_len;          /* Total bytes processed */
    u8 buf[8];              /* Buffer for partial block */
    size_t buf_len;         /* Bytes in buffer */
} buckets_siphash_state_t;

/**
 * Initialize SipHash state with key
 * 
 * The 128-bit key should be generated from a secure random source
 * and kept consistent for the lifetime of the cluster to ensure
 * deterministic object placement.
 * 
 * @param state State structure to initialize
 * @param k0 First 64 bits of key
 * @param k1 Second 64 bits of key
 */
void buckets_siphash_init(buckets_siphash_state_t *state, u64 k0, u64 k1);

/**
 * Update SipHash state with data
 * 
 * Can be called multiple times to hash data incrementally.
 * 
 * @param state State structure
 * @param data Input data
 * @param len Length of input data
 */
void buckets_siphash_update(buckets_siphash_state_t *state, 
                            const void *data, 
                            size_t len);

/**
 * Finalize SipHash and produce 64-bit hash
 * 
 * After calling this, the state is consumed and must be
 * reinitialized for a new hash computation.
 * 
 * @param state State structure
 * @return 64-bit hash value
 */
u64 buckets_siphash_final(buckets_siphash_state_t *state);

/**
 * One-shot SipHash-2-4 computation (64-bit output)
 * 
 * Convenience function that init+update+final in one call.
 * 
 * @param k0 First 64 bits of key
 * @param k1 Second 64 bits of key
 * @param data Input data
 * @param len Length of input data
 * @return 64-bit hash value
 */
u64 buckets_siphash(u64 k0, u64 k1, const void *data, size_t len);

/**
 * SipHash-2-4 with 128-bit output (SipHash-128)
 * 
 * Produces a 128-bit hash by using both halves of the internal state.
 * Useful when 64-bit hash space is insufficient.
 * 
 * @param k0 First 64 bits of key
 * @param k1 Second 64 bits of key
 * @param data Input data
 * @param len Length of input data
 * @param out Output buffer (must be at least 16 bytes)
 */
void buckets_siphash128(u64 k0, u64 k1, 
                        const void *data, 
                        size_t len,
                        u8 out[16]);

/* ============================================================================
 * Object Placement Hashing
 * ============================================================================
 * 
 * High-level utilities for mapping object names to erasure sets using SipHash.
 */

/**
 * Hash object name to set index using SipHash modulo
 * 
 * This is the core placement algorithm:
 *   set_index = siphash(deployment_id, object_name) % set_count
 * 
 * The deployment_id (from format.json) is used as the SipHash key
 * to ensure different clusters map objects differently (security).
 * 
 * @param object_name Full object name (bucket/path)
 * @param deployment_id 16-byte deployment UUID from format.json
 * @param set_count Number of erasure sets in the cluster
 * @return Set index (0 to set_count-1), or -1 on error
 */
i32 buckets_hash_object_to_set(const char *object_name,
                               const u8 deployment_id[16],
                               i32 set_count);

/**
 * Hash object name to set index (string UUID variant)
 * 
 * Convenience wrapper that accepts deployment ID as UUID string.
 * 
 * @param object_name Full object name (bucket/path)
 * @param deployment_id_str Deployment UUID string (36 chars)
 * @param set_count Number of erasure sets
 * @return Set index (0 to set_count-1), or -1 on error
 */
i32 buckets_hash_object_to_set_str(const char *object_name,
                                   const char *deployment_id_str,
                                   i32 set_count);

/**
 * Generate a random SipHash key pair
 * 
 * Uses secure random source (/dev/urandom) to generate
 * a 128-bit key for SipHash.
 * 
 * @param k0 Output: first 64 bits of key
 * @param k1 Output: second 64 bits of key
 * @return BUCKETS_OK on success, error code on failure
 */
buckets_error_t buckets_siphash_keygen(u64 *k0, u64 *k1);

/**
 * Parse UUID bytes to SipHash key
 * 
 * Converts 16-byte UUID to two 64-bit key words for SipHash.
 * Uses little-endian encoding to match MinIO behavior.
 * 
 * @param uuid 16-byte UUID
 * @param k0 Output: first 64 bits
 * @param k1 Output: second 64 bits
 */
void buckets_uuid_to_siphash_key(const u8 uuid[16], u64 *k0, u64 *k1);

/**
 * Parse UUID string to SipHash key
 * 
 * Converts UUID string (e.g., "550e8400-e29b-41d4-a716-446655440000")
 * to two 64-bit key words.
 * 
 * @param uuid_str UUID string (36 characters)
 * @param k0 Output: first 64 bits
 * @param k1 Output: second 64 bits
 * @return BUCKETS_OK on success, BUCKETS_ERR_INVALID_ARG if malformed
 */
buckets_error_t buckets_uuid_str_to_siphash_key(const char *uuid_str,
                                                u64 *k0,
                                                u64 *k1);

/* ============================================================================
 * xxHash-64 - High-Speed Non-Cryptographic Hash
 * ============================================================================
 * 
 * xxHash is an extremely fast hash algorithm designed for speed.
 * 
 * Properties:
 * - Very fast: 10+ GB/s on modern CPUs (6-7x faster than SipHash)
 * - Good distribution: passes all SMHasher tests
 * - Not cryptographic: susceptible to collision attacks
 * - Deterministic: same input = same output
 * - Portable: works on all platforms
 * 
 * Use cases:
 * - Data integrity checks (checksums)
 * - Hash tables and bloom filters
 * - Fast content addressing
 * - Non-security-critical hashing
 * 
 * Reference: https://github.com/Cyan4973/xxHash
 */

/**
 * xxHash state structure
 * 
 * Internal state for incremental hashing.
 */
typedef struct buckets_xxhash_state {
    u64 v1, v2, v3, v4;     /* Accumulators */
    u64 total_len;          /* Total bytes processed */
    u64 seed;               /* Seed value */
    u8 buf[32];             /* Buffer for partial blocks */
    size_t buf_len;         /* Bytes in buffer */
} buckets_xxhash_state_t;

/**
 * Initialize xxHash state with seed
 * 
 * @param state State structure to initialize
 * @param seed Seed value (use 0 for default)
 */
void buckets_xxhash_init(buckets_xxhash_state_t *state, u64 seed);

/**
 * Update xxHash state with data
 * 
 * Can be called multiple times to hash data incrementally.
 * 
 * @param state State structure
 * @param data Input data
 * @param len Length of input data
 */
void buckets_xxhash_update(buckets_xxhash_state_t *state,
                           const void *data,
                           size_t len);

/**
 * Finalize xxHash and produce 64-bit hash
 * 
 * @param state State structure
 * @return 64-bit hash value
 */
u64 buckets_xxhash_final(buckets_xxhash_state_t *state);

/**
 * One-shot xxHash-64 computation
 * 
 * Convenience function that init+update+final in one call.
 * 
 * @param seed Seed value (use 0 for default)
 * @param data Input data
 * @param len Length of input data
 * @return 64-bit hash value
 */
u64 buckets_xxhash64(u64 seed, const void *data, size_t len);

/**
 * Fast checksum for data blocks
 * 
 * Computes xxHash-64 with seed=0 for data integrity checks.
 * 
 * @param data Input data
 * @param len Length of input data
 * @return 64-bit checksum
 */
u64 buckets_checksum(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_HASH_H */
