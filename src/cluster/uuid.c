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
