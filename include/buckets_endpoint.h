/**
 * Endpoint Parsing and Validation
 * 
 * This module handles parsing of storage endpoint URLs and expansion syntax.
 * Supports both URL-style (http://host:port/path) and path-style (/mnt/disk) endpoints.
 * Implements ellipses expansion for patterns like node{1...4} and disk{a...d}.
 */

#ifndef BUCKETS_ENDPOINT_H
#define BUCKETS_ENDPOINT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "buckets.h"

/* Endpoint Types */
typedef enum {
    BUCKETS_ENDPOINT_TYPE_PATH = 1,  /* Local path: /mnt/disk1 */
    BUCKETS_ENDPOINT_TYPE_URL  = 2   /* Network URL: http://host:port/path */
} buckets_endpoint_type_t;

/* Endpoint Structure */
typedef struct buckets_endpoint {
    char *original;           /* Original endpoint string */
    buckets_endpoint_type_t type;
    
    /* Parsed URL components (for URL-type endpoints) */
    char *scheme;             /* http or https */
    char *host;               /* hostname or IP */
    u16 port;                 /* Port number (0 = default) */
    char *path;               /* Path component */
    
    /* Cluster placement info */
    bool is_local;            /* Is this endpoint on local machine? */
    i32 pool_idx;             /* Pool index (-1 = unassigned) */
    i32 set_idx;              /* Set index (-1 = unassigned) */
    i32 disk_idx;             /* Disk index (-1 = unassigned) */
} buckets_endpoint_t;

/* Endpoint Array */
typedef struct buckets_endpoints {
    buckets_endpoint_t *endpoints;  /* Array of endpoints */
    size_t count;                   /* Number of endpoints */
} buckets_endpoints_t;

/* Expansion Pattern (for ellipses like {1...4}) */
typedef struct buckets_expansion_pattern {
    char *prefix;             /* Text before pattern */
    char *suffix;             /* Text after pattern */
    bool is_numeric;          /* true={1...4}, false={a...d} */
    union {
        struct {
            i64 start;        /* Start number */
            i64 end;          /* End number */
        } numeric;
        struct {
            char start;       /* Start character */
            char end;         /* End character */
        } alpha;
    } range;
} buckets_expansion_pattern_t;

/* Endpoint Set (group of endpoints forming an erasure set) */
typedef struct buckets_endpoint_set {
    buckets_endpoint_t **endpoints;  /* Array of endpoint pointers */
    size_t count;                    /* Number of endpoints in set */
} buckets_endpoint_set_t;

/**
 * Parse a single endpoint string
 * 
 * Supports:
 * - URL format: http://host:port/path or https://host:port/path
 * - Path format: /mnt/disk1
 * 
 * @param endpoint_str Endpoint string to parse
 * @return Parsed endpoint (caller must free with buckets_endpoint_free)
 */
buckets_endpoint_t *buckets_endpoint_parse(const char *endpoint_str);

/**
 * Free endpoint structure
 * 
 * @param endpoint Endpoint to free
 */
void buckets_endpoint_free(buckets_endpoint_t *endpoint);

/**
 * Validate endpoint structure
 * 
 * Checks:
 * - Valid scheme (http/https for URL type)
 * - Non-empty host (for URL type)
 * - Valid port range (1-65535)
 * - Non-empty path
 * 
 * @param endpoint Endpoint to validate
 * @return BUCKETS_OK if valid, error code otherwise
 */
buckets_error_t buckets_endpoint_validate(const buckets_endpoint_t *endpoint);

/**
 * Check if endpoint string contains ellipses pattern
 * 
 * Detects patterns like:
 * - {1...4} (numeric range)
 * - {a...d} (alphabetic range)
 * - Nested: node{1...2}/disk{1...4}
 * 
 * @param str String to check
 * @return true if contains ellipses, false otherwise
 */
bool buckets_endpoint_has_ellipses(const char *str);

/**
 * Parse ellipses pattern from string
 * 
 * Examples:
 * - "node{1...4}" -> prefix="node", numeric range 1-4
 * - "disk{a...d}" -> prefix="disk", alpha range a-d
 * 
 * @param str String containing pattern
 * @return Expansion pattern (caller must free with buckets_expansion_pattern_free)
 */
buckets_expansion_pattern_t *buckets_expansion_pattern_parse(const char *str);

/**
 * Expand ellipses pattern into array of strings
 * 
 * Examples:
 * - "node{1...3}" -> ["node1", "node2", "node3"]
 * - "disk{a...c}" -> ["diska", "diskb", "diskc"]
 * 
 * @param pattern Expansion pattern
 * @param out_count Output: number of expanded strings
 * @return Array of expanded strings (caller must free with buckets_free)
 */
char **buckets_expansion_pattern_expand(const buckets_expansion_pattern_t *pattern,
                                        size_t *out_count);

/**
 * Free expansion pattern
 * 
 * @param pattern Pattern to free
 */
void buckets_expansion_pattern_free(buckets_expansion_pattern_t *pattern);

/**
 * Parse multiple endpoint strings (with expansion support)
 * 
 * Handles:
 * - Simple list: ["/mnt/disk1", "/mnt/disk2"]
 * - Ellipses: ["http://node{1...4}:9000/mnt/disk{1...4}"]
 * 
 * @param args Array of endpoint strings
 * @param count Number of strings
 * @return Endpoints structure (caller must free with buckets_endpoints_free)
 */
buckets_endpoints_t *buckets_endpoints_parse(const char **args, size_t count);

/**
 * Free endpoints structure
 * 
 * @param endpoints Endpoints to free
 */
void buckets_endpoints_free(buckets_endpoints_t *endpoints);

/**
 * Organize endpoints into erasure sets
 * 
 * Divides endpoints into groups of `disks_per_set` for erasure coding.
 * 
 * @param endpoints All endpoints
 * @param disks_per_set Number of disks per erasure set (e.g., 16)
 * @param out_set_count Output: number of sets created
 * @return Array of endpoint sets (caller must free with buckets_endpoint_sets_free)
 */
buckets_endpoint_set_t *buckets_endpoints_to_sets(const buckets_endpoints_t *endpoints,
                                                   size_t disks_per_set,
                                                   size_t *out_set_count);

/**
 * Free endpoint sets
 * 
 * @param sets Array of sets
 * @param count Number of sets
 */
void buckets_endpoint_sets_free(buckets_endpoint_set_t *sets, size_t count);

/**
 * Convert endpoint to string representation
 * 
 * @param endpoint Endpoint to convert
 * @return String representation (caller must free with buckets_free)
 */
char *buckets_endpoint_to_string(const buckets_endpoint_t *endpoint);

/**
 * Check if two endpoints are equal
 * 
 * @param ep1 First endpoint
 * @param ep2 Second endpoint
 * @return true if equal, false otherwise
 */
bool buckets_endpoint_equal(const buckets_endpoint_t *ep1,
                           const buckets_endpoint_t *ep2);

/**
 * Determine if hostname is localhost
 * 
 * Checks against:
 * - localhost
 * - 127.0.0.1
 * - ::1
 * - Local machine hostname
 * 
 * @param endpoint Endpoint to check
 * @return true if local, false if remote
 */
bool buckets_endpoint_is_local(const buckets_endpoint_t *endpoint);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_ENDPOINT_H */
