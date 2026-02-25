/**
 * xxHash-64 Implementation
 * 
 * Based on the reference implementation by Yann Collet.
 * Reference: https://github.com/Cyan4973/xxHash
 * 
 * xxHash is an extremely fast non-cryptographic hash algorithm.
 * It's used in Buckets for checksums and fast data integrity checks.
 */

#include <string.h>

#include "buckets.h"
#include "buckets_hash.h"

/* xxHash constants */
#define XXHASH_PRIME64_1  0x9E3779B185EBCA87ULL
#define XXHASH_PRIME64_2  0xC2B2AE3D27D4EB4FULL
#define XXHASH_PRIME64_3  0x165667B19E3779F9ULL
#define XXHASH_PRIME64_4  0x85EBCA77C2B2AE63ULL
#define XXHASH_PRIME64_5  0x27D4EB2F165667C5ULL

/* Rotation */
#define ROTL64(x, r) (((x) << (r)) | ((x) >> (64 - (r))))

/* Read 64-bit little-endian */
static inline u64 xxh_read64(const void *ptr)
{
    const u8 *p = (const u8 *)ptr;
    return ((u64)p[0])       | ((u64)p[1] << 8)  |
           ((u64)p[2] << 16) | ((u64)p[3] << 24) |
           ((u64)p[4] << 32) | ((u64)p[5] << 40) |
           ((u64)p[6] << 48) | ((u64)p[7] << 56);
}

/* Read 32-bit little-endian */
static inline u32 xxh_read32(const void *ptr)
{
    const u8 *p = (const u8 *)ptr;
    return ((u32)p[0]) | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* xxHash round */
static inline u64 xxh_round(u64 acc, u64 input)
{
    acc += input * XXHASH_PRIME64_2;
    acc = ROTL64(acc, 31);
    acc *= XXHASH_PRIME64_1;
    return acc;
}

/* Merge accumulator */
static inline u64 xxh_merge_round(u64 acc, u64 val)
{
    val = xxh_round(0, val);
    acc ^= val;
    acc = acc * XXHASH_PRIME64_1 + XXHASH_PRIME64_4;
    return acc;
}

void buckets_xxhash_init(buckets_xxhash_state_t *state, u64 seed)
{
    if (!state) {
        return;
    }
    
    state->seed = seed;
    state->v1 = seed + XXHASH_PRIME64_1 + XXHASH_PRIME64_2;
    state->v2 = seed + XXHASH_PRIME64_2;
    state->v3 = seed;
    state->v4 = seed - XXHASH_PRIME64_1;
    state->total_len = 0;
    state->buf_len = 0;
}

void buckets_xxhash_update(buckets_xxhash_state_t *state,
                           const void *data,
                           size_t len)
{
    if (!state || !data) {
        return;
    }
    
    const u8 *p = (const u8 *)data;
    state->total_len += len;
    
    /* Handle buffered data */
    if (state->buf_len > 0) {
        size_t to_copy = 32 - state->buf_len;
        if (to_copy > len) {
            to_copy = len;
        }
        
        memcpy(state->buf + state->buf_len, p, to_copy);
        state->buf_len += to_copy;
        p += to_copy;
        len -= to_copy;
        
        if (state->buf_len == 32) {
            /* Process buffered block */
            const u8 *buf_p = state->buf;
            state->v1 = xxh_round(state->v1, xxh_read64(buf_p));
            state->v2 = xxh_round(state->v2, xxh_read64(buf_p + 8));
            state->v3 = xxh_round(state->v3, xxh_read64(buf_p + 16));
            state->v4 = xxh_round(state->v4, xxh_read64(buf_p + 24));
            state->buf_len = 0;
        }
    }
    
    /* Process full 32-byte blocks */
    while (len >= 32) {
        state->v1 = xxh_round(state->v1, xxh_read64(p));
        state->v2 = xxh_round(state->v2, xxh_read64(p + 8));
        state->v3 = xxh_round(state->v3, xxh_read64(p + 16));
        state->v4 = xxh_round(state->v4, xxh_read64(p + 24));
        
        p += 32;
        len -= 32;
    }
    
    /* Buffer remaining bytes */
    if (len > 0) {
        memcpy(state->buf, p, len);
        state->buf_len = len;
    }
}

u64 buckets_xxhash_final(buckets_xxhash_state_t *state)
{
    if (!state) {
        return 0;
    }
    
    u64 h64;
    
    if (state->total_len >= 32) {
        /* Mix accumulators */
        h64 = ROTL64(state->v1, 1) + ROTL64(state->v2, 7) +
              ROTL64(state->v3, 12) + ROTL64(state->v4, 18);
        
        h64 = xxh_merge_round(h64, state->v1);
        h64 = xxh_merge_round(h64, state->v2);
        h64 = xxh_merge_round(h64, state->v3);
        h64 = xxh_merge_round(h64, state->v4);
    } else {
        /* Short input */
        h64 = state->seed + XXHASH_PRIME64_5;
    }
    
    h64 += state->total_len;
    
    /* Process remaining bytes */
    const u8 *p = state->buf;
    size_t len = state->buf_len;
    
    while (len >= 8) {
        u64 k1 = xxh_round(0, xxh_read64(p));
        h64 ^= k1;
        h64 = ROTL64(h64, 27) * XXHASH_PRIME64_1 + XXHASH_PRIME64_4;
        p += 8;
        len -= 8;
    }
    
    if (len >= 4) {
        h64 ^= (u64)(xxh_read32(p)) * XXHASH_PRIME64_1;
        h64 = ROTL64(h64, 23) * XXHASH_PRIME64_2 + XXHASH_PRIME64_3;
        p += 4;
        len -= 4;
    }
    
    while (len > 0) {
        h64 ^= (*p) * XXHASH_PRIME64_5;
        h64 = ROTL64(h64, 11) * XXHASH_PRIME64_1;
        p++;
        len--;
    }
    
    /* Avalanche */
    h64 ^= h64 >> 33;
    h64 *= XXHASH_PRIME64_2;
    h64 ^= h64 >> 29;
    h64 *= XXHASH_PRIME64_3;
    h64 ^= h64 >> 32;
    
    return h64;
}

u64 buckets_xxhash64(u64 seed, const void *data, size_t len)
{
    buckets_xxhash_state_t state;
    buckets_xxhash_init(&state, seed);
    buckets_xxhash_update(&state, data, len);
    return buckets_xxhash_final(&state);
}

u64 buckets_checksum(const void *data, size_t len)
{
    return buckets_xxhash64(0, data, len);
}
