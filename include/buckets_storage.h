/**
 * Storage Layer API
 * 
 * Object storage operations with erasure coding integration.
 * Implements MinIO-compatible xl.meta format with BLAKE2b checksums.
 */

#ifndef BUCKETS_STORAGE_H
#define BUCKETS_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <limits.h>

#include "buckets.h"
#include "buckets_placement.h"

/* Storage constants */
#define BUCKETS_INLINE_THRESHOLD  (128 * 1024)  /* 128 KB */
#define BUCKETS_MIN_CHUNK_SIZE    (64 * 1024)   /* 64 KB */
#define BUCKETS_MAX_CHUNK_SIZE    (512 * 1024 * 1024)  /* 512 MB - support large files */
#define BUCKETS_HASH_PREFIX_LEN   2             /* 00-ff */
#define BUCKETS_OBJECT_HASH_LEN   16            /* 16 hex chars */
#define BUCKETS_MAX_CHUNKS        32            /* K+M max */

/**
 * Checksum information
 */
typedef struct {
    char algo[16];          /* "BLAKE2b-256" */
    u8 hash[32];            /* Checksum bytes */
} buckets_checksum_t;

/**
 * Object metadata (xl.meta)
 */
typedef struct {
    u32 version;                        /* Format version */
    char format[8];                     /* "xl" */
    
    /* Object statistics */
    struct {
        size_t size;                    /* Object size (bytes) */
        char modTime[32];               /* ISO 8601 timestamp */
    } stat;
    
    /* Erasure coding configuration */
    struct {
        char algorithm[16];             /* "ReedSolomon" */
        u32 data;                       /* K (data chunks) */
        u32 parity;                     /* M (parity chunks) */
        size_t blockSize;               /* Chunk size (bytes) */
        u32 index;                      /* This disk's index (1-based) */
        u32 *distribution;              /* Chunk placement (length: K+M) */
        buckets_checksum_t *checksums;  /* Array of K+M checksums */
    } erasure;
    
    /* S3-compatible metadata */
    struct {
        /* Standard S3 metadata */
        char *content_type;             /* Content-Type header */
        char *etag;                     /* ETag (MD5 or BLAKE2b) */
        char *cache_control;            /* Cache-Control header */
        char *content_disposition;      /* Content-Disposition header */
        char *content_encoding;         /* Content-Encoding header */
        char *content_language;         /* Content-Language header */
        char *expires;                  /* Expires header */
        
        /* User metadata (x-amz-meta-*) */
        char **user_keys;               /* User-defined keys */
        char **user_values;             /* User-defined values */
        u32 user_count;                 /* Number of user metadata entries */
    } meta;
    
    /* Versioning information */
    struct {
        char *versionId;                /* Version ID (UUID) */
        bool isLatest;                  /* Is this the latest version? */
        bool isDeleteMarker;            /* Is this a delete marker? */
        char *deleteMarkerVersionId;    /* Version ID of delete marker */
    } versioning;
    
    /* Inline data for small objects */
    char *inline_data;                  /* Base64-encoded (optional) */
} buckets_xl_meta_t;

/**
 * Storage configuration
 */
typedef struct {
    char *data_dir;                     /* /disk/.buckets/data/ */
    u32 inline_threshold;               /* 128 KB default */
    u32 default_ec_k;                   /* 8 (default) */
    u32 default_ec_m;                   /* 4 (default) */
    bool verify_checksums;              /* true (default) */
} buckets_storage_config_t;

/**
 * Object handle (for reads/writes)
 */
typedef struct {
    char *bucket;
    char *object;
    char path[PATH_MAX];                /* Computed object path */
    buckets_xl_meta_t meta;
    u8 **chunks;                        /* Chunk data (K+M pointers) */
    size_t chunk_size;
    bool is_open;
} buckets_object_handle_t;

/* ===== Storage Initialization ===== */

/**
 * Initialize storage system
 * 
 * @param config Storage configuration
 * @return 0 on success, -1 on error
 */
int buckets_storage_init(const buckets_storage_config_t *config);

/**
 * Cleanup storage system
 */
void buckets_storage_cleanup(void);

/**
 * Get current storage configuration
 */
const buckets_storage_config_t* buckets_storage_get_config(void);

/**
 * Initialize distributed storage RPC handlers
 * 
 * Registers RPC methods for cross-node chunk distribution:
 * - storage.writeChunk: Write chunk to remote node
 * - storage.readChunk: Read chunk from remote node
 * 
 * @param rpc_ctx RPC context
 * @return 0 on success, error code otherwise
 */
struct buckets_rpc_context;  /* Forward declaration */
int buckets_distributed_rpc_init(struct buckets_rpc_context *rpc_ctx);

/**
 * Initialize distributed storage system
 * 
 * Creates RPC context and connection pool for distributed operations.
 * Must be called before using distributed chunk operations.
 * 
 * @return 0 on success, error code otherwise
 */
int buckets_distributed_storage_init(void);

/**
 * Cleanup distributed storage system
 */
void buckets_distributed_storage_cleanup(void);

/**
 * Get RPC context for distributed operations
 * 
 * @return RPC context or NULL if not initialized
 */
struct buckets_rpc_context* buckets_distributed_storage_get_rpc_context(void);

/**
 * Set current node's endpoint
 * 
 * Used to determine if a disk is local or remote.
 * 
 * @param node_endpoint Current node's endpoint (e.g., "http://localhost:9001")
 * @return 0 on success, error code otherwise
 */
int buckets_distributed_set_local_endpoint(const char *node_endpoint);

/**
 * Extract node endpoint from full disk endpoint
 * 
 * Converts "http://node1:9001/mnt/disk1" to "http://node1:9001"
 * 
 * @param disk_endpoint Full disk endpoint
 * @param node_endpoint Output buffer for node endpoint (min 256 bytes)
 * @param size Buffer size
 * @return 0 on success, error code otherwise
 */
int buckets_distributed_extract_node_endpoint(const char *disk_endpoint, char *node_endpoint, size_t size);

/**
 * Check if a disk endpoint is local to this node
 * 
 * @param disk_endpoint Full disk endpoint
 * @return true if local, false if remote
 */
bool buckets_distributed_is_local_disk(const char *disk_endpoint);

/**
 * Write chunk to remote node via RPC
 * 
 * @param peer_endpoint Remote node endpoint (e.g., "http://localhost:9002")
 * @param bucket Bucket name
 * @param object Object key
 * @param chunk_index Chunk index (1-based)
 * @param chunk_data Chunk data
 * @param chunk_size Chunk size
 * @param disk_path Disk path on remote node
 * @return 0 on success, error code otherwise
 */
int buckets_distributed_write_chunk(const char *peer_endpoint,
                                     const char *bucket,
                                     const char *object,
                                     u32 chunk_index,
                                     const void *chunk_data,
                                     size_t chunk_size,
                                     const char *disk_path);

/**
 * Read chunk from remote node via RPC
 * 
 * @param peer_endpoint Remote node endpoint
 * @param bucket Bucket name
 * @param object Object key
 * @param chunk_index Chunk index (1-based)
 * @param chunk_data Output: chunk data (caller must free)
 * @param chunk_size Output: chunk size
 * @param disk_path Disk path on remote node
 * @return 0 on success, error code otherwise
 */
int buckets_distributed_read_chunk(const char *peer_endpoint,
                                    const char *bucket,
                                    const char *object,
                                    u32 chunk_index,
                                    void **chunk_data,
                                    size_t *chunk_size,
                                    const char *disk_path);

/* ===== Object Operations ===== */

/**
 * Put object (write)
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param data Object data
 * @param size Object size
 * @param content_type MIME type (optional, can be NULL)
 * @return 0 on success, -1 on error
 * 
 * Example:
 *   const char *data = "Hello, World!";
 *   buckets_put_object("mybucket", "hello.txt", data, strlen(data), "text/plain");
 */
int buckets_put_object(const char *bucket, const char *object,
                       const void *data, size_t size,
                       const char *content_type);

/**
 * Get object (read)
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param data Output buffer pointer (allocated by function, caller must free)
 * @param size Output size
 * @return 0 on success, -1 on error
 * 
 * Example:
 *   void *data;
 *   size_t size;
 *   if (buckets_get_object("mybucket", "hello.txt", &data, &size) == 0) {
 *       printf("Data: %.*s\n", (int)size, (char*)data);
 *       buckets_free(data);
 *   }
 */
int buckets_get_object(const char *bucket, const char *object,
                       void **data, size_t *size);

/**
 * Get storage data directory
 * 
 * @param data_dir Output buffer for data directory path
 * @param size Size of output buffer
 * @return 0 on success, -1 on error
 */
int buckets_get_data_dir(char *data_dir, size_t size);

/**
 * Delete object
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @return 0 on success, -1 on error
 */
int buckets_delete_object(const char *bucket, const char *object);

/**
 * Head object (metadata only)
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param meta Output metadata (allocated by function, caller must free with xl_meta_free)
 * @return 0 on success, -1 on error
 */
int buckets_head_object(const char *bucket, const char *object,
                        buckets_xl_meta_t *meta);

/**
 * Stat object (size and modification time only)
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param size Output size
 * @param modTime Output modification time (ISO 8601, buffer must be >= 32 bytes)
 * @return 0 on success, -1 on error
 */
int buckets_stat_object(const char *bucket, const char *object,
                        size_t *size, char *modTime);

/* ===== Path Utilities ===== */

/**
 * Compute object path from bucket + object key
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param path Output path buffer
 * @param path_len Path buffer length
 * 
 * Example:
 *   char path[PATH_MAX];
 *   buckets_compute_object_path("mybucket", "photos/2024/img.jpg", path, sizeof(path));
 *   // path = ".buckets/data/a7/a7b3c9d2e5f81234/"
 */
void buckets_compute_object_path(const char *bucket, const char *object,
                                  char *path, size_t path_len);

/**
 * Compute hash prefix (00-ff)
 * 
 * @param hash Object hash
 * @param prefix Output prefix buffer (must be >= 3 bytes)
 */
void buckets_compute_hash_prefix(u64 hash, char *prefix, size_t prefix_len);

/**
 * Compute full object hash (16 hex chars)
 * 
 * @param object_key Object key (bucket/object combined)
 * @param hash Output hash buffer (must be >= 17 bytes)
 */
void buckets_compute_object_hash(const char *object_key, char *hash, size_t hash_len);

/* ===== xl.meta Operations ===== */

/**
 * Read xl.meta from disk
 * 
 * @param disk_path Disk root path
 * @param object_path Object path (relative to disk)
 * @param meta Output metadata
 * @return 0 on success, -1 on error
 */
int buckets_read_xl_meta(const char *disk_path, const char *object_path,
                         buckets_xl_meta_t *meta);

/**
 * Write xl.meta to disk (atomic)
 * 
 * @param disk_path Disk root path
 * @param object_path Object path (relative to disk)
 * @param meta Metadata to write
 * @return 0 on success, -1 on error
 */
int buckets_write_xl_meta(const char *disk_path, const char *object_path,
                          const buckets_xl_meta_t *meta);

/**
 * Free xl.meta resources
 * 
 * @param meta Metadata to free
 */
void buckets_xl_meta_free(buckets_xl_meta_t *meta);

/**
 * Serialize xl.meta to JSON
 * 
 * @param meta Metadata to serialize
 * @return JSON string (caller must free)
 */
char* buckets_xl_meta_to_json(const buckets_xl_meta_t *meta);

/**
 * Deserialize xl.meta from JSON
 * 
 * @param json JSON string
 * @param meta Output metadata
 * @return 0 on success, -1 on error
 */
int buckets_xl_meta_from_json(const char *json, buckets_xl_meta_t *meta);

/* ===== Chunk Operations ===== */

/**
 * Read chunk from disk
 * 
 * @param disk_path Disk root path
 * @param object_path Object path (relative to disk)
 * @param chunk_index Chunk index (1-based)
 * @param data Output data pointer (allocated by function, caller must free)
 * @param size Output size
 * @return 0 on success, -1 on error
 */
int buckets_read_chunk(const char *disk_path, const char *object_path,
                       u32 chunk_index, void **data, size_t *size);

/**
 * Write chunk to disk (atomic)
 * 
 * @param disk_path Disk root path
 * @param object_path Object path (relative to disk)
 * @param chunk_index Chunk index (1-based)
 * @param data Chunk data
 * @param size Chunk size
 * @return 0 on success, -1 on error
 */
int buckets_write_chunk(const char *disk_path, const char *object_path,
                        u32 chunk_index, const void *data, size_t size);

/**
 * Verify chunk checksum
 * 
 * @param data Chunk data
 * @param size Chunk size
 * @param checksum Expected checksum
 * @return true if valid, false otherwise
 */
bool buckets_verify_chunk(const void *data, size_t size, 
                          const buckets_checksum_t *checksum);

/* ===== Erasure Coding Integration ===== */

/**
 * Encode object with erasure coding
 * 
 * @param data Object data
 * @param size Object size
 * @param k Number of data chunks
 * @param m Number of parity chunks
 * @param data_chunks Output data chunks (caller must allocate array)
 * @param parity_chunks Output parity chunks (caller must allocate array)
 * @param chunk_size Output chunk size
 * @return 0 on success, -1 on error
 */
int buckets_encode_object(const void *data, size_t size, u32 k, u32 m,
                          u8 **data_chunks, u8 **parity_chunks, 
                          size_t *chunk_size);

/**
 * Decode object from chunks
 * 
 * @param chunks Array of K+M chunks (NULL for missing chunks)
 * @param k Number of data chunks
 * @param m Number of parity chunks
 * @param chunk_size Chunk size
 * @param data Output data pointer (allocated by function, caller must free)
 * @param size Output size
 * @return 0 on success, -1 on error
 */
int buckets_decode_object(u8 **chunks, u32 k, u32 m, size_t chunk_size,
                          void **data, size_t *size);

/* ===== Helper Functions ===== */

/**
 * Check if object should be inlined
 * 
 * @param size Object size
 * @return true if should inline, false otherwise
 */
bool buckets_should_inline_object(size_t size);

/**
 * Calculate optimal chunk size for object
 * 
 * @param object_size Object size
 * @param k Number of data chunks
 * @return Optimal chunk size
 */
size_t buckets_calculate_chunk_size(size_t object_size, u32 k);

/**
 * Select erasure coding configuration based on cluster size
 * 
 * @param cluster_size Number of disks in cluster
 * @param k Output data chunks
 * @param m Output parity chunks
 */
void buckets_select_erasure_config(u32 cluster_size, u32 *k, u32 *m);

/**
 * Get current time in ISO 8601 format
 * 
 * @param buf Output buffer (must be >= 32 bytes)
 */
void buckets_get_iso8601_time(char *buf);

/* ===== Metadata Functions ===== */

/**
 * Compute ETag for object data
 * 
 * Uses BLAKE2b-256 and returns hex-encoded hash
 * (S3-compatible format)
 * 
 * @param data Object data
 * @param size Object size
 * @param etag Output buffer for ETag (must be >= 65 bytes for hex + quotes)
 * @return 0 on success, -1 on error
 */
int buckets_compute_etag(const void *data, size_t size, char *etag);

/**
 * Add user metadata (x-amz-meta-* header)
 * 
 * @param meta xl.meta structure
 * @param key Metadata key (without x-amz-meta- prefix)
 * @param value Metadata value
 * @return 0 on success, -1 on error
 */
int buckets_add_user_metadata(buckets_xl_meta_t *meta, const char *key, const char *value);

/**
 * Get user metadata value
 * 
 * @param meta xl.meta structure
 * @param key Metadata key (without x-amz-meta- prefix)
 * @return Metadata value, or NULL if not found
 */
const char* buckets_get_user_metadata(const buckets_xl_meta_t *meta, const char *key);

/**
 * Generate version ID (UUID-based)
 * 
 * @param versionId Output buffer (must be >= 37 bytes for UUID string)
 * @return 0 on success, -1 on error
 */
int buckets_generate_version_id(char *versionId);

/**
 * Put object with metadata and versioning
 * 
 * Extended version of buckets_put_object with full metadata support
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param data Object data
 * @param size Object size
 * @param meta Object metadata (content-type, user metadata, etc.)
 * @param enable_versioning Enable versioning for this object
 * @param versionId Output version ID (optional, can be NULL)
 * @return 0 on success, -1 on error
 */
int buckets_put_object_with_metadata(const char *bucket, const char *object,
                                     const void *data, size_t size,
                                     buckets_xl_meta_t *meta,
                                     bool enable_versioning,
                                     char *versionId);

/**
 * Get object by version ID
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Version ID (NULL for latest version)
 * @param data Output data pointer
 * @param size Output size
 * @return 0 on success, -1 on error
 */
int buckets_get_object_version(const char *bucket, const char *object,
                                const char *versionId,
                                void **data, size_t *size);

/**
 * List object versions
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versions Output array of version IDs (caller must free)
 * @param count Output number of versions
 * @return 0 on success, -1 on error
 */
int buckets_list_object_versions(const char *bucket, const char *object,
                                  char ***versions, u32 *count);

/* ===== Versioning Operations (Week 13) ===== */

/**
 * Put object with versioning enabled
 * 
 * Creates a new version of the object. Each version is stored in a separate
 * directory under versions/<versionId>/.
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param data Object data
 * @param size Object size
 * @param meta Object metadata (optional, can be NULL)
 * @param versionId Output version ID (must be >= 37 bytes)
 * @return 0 on success, -1 on error
 */
int buckets_put_object_versioned(const char *bucket, const char *object,
                                  const void *data, size_t size,
                                  buckets_xl_meta_t *meta,
                                  char *versionId);

/**
 * Delete object with versioning (create delete marker)
 * 
 * S3-compatible soft delete: creates a delete marker version rather than
 * actually deleting the object. The object becomes invisible until the
 * delete marker is removed.
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Output version ID of delete marker (must be >= 37 bytes)
 * @return 0 on success, -1 on error
 */
int buckets_delete_object_versioned(const char *bucket, const char *object,
                                     char *versionId);

/**
 * Get object by specific version ID
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Version ID (NULL for latest non-deleted version)
 * @param data Output data pointer (allocated by function, caller must free)
 * @param size Output size
 * @return 0 on success, -1 on error, -2 if version is delete marker
 */
int buckets_get_object_by_version(const char *bucket, const char *object,
                                   const char *versionId,
                                   void **data, size_t *size);

/**
 * List all versions of an object
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versions Output array of version IDs (caller must free each string and array)
 * @param is_delete_markers Output array of delete marker flags (caller must free)
 * @param count Output number of versions
 * @return 0 on success, -1 on error
 */
int buckets_list_versions(const char *bucket, const char *object,
                          char ***versions, bool **is_delete_markers, u32 *count);

/**
 * Delete specific version (hard delete)
 * 
 * Permanently deletes a specific version including delete markers.
 * This is different from creating a delete marker.
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Version ID to delete
 * @return 0 on success, -1 on error
 */
int buckets_delete_version(const char *bucket, const char *object,
                           const char *versionId);

/* ===== Metadata Caching (Week 13) ===== */

/**
 * Initialize metadata cache
 * 
 * @param max_size Maximum number of cached entries (0 for default: 10000)
 * @param ttl_seconds Time-to-live for entries (0 for default: 300 = 5 minutes)
 * @return 0 on success, -1 on error
 */
int buckets_metadata_cache_init(u32 max_size, u32 ttl_seconds);

/**
 * Cleanup metadata cache
 */
void buckets_metadata_cache_cleanup(void);

/**
 * Get metadata from cache
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Version ID (NULL for latest)
 * @param meta Output metadata (filled if cache hit)
 * @return 0 on cache hit, -1 on cache miss
 */
int buckets_metadata_cache_get(const char *bucket, const char *object,
                                const char *versionId,
                                buckets_xl_meta_t *meta);

/**
 * Put metadata into cache
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Version ID (NULL for latest)
 * @param meta Metadata to cache (will be cloned)
 * @return 0 on success, -1 on error
 */
int buckets_metadata_cache_put(const char *bucket, const char *object,
                                const char *versionId,
                                const buckets_xl_meta_t *meta);

/**
 * Invalidate cache entry
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Version ID (NULL for latest)
 * @return 0 on success, -1 if not found
 */
int buckets_metadata_cache_invalidate(const char *bucket, const char *object,
                                       const char *versionId);

/**
 * Get cache statistics
 * 
 * @param hits Output hit count
 * @param misses Output miss count
 * @param evictions Output eviction count
 * @param count Output current entry count
 */
void buckets_metadata_cache_stats(u64 *hits, u64 *misses, u64 *evictions, u32 *count);

/* ===== Multi-Disk Management (Week 14-16) ===== */

/**
 * Initialize multi-disk context from disk paths
 * 
 * Enumerates all disks, loads format.json, and organizes disks into erasure sets.
 * 
 * @param disk_paths Array of disk mount paths
 * @param disk_count Number of disks
 * @return 0 on success, -1 on error
 */
int buckets_multidisk_init(const char **disk_paths, int disk_count);

/**
 * Cleanup multi-disk context
 */
void buckets_multidisk_cleanup(void);

/**
 * Get disk paths for a specific erasure set
 * 
 * @param set_index Set index
 * @param disk_paths Output array of disk paths (caller must allocate)
 * @param max_disks Maximum number of disks
 * @return Number of disks returned
 */
int buckets_multidisk_get_set_disks(int set_index, char **disk_paths, int max_disks);

/**
 * Read xl.meta with quorum from multiple disks
 * 
 * Reads from all available disks in set and succeeds if quorum (majority) agree.
 * 
 * @param set_index Set index
 * @param object_path Object path (relative to disk root)
 * @param meta Output metadata
 * @return 0 on success, -1 on error
 */
int buckets_multidisk_read_xl_meta(int set_index, const char *object_path,
                                    buckets_xl_meta_t *meta);

/**
 * Write xl.meta with quorum to multiple disks
 * 
 * Writes to all available disks in set and succeeds if quorum (majority) succeed.
 * 
 * @param set_index Set index
 * @param object_path Object path (relative to disk root)
 * @param meta Metadata to write
 * @return 0 on success, -1 on error
 */
int buckets_multidisk_write_xl_meta(int set_index, const char *object_path,
                                     const buckets_xl_meta_t *meta);

/**
 * Get online disk count for an erasure set
 * 
 * @param set_index Set index
 * @return Number of online disks, -1 on error
 */
int buckets_multidisk_get_online_count(int set_index);

/**
 * Mark disk as offline (failure detection)
 * 
 * @param set_index Set index
 * @param disk_index Disk index within set
 * @return 0 on success, -1 on error
 */
int buckets_multidisk_mark_offline(int set_index, int disk_index);

/**
 * Get cluster statistics
 * 
 * @param total_sets Output total sets
 * @param total_disks Output total disks
 * @param online_disks Output online disks
 */
void buckets_multidisk_stats(int *total_sets, int *total_disks, int *online_disks);

/**
 * Validate xl.meta consistency across disks
 * 
 * @param set_index Set index
 * @param object_path Object path
 * @param inconsistent_disks Output array of inconsistent disk indices
 * @param max_inconsistent Maximum inconsistent disks to return
 * @return Number of inconsistent disks found, -1 on error
 */
int buckets_multidisk_validate_xl_meta(int set_index, const char *object_path,
                                        int *inconsistent_disks, int max_inconsistent);

/**
 * Heal inconsistent xl.meta by copying from healthy disks
 * 
 * @param set_index Set index
 * @param object_path Object path
 * @return Number of disks healed, -1 on error
 */
int buckets_multidisk_heal_xl_meta(int set_index, const char *object_path);

/**
 * Scrub all objects in a set (background verification)
 * 
 * @param set_index Set index
 * @param auto_heal Automatically heal inconsistencies if true
 * @return Number of inconsistencies found, -1 on error
 */
int buckets_multidisk_scrub_set(int set_index, bool auto_heal);

/* ===================================================================
 * Parallel Chunk Operations
 * ===================================================================*/

/**
 * Write multiple chunks in parallel (local + RPC)
 * 
 * Executes chunk writes concurrently using thread pool for maximum performance.
 * Each chunk is written to its target disk (local or remote via RPC) in parallel.
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param placement Placement result with disk endpoints
 * @param chunk_data_array Array of chunk data pointers (K+M chunks)
 * @param chunk_size Chunk size (same for all chunks)
 * @param num_chunks Total number of chunks (K+M)
 * @return 0 on success, -1 if any chunk failed
 */
int buckets_parallel_write_chunks(const char *bucket,
                                   const char *object,
                                   const char *object_path,
                                   buckets_placement_result_t *placement,
                                   const void **chunk_data_array,
                                   size_t chunk_size,
                                   u32 num_chunks);

/**
 * Read multiple chunks in parallel (local + RPC)
 * 
 * Executes chunk reads concurrently using thread pool for maximum performance.
 * Each chunk is read from its target disk (local or remote via RPC) in parallel.
 * Tolerates failures - returns number of successful reads (need >= K for reconstruction).
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param object_path Hashed object path (prefix/hash/)
 * @param placement Placement result with disk endpoints
 * @param chunk_data_array Output: Array of chunk data pointers (caller must free each)
 * @param chunk_sizes_array Output: Array of chunk sizes
 * @param num_chunks Total number of chunks to read (K+M)
 * @return Number of successfully read chunks (need >= K for reconstruction), -1 on error
 */
int buckets_parallel_read_chunks(const char *bucket,
                                  const char *object,
                                  const char *object_path,
                                  buckets_placement_result_t *placement,
                                  void **chunk_data_array,
                                  size_t *chunk_sizes_array,
                                  u32 num_chunks);

/**
 * Write xl.meta to multiple disks in parallel (local + RPC)
 * 
 * Executes metadata writes concurrently using thread pool for maximum performance.
 * Each metadata write is sent to its target disk (local or remote via RPC) in parallel.
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param object_path Full object path (bucket/object)
 * @param placement Placement result with disk endpoints
 * @param disk_paths Array of disk paths
 * @param base_meta Base metadata (will be copied and index updated for each disk)
 * @param num_disks Number of disks to write to
 * @param has_endpoints True if placement has endpoint information
 * @return 0 on success, -1 on error
 */
int buckets_parallel_write_metadata(const char *bucket,
                                     const char *object,
                                     const char *object_path,
                                     buckets_placement_result_t *placement,
                                     char **disk_paths,
                                     const buckets_xl_meta_t *base_meta,
                                     u32 num_disks,
                                     bool has_endpoints);

/* ===================================================================
 * Binary Chunk Transport
 * 
 * Efficient binary transport for chunk data between nodes.
 * Uses HTTP with raw binary body instead of base64-encoded JSON.
 * ===================================================================*/

/**
 * Write chunk to remote node using binary transport
 * 
 * @param peer_endpoint Remote node endpoint (e.g., "http://localhost:9002")
 * @param bucket Bucket name
 * @param object Object key
 * @param chunk_index Chunk index (1-based)
 * @param chunk_data Chunk data
 * @param chunk_size Chunk size
 * @param disk_path Disk path on remote node
 * @return BUCKETS_OK on success
 */
int buckets_binary_write_chunk(const char *peer_endpoint,
                                const char *bucket,
                                const char *object,
                                u32 chunk_index,
                                const void *chunk_data,
                                size_t chunk_size,
                                const char *disk_path);

/**
 * Read chunk from remote node using binary transport
 * 
 * @param peer_endpoint Remote node endpoint
 * @param bucket Bucket name
 * @param object Object key
 * @param chunk_index Chunk index (1-based)
 * @param chunk_data Output: chunk data (caller must free)
 * @param chunk_size Output: chunk size
 * @param disk_path Disk path on remote node
 * @return BUCKETS_OK on success
 */
int buckets_binary_read_chunk(const char *peer_endpoint,
                               const char *bucket,
                               const char *object,
                               u32 chunk_index,
                               void **chunk_data,
                               size_t *chunk_size,
                               const char *disk_path);

/**
 * Register binary chunk transport handlers with HTTP server
 * 
 * @param server HTTP server handle
 * @return BUCKETS_OK on success
 */
struct buckets_http_server;
typedef struct buckets_http_server buckets_http_server_t;
int buckets_binary_transport_register(buckets_http_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_STORAGE_H */
