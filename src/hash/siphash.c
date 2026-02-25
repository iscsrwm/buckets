/**
 * SipHash-2-4 Implementation
 * 
 * Based on the reference implementation by Jean-Philippe Aumasson and Daniel J. Bernstein.
 * Reference: https://131002.net/siphash/
 * 
 * SipHash-2-4 is a fast, cryptographically strong PRF optimized for short inputs.
 * It's used in Buckets for deterministic object placement across erasure sets.
 */

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_hash.h"

/* SipHash rotation */
#define ROTL64(x, b) (u64)(((x) << (b)) | ((x) >> (64 - (b))))

/* SipRound: the core mixing function */
#define SIPROUND do { \
    v0 += v1; v1 = ROTL64(v1, 13); v1 ^= v0; v0 = ROTL64(v0, 32); \
    v2 += v3; v3 = ROTL64(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = ROTL64(v3, 21); v3 ^= v0; \
    v2 += v1; v1 = ROTL64(v1, 17); v1 ^= v2; v2 = ROTL64(v2, 32); \
} while (0)

/* Read 64-bit little-endian integer */
static inline u64 load_le64(const u8 *p)
{
    return ((u64)p[0])       | ((u64)p[1] << 8)  |
           ((u64)p[2] << 16) | ((u64)p[3] << 24) |
           ((u64)p[4] << 32) | ((u64)p[5] << 40) |
           ((u64)p[6] << 48) | ((u64)p[7] << 56);
}

/* Write 64-bit little-endian integer */
static inline void store_le64(u8 *p, u64 v)
{
    p[0] = (u8)(v);
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
    p[4] = (u8)(v >> 32);
    p[5] = (u8)(v >> 40);
    p[6] = (u8)(v >> 48);
    p[7] = (u8)(v >> 56);
}

void buckets_siphash_init(buckets_siphash_state_t *state, u64 k0, u64 k1)
{
    if (!state) {
        return;
    }
    
    /* Initialize state with key */
    state->k0 = k0;
    state->k1 = k1;
    
    /* Initialize internal state */
    state->v0 = k0 ^ 0x736f6d6570736575ULL;  /* "somepseu" */
    state->v1 = k1 ^ 0x646f72616e646f6dULL;  /* "dorandom" */
    state->v2 = k0 ^ 0x6c7967656e657261ULL;  /* "lygenera" */
    state->v3 = k1 ^ 0x7465646279746573ULL;  /* "tedbytes" */
    
    state->total_len = 0;
    state->buf_len = 0;
}

void buckets_siphash_update(buckets_siphash_state_t *state,
                            const void *data,
                            size_t len)
{
    if (!state || !data) {
        return;
    }
    
    const u8 *in = (const u8 *)data;
    u64 v0 = state->v0;
    u64 v1 = state->v1;
    u64 v2 = state->v2;
    u64 v3 = state->v3;
    
    state->total_len += len;
    
    /* Handle buffered data first */
    if (state->buf_len > 0) {
        size_t to_copy = 8 - state->buf_len;
        if (to_copy > len) {
            to_copy = len;
        }
        
        memcpy(state->buf + state->buf_len, in, to_copy);
        state->buf_len += to_copy;
        in += to_copy;
        len -= to_copy;
        
        if (state->buf_len == 8) {
            /* Process buffered block */
            u64 m = load_le64(state->buf);
            v3 ^= m;
            SIPROUND; SIPROUND;  /* c=2 compression rounds */
            v0 ^= m;
            state->buf_len = 0;
        }
    }
    
    /* Process full 8-byte blocks */
    while (len >= 8) {
        u64 m = load_le64(in);
        v3 ^= m;
        SIPROUND; SIPROUND;  /* c=2 compression rounds */
        v0 ^= m;
        
        in += 8;
        len -= 8;
    }
    
    /* Buffer remaining bytes */
    if (len > 0) {
        memcpy(state->buf, in, len);
        state->buf_len = len;
    }
    
    /* Save state */
    state->v0 = v0;
    state->v1 = v1;
    state->v2 = v2;
    state->v3 = v3;
}

u64 buckets_siphash_final(buckets_siphash_state_t *state)
{
    if (!state) {
        return 0;
    }
    
    u64 v0 = state->v0;
    u64 v1 = state->v1;
    u64 v2 = state->v2;
    u64 v3 = state->v3;
    
    /* Prepare final block with length encoding */
    u64 b = (state->total_len & 0xff) << 56;
    
    /* Add buffered bytes to final block */
    switch (state->buf_len) {
        case 7: b |= ((u64)state->buf[6]) << 48; /* fallthrough */
        case 6: b |= ((u64)state->buf[5]) << 40; /* fallthrough */
        case 5: b |= ((u64)state->buf[4]) << 32; /* fallthrough */
        case 4: b |= ((u64)state->buf[3]) << 24; /* fallthrough */
        case 3: b |= ((u64)state->buf[2]) << 16; /* fallthrough */
        case 2: b |= ((u64)state->buf[1]) << 8;  /* fallthrough */
        case 1: b |= ((u64)state->buf[0]);       /* fallthrough */
        case 0: break;
    }
    
    /* Final compression */
    v3 ^= b;
    SIPROUND; SIPROUND;  /* c=2 compression rounds */
    v0 ^= b;
    
    /* Finalization */
    v2 ^= 0xff;
    SIPROUND; SIPROUND; SIPROUND; SIPROUND;  /* d=4 finalization rounds */
    
    return v0 ^ v1 ^ v2 ^ v3;
}

u64 buckets_siphash(u64 k0, u64 k1, const void *data, size_t len)
{
    buckets_siphash_state_t state;
    buckets_siphash_init(&state, k0, k1);
    buckets_siphash_update(&state, data, len);
    return buckets_siphash_final(&state);
}

void buckets_siphash128(u64 k0, u64 k1,
                        const void *data,
                        size_t len,
                        u8 out[16])
{
    if (!out) {
        return;
    }
    
    buckets_siphash_state_t state;
    buckets_siphash_init(&state, k0, k1);
    
    /* Modify initial state for 128-bit output */
    state.v1 ^= 0xee;  /* XOR to enable 128-bit mode */
    
    buckets_siphash_update(&state, data, len);
    
    u64 v0 = state.v0;
    u64 v1 = state.v1;
    u64 v2 = state.v2;
    u64 v3 = state.v3;
    
    /* Prepare final block */
    u64 b = (state.total_len & 0xff) << 56;
    switch (state.buf_len) {
        case 7: b |= ((u64)state.buf[6]) << 48; /* fallthrough */
        case 6: b |= ((u64)state.buf[5]) << 40; /* fallthrough */
        case 5: b |= ((u64)state.buf[4]) << 32; /* fallthrough */
        case 4: b |= ((u64)state.buf[3]) << 24; /* fallthrough */
        case 3: b |= ((u64)state.buf[2]) << 16; /* fallthrough */
        case 2: b |= ((u64)state.buf[1]) << 8;  /* fallthrough */
        case 1: b |= ((u64)state.buf[0]);       /* fallthrough */
        case 0: break;
    }
    
    v3 ^= b;
    SIPROUND; SIPROUND;
    v0 ^= b;
    
    /* First half finalization */
    v2 ^= 0xee;  /* Different constant for 128-bit mode */
    SIPROUND; SIPROUND; SIPROUND; SIPROUND;
    u64 h0 = v0 ^ v1 ^ v2 ^ v3;
    
    /* Second half finalization */
    v1 ^= 0xdd;
    SIPROUND; SIPROUND; SIPROUND; SIPROUND;
    u64 h1 = v0 ^ v1 ^ v2 ^ v3;
    
    /* Output 128-bit hash */
    store_le64(out, h0);
    store_le64(out + 8, h1);
}

i32 buckets_hash_object_to_set(const char *object_name,
                               const u8 deployment_id[16],
                               i32 set_count)
{
    if (!object_name || !deployment_id || set_count <= 0) {
        return -1;
    }
    
    /* Extract key from deployment ID */
    u64 k0 = load_le64(deployment_id);
    u64 k1 = load_le64(deployment_id + 8);
    
    /* Hash object name */
    u64 hash = buckets_siphash(k0, k1, object_name, strlen(object_name));
    
    /* Map to set using modulo */
    return (i32)(hash % (u64)set_count);
}

i32 buckets_hash_object_to_set_str(const char *object_name,
                                   const char *deployment_id_str,
                                   i32 set_count)
{
    if (!deployment_id_str) {
        return -1;
    }
    
    u64 k0, k1;
    buckets_error_t err = buckets_uuid_str_to_siphash_key(deployment_id_str, &k0, &k1);
    if (err != BUCKETS_OK) {
        return -1;
    }
    
    u8 uuid[16];
    store_le64(uuid, k0);
    store_le64(uuid + 8, k1);
    
    return buckets_hash_object_to_set(object_name, uuid, set_count);
}

buckets_error_t buckets_siphash_keygen(u64 *k0, u64 *k1)
{
    if (!k0 || !k1) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Read 16 bytes from /dev/urandom */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        buckets_error("Failed to open /dev/urandom");
        return BUCKETS_ERR_IO;
    }
    
    u8 buf[16];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    
    if (n != sizeof(buf)) {
        buckets_error("Failed to read random bytes");
        return BUCKETS_ERR_IO;
    }
    
    *k0 = load_le64(buf);
    *k1 = load_le64(buf + 8);
    
    return BUCKETS_OK;
}

void buckets_uuid_to_siphash_key(const u8 uuid[16], u64 *k0, u64 *k1)
{
    if (!uuid || !k0 || !k1) {
        return;
    }
    
    *k0 = load_le64(uuid);
    *k1 = load_le64(uuid + 8);
}

buckets_error_t buckets_uuid_str_to_siphash_key(const char *uuid_str,
                                                u64 *k0,
                                                u64 *k1)
{
    if (!uuid_str || !k0 || !k1) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars) */
    if (strlen(uuid_str) != 36) {
        buckets_error("Invalid UUID string length");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Parse UUID string to bytes */
    u8 uuid[16];
    const char *p = uuid_str;
    
    for (int i = 0; i < 16; i++) {
        /* Skip hyphens */
        if (*p == '-') {
            p++;
        }
        
        /* Parse two hex digits */
        int hi = (*p >= '0' && *p <= '9') ? (*p - '0') :
                 (*p >= 'a' && *p <= 'f') ? (*p - 'a' + 10) :
                 (*p >= 'A' && *p <= 'F') ? (*p - 'A' + 10) : -1;
        p++;
        
        int lo = (*p >= '0' && *p <= '9') ? (*p - '0') :
                 (*p >= 'a' && *p <= 'f') ? (*p - 'a' + 10) :
                 (*p >= 'A' && *p <= 'F') ? (*p - 'A' + 10) : -1;
        p++;
        
        if (hi < 0 || lo < 0) {
            buckets_error("Invalid hex digit in UUID string");
            return BUCKETS_ERR_INVALID_ARG;
        }
        
        uuid[i] = (u8)((hi << 4) | lo);
    }
    
    buckets_uuid_to_siphash_key(uuid, k0, k1);
    return BUCKETS_OK;
}
