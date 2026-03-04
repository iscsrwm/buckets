/**
 * UUID Generation and Parsing
 * 
 * UUID v4 (random) generation and conversion utilities
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>
#include "buckets.h"
#include "buckets_cluster.h"

void buckets_uuid_generate(char *uuid_str)
{
    uuid_t uuid;
    uuid_generate_random(uuid);
    uuid_unparse_lower(uuid, uuid_str);
}

int buckets_uuid_parse(const char *str, u8 *uuid)
{
    if (!str || !uuid) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    uuid_t temp;
    if (uuid_parse(str, temp) != 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    memcpy(uuid, temp, 16);
    return BUCKETS_OK;
}

void buckets_uuid_to_string(const u8 *uuid, char *str)
{
    uuid_unparse_lower(uuid, str);
}

/**
 * Generate a deterministic UUID from a name string
 * 
 * Uses SHA-256 to hash the name and converts to UUID format.
 * This ensures that the same name always produces the same UUID,
 * which is critical for cluster-wide consistency.
 * 
 * @param name Input name string (e.g., cluster deployment_id from config)
 * @param uuid_str Output buffer for UUID string (min 37 bytes)
 */
void buckets_uuid_generate_from_name(const char *name, char *uuid_str)
{
    if (!name || !uuid_str) {
        /* Fall back to random UUID if invalid input */
        buckets_uuid_generate(uuid_str);
        return;
    }
    
    /* Hash the name using SHA-256 */
    extern int buckets_sha256(void *out, const void *data, size_t datalen);
    u8 hash[32];
    buckets_sha256(hash, name, strlen(name));
    
    /* Convert first 16 bytes of hash to UUID format */
    uuid_t uuid;
    memcpy(uuid, hash, 16);
    
    /* Set version to 5 (name-based SHA-1, we use SHA-256 but same concept) */
    uuid[6] = (uuid[6] & 0x0F) | 0x50;  /* Version 5 */
    uuid[8] = (uuid[8] & 0x3F) | 0x80;  /* Variant RFC 4122 */
    
    uuid_unparse_lower(uuid, uuid_str);
}
