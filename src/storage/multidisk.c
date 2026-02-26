/**
 * Multi-Disk Management
 * 
 * Integrates storage layer with cluster topology for multi-disk operations.
 * Implements quorum-based reads/writes and automatic healing.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_storage.h"
#include "buckets_io.h"

/**
 * Disk set - represents all disks in an erasure set
 */
typedef struct {
    char **disk_paths;          /* Array of disk paths */
    char **disk_uuids;          /* Array of disk UUIDs */
    int disk_count;             /* Number of disks in set */
    int set_index;              /* Set index */
    bool *disk_online;          /* Online status per disk */
} disk_set_t;

/**
 * Multi-disk context - manages all disks in the cluster
 */
typedef struct {
    disk_set_t *sets;           /* Array of disk sets */
    int set_count;              /* Number of sets */
    
    buckets_format_t *format;   /* Cluster format (loaded from disk) */
    buckets_cluster_topology_t *topology;  /* Cluster topology */
    
    pthread_rwlock_t lock;      /* Protects disk online status */
} multidisk_ctx_t;

/* Global multi-disk context */
static multidisk_ctx_t *g_multidisk_ctx = NULL;

/**
 * Initialize multi-disk context from disk paths
 * 
 * @param disk_paths Array of disk mount paths
 * @param disk_count Number of disks
 * @return 0 on success, -1 on error
 */
int buckets_multidisk_init(const char **disk_paths, int disk_count)
{
    if (!disk_paths || disk_count <= 0) {
        buckets_error("Invalid parameters for multidisk_init");
        return -1;
    }
    
    if (g_multidisk_ctx) {
        buckets_warn("Multi-disk context already initialized");
        return 0;
    }
    
    buckets_info("Initializing multi-disk system with %d disks", disk_count);
    
    /* Allocate context */
    g_multidisk_ctx = buckets_malloc(sizeof(multidisk_ctx_t));
    memset(g_multidisk_ctx, 0, sizeof(multidisk_ctx_t));
    
    /* Load format from first disk */
    buckets_format_t *format = NULL;
    for (int i = 0; i < disk_count; i++) {
        format = buckets_format_load(disk_paths[i]);
        if (format) {
            buckets_info("Loaded format from disk: %s", disk_paths[i]);
            break;
        }
    }
    
    if (!format) {
        buckets_error("Failed to load format from any disk");
        buckets_free(g_multidisk_ctx);
        g_multidisk_ctx = NULL;
        return -1;
    }
    
    g_multidisk_ctx->format = format;
    g_multidisk_ctx->set_count = format->erasure.set_count;
    
    /* Allocate disk sets */
    g_multidisk_ctx->sets = buckets_calloc(g_multidisk_ctx->set_count,
                                           sizeof(disk_set_t));
    
    /* Organize disks into sets based on format.json */
    for (int set_idx = 0; set_idx < format->erasure.set_count; set_idx++) {
        disk_set_t *set = &g_multidisk_ctx->sets[set_idx];
        set->set_index = set_idx;
        set->disk_count = format->erasure.disks_per_set;
        set->disk_paths = buckets_calloc(set->disk_count, sizeof(char*));
        set->disk_uuids = buckets_calloc(set->disk_count, sizeof(char*));
        set->disk_online = buckets_calloc(set->disk_count, sizeof(bool));
        
        /* Map disk UUIDs to paths */
        for (int disk_idx = 0; disk_idx < set->disk_count; disk_idx++) {
            const char *disk_uuid = format->erasure.sets[set_idx][disk_idx];
            set->disk_uuids[disk_idx] = buckets_strdup(disk_uuid);
            
            /* Find matching disk path */
            for (int i = 0; i < disk_count; i++) {
                buckets_format_t *disk_format = buckets_format_load(disk_paths[i]);
                if (disk_format && strcmp(disk_format->erasure.this_disk, disk_uuid) == 0) {
                    set->disk_paths[disk_idx] = buckets_strdup(disk_paths[i]);
                    set->disk_online[disk_idx] = true;
                    buckets_format_free(disk_format);
                    buckets_info("Set %d, Disk %d: %s (UUID: %.8s...)", 
                                set_idx, disk_idx, disk_paths[i], disk_uuid);
                    break;
                }
                if (disk_format) {
                    buckets_format_free(disk_format);
                }
            }
            
            if (!set->disk_paths[disk_idx]) {
                buckets_warn("Disk UUID %s not found in provided paths (offline)", disk_uuid);
                set->disk_online[disk_idx] = false;
            }
        }
    }
    
    /* Load topology */
    g_multidisk_ctx->topology = buckets_topology_load(disk_paths[0]);
    if (!g_multidisk_ctx->topology) {
        buckets_warn("No topology found, creating from format");
        g_multidisk_ctx->topology = buckets_topology_from_format(format);
    }
    
    /* Initialize rwlock */
    pthread_rwlock_init(&g_multidisk_ctx->lock, NULL);
    
    buckets_info("Multi-disk initialization complete: %d sets, %d disks per set",
                 g_multidisk_ctx->set_count, format->erasure.disks_per_set);
    
    return 0;
}

/**
 * Cleanup multi-disk context
 */
void buckets_multidisk_cleanup(void)
{
    if (!g_multidisk_ctx) {
        return;
    }
    
    pthread_rwlock_wrlock(&g_multidisk_ctx->lock);
    
    /* Free disk sets */
    for (int i = 0; i < g_multidisk_ctx->set_count; i++) {
        disk_set_t *set = &g_multidisk_ctx->sets[i];
        for (int j = 0; j < set->disk_count; j++) {
            if (set->disk_paths[j]) buckets_free(set->disk_paths[j]);
            if (set->disk_uuids[j]) buckets_free(set->disk_uuids[j]);
        }
        buckets_free(set->disk_paths);
        buckets_free(set->disk_uuids);
        buckets_free(set->disk_online);
    }
    buckets_free(g_multidisk_ctx->sets);
    
    /* Free format and topology */
    if (g_multidisk_ctx->format) {
        buckets_format_free(g_multidisk_ctx->format);
    }
    if (g_multidisk_ctx->topology) {
        buckets_topology_free(g_multidisk_ctx->topology);
    }
    
    pthread_rwlock_unlock(&g_multidisk_ctx->lock);
    pthread_rwlock_destroy(&g_multidisk_ctx->lock);
    
    buckets_free(g_multidisk_ctx);
    g_multidisk_ctx = NULL;
    
    buckets_info("Multi-disk cleanup complete");
}

/**
 * Get disk paths for a specific set
 * 
 * @param set_index Set index
 * @param disk_paths Output array of disk paths (caller allocates)
 * @param max_disks Maximum number of disks
 * @return Number of disks returned
 */
int buckets_multidisk_get_set_disks(int set_index, char **disk_paths, int max_disks)
{
    if (!g_multidisk_ctx || set_index < 0 || set_index >= g_multidisk_ctx->set_count) {
        return 0;
    }
    
    pthread_rwlock_rdlock(&g_multidisk_ctx->lock);
    
    disk_set_t *set = &g_multidisk_ctx->sets[set_index];
    int count = 0;
    
    for (int i = 0; i < set->disk_count && count < max_disks; i++) {
        if (set->disk_online[i] && set->disk_paths[i]) {
            disk_paths[count++] = set->disk_paths[i];
        }
    }
    
    pthread_rwlock_unlock(&g_multidisk_ctx->lock);
    
    return count;
}

/**
 * Read xl.meta with quorum from multiple disks
 * 
 * Reads from all available disks in set and returns if quorum (majority) agree.
 * 
 * @param set_index Set index
 * @param object_path Object path (relative)
 * @param meta Output metadata
 * @return 0 on success, -1 on error
 */
int buckets_multidisk_read_xl_meta(int set_index, const char *object_path,
                                    buckets_xl_meta_t *meta)
{
    if (!g_multidisk_ctx || !object_path || !meta) {
        buckets_error("Invalid parameters for multidisk_read_xl_meta");
        return -1;
    }
    
    if (set_index < 0 || set_index >= g_multidisk_ctx->set_count) {
        buckets_error("Invalid set index: %d", set_index);
        return -1;
    }
    
    pthread_rwlock_rdlock(&g_multidisk_ctx->lock);
    
    disk_set_t *set = &g_multidisk_ctx->sets[set_index];
    int required_quorum = (set->disk_count / 2) + 1;
    
    /* Read from all online disks */
    buckets_xl_meta_t *metas = buckets_calloc(set->disk_count, sizeof(buckets_xl_meta_t));
    int *success = buckets_calloc(set->disk_count, sizeof(int));
    int success_count = 0;
    
    for (int i = 0; i < set->disk_count; i++) {
        if (!set->disk_online[i] || !set->disk_paths[i]) {
            continue;
        }
        
        if (buckets_read_xl_meta(set->disk_paths[i], object_path, &metas[i]) == 0) {
            success[i] = 1;
            success_count++;
        }
    }
    
    pthread_rwlock_unlock(&g_multidisk_ctx->lock);
    
    /* Check quorum */
    if (success_count < required_quorum) {
        buckets_error("Failed to achieve quorum: %d/%d (need %d)",
                     success_count, set->disk_count, required_quorum);
        buckets_free(metas);
        buckets_free(success);
        return -1;
    }
    
    /* Return first successful read (TODO: validate consistency) */
    for (int i = 0; i < set->disk_count; i++) {
        if (success[i]) {
            *meta = metas[i];
            /* Note: Not freeing metas[i] since we're transferring ownership */
            break;
        }
    }
    
    /* Free remaining metadata */
    for (int i = 0; i < set->disk_count; i++) {
        if (success[i] && i > 0) {  /* Skip first one we returned */
            buckets_xl_meta_free(&metas[i]);
        }
    }
    
    buckets_free(metas);
    buckets_free(success);
    
    buckets_debug("Quorum read successful: %d/%d disks", success_count, set->disk_count);
    return 0;
}

/**
 * Write xl.meta with quorum to multiple disks
 * 
 * Writes to all available disks in set and succeeds if quorum (majority) succeed.
 * 
 * @param set_index Set index
 * @param object_path Object path (relative)
 * @param meta Metadata to write
 * @return 0 on success, -1 on error
 */
int buckets_multidisk_write_xl_meta(int set_index, const char *object_path,
                                     const buckets_xl_meta_t *meta)
{
    if (!g_multidisk_ctx || !object_path || !meta) {
        buckets_error("Invalid parameters for multidisk_write_xl_meta");
        return -1;
    }
    
    if (set_index < 0 || set_index >= g_multidisk_ctx->set_count) {
        buckets_error("Invalid set index: %d", set_index);
        return -1;
    }
    
    pthread_rwlock_rdlock(&g_multidisk_ctx->lock);
    
    disk_set_t *set = &g_multidisk_ctx->sets[set_index];
    int required_quorum = (set->disk_count / 2) + 1;
    int success_count = 0;
    
    /* Write to all online disks */
    for (int i = 0; i < set->disk_count; i++) {
        if (!set->disk_online[i] || !set->disk_paths[i]) {
            continue;
        }
        
        if (buckets_write_xl_meta(set->disk_paths[i], object_path, meta) == 0) {
            success_count++;
        } else {
            buckets_warn("Failed to write xl.meta to disk %d (%s)", 
                        i, set->disk_paths[i]);
        }
    }
    
    pthread_rwlock_unlock(&g_multidisk_ctx->lock);
    
    /* Check quorum */
    if (success_count < required_quorum) {
        buckets_error("Failed to achieve write quorum: %d/%d (need %d)",
                     success_count, set->disk_count, required_quorum);
        return -1;
    }
    
    buckets_debug("Quorum write successful: %d/%d disks", success_count, set->disk_count);
    return 0;
}

/**
 * Get online disk count for a set
 * 
 * @param set_index Set index
 * @return Number of online disks, -1 on error
 */
int buckets_multidisk_get_online_count(int set_index)
{
    if (!g_multidisk_ctx || set_index < 0 || set_index >= g_multidisk_ctx->set_count) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&g_multidisk_ctx->lock);
    
    disk_set_t *set = &g_multidisk_ctx->sets[set_index];
    int online_count = 0;
    
    for (int i = 0; i < set->disk_count; i++) {
        if (set->disk_online[i]) {
            online_count++;
        }
    }
    
    pthread_rwlock_unlock(&g_multidisk_ctx->lock);
    
    return online_count;
}

/**
 * Mark disk as offline
 * 
 * @param set_index Set index
 * @param disk_index Disk index within set
 * @return 0 on success, -1 on error
 */
int buckets_multidisk_mark_offline(int set_index, int disk_index)
{
    if (!g_multidisk_ctx || set_index < 0 || set_index >= g_multidisk_ctx->set_count) {
        return -1;
    }
    
    pthread_rwlock_wrlock(&g_multidisk_ctx->lock);
    
    disk_set_t *set = &g_multidisk_ctx->sets[set_index];
    
    if (disk_index < 0 || disk_index >= set->disk_count) {
        pthread_rwlock_unlock(&g_multidisk_ctx->lock);
        return -1;
    }
    
    set->disk_online[disk_index] = false;
    
    pthread_rwlock_unlock(&g_multidisk_ctx->lock);
    
    buckets_warn("Marked disk offline: set=%d, disk=%d", set_index, disk_index);
    return 0;
}

/**
 * Get cluster statistics
 * 
 * @param total_sets Output total sets
 * @param total_disks Output total disks
 * @param online_disks Output online disks
 */
void buckets_multidisk_stats(int *total_sets, int *total_disks, int *online_disks)
{
    if (!g_multidisk_ctx) {
        if (total_sets) *total_sets = 0;
        if (total_disks) *total_disks = 0;
        if (online_disks) *online_disks = 0;
        return;
    }
    
    pthread_rwlock_rdlock(&g_multidisk_ctx->lock);
    
    if (total_sets) {
        *total_sets = g_multidisk_ctx->set_count;
    }
    
    int disks = 0;
    int online = 0;
    
    for (int i = 0; i < g_multidisk_ctx->set_count; i++) {
        disk_set_t *set = &g_multidisk_ctx->sets[i];
        disks += set->disk_count;
        
        for (int j = 0; j < set->disk_count; j++) {
            if (set->disk_online[j]) {
                online++;
            }
        }
    }
    
    if (total_disks) *total_disks = disks;
    if (online_disks) *online_disks = online;
    
    pthread_rwlock_unlock(&g_multidisk_ctx->lock);
}

/**
 * Validate xl.meta consistency across disks
 * 
 * Reads xl.meta from all disks and checks if they match.
 * 
 * @param set_index Set index
 * @param object_path Object path
 * @param inconsistent_disks Output array of inconsistent disk indices (caller allocates)
 * @param max_inconsistent Maximum inconsistent disks to return
 * @return Number of inconsistent disks found, -1 on error
 */
int buckets_multidisk_validate_xl_meta(int set_index, const char *object_path,
                                        int *inconsistent_disks, int max_inconsistent)
{
    if (!g_multidisk_ctx || !object_path) {
        buckets_error("Invalid parameters for validate_xl_meta");
        return -1;
    }
    
    if (set_index < 0 || set_index >= g_multidisk_ctx->set_count) {
        buckets_error("Invalid set index: %d", set_index);
        return -1;
    }
    
    pthread_rwlock_rdlock(&g_multidisk_ctx->lock);
    
    disk_set_t *set = &g_multidisk_ctx->sets[set_index];
    
    /* Read from all online disks */
    buckets_xl_meta_t *metas = buckets_calloc(set->disk_count, sizeof(buckets_xl_meta_t));
    bool *success = buckets_calloc(set->disk_count, sizeof(bool));
    int success_count = 0;
    int first_success = -1;
    
    for (int i = 0; i < set->disk_count; i++) {
        if (!set->disk_online[i] || !set->disk_paths[i]) {
            continue;
        }
        
        if (buckets_read_xl_meta(set->disk_paths[i], object_path, &metas[i]) == 0) {
            success[i] = true;
            success_count++;
            if (first_success == -1) {
                first_success = i;
            }
        }
    }
    
    pthread_rwlock_unlock(&g_multidisk_ctx->lock);
    
    if (success_count < 2) {
        /* Not enough copies to validate */
        buckets_free(metas);
        buckets_free(success);
        return 0;
    }
    
    /* Compare all copies against the first successful one */
    int inconsistent_count = 0;
    buckets_xl_meta_t *reference = &metas[first_success];
    
    for (int i = 0; i < set->disk_count; i++) {
        if (!success[i] || i == first_success) {
            continue;
        }
        
        buckets_xl_meta_t *current = &metas[i];
        
        /* Simple validation: compare size and modTime */
        if (current->stat.size != reference->stat.size ||
            strcmp(current->stat.modTime, reference->stat.modTime) != 0) {
            
            if (inconsistent_disks && inconsistent_count < max_inconsistent) {
                inconsistent_disks[inconsistent_count] = i;
            }
            inconsistent_count++;
            
            buckets_warn("Inconsistent xl.meta on disk %d: size=%zu vs %zu, modTime=%s vs %s",
                        i, current->stat.size, reference->stat.size,
                        current->stat.modTime, reference->stat.modTime);
        }
    }
    
    /* Free metadata */
    for (int i = 0; i < set->disk_count; i++) {
        if (success[i]) {
            buckets_xl_meta_free(&metas[i]);
        }
    }
    buckets_free(metas);
    buckets_free(success);
    
    return inconsistent_count;
}

/**
 * Heal inconsistent xl.meta by copying from healthy disks
 * 
 * @param set_index Set index
 * @param object_path Object path
 * @return Number of disks healed, -1 on error
 */
int buckets_multidisk_heal_xl_meta(int set_index, const char *object_path)
{
    if (!g_multidisk_ctx || !object_path) {
        buckets_error("Invalid parameters for heal_xl_meta");
        return -1;
    }
    
    if (set_index < 0 || set_index >= g_multidisk_ctx->set_count) {
        buckets_error("Invalid set index: %d", set_index);
        return -1;
    }
    
    pthread_rwlock_rdlock(&g_multidisk_ctx->lock);
    
    disk_set_t *set = &g_multidisk_ctx->sets[set_index];
    
    /* Find healthy reference copy via quorum read */
    buckets_xl_meta_t reference_meta;
    int result = buckets_multidisk_read_xl_meta(set_index, object_path, &reference_meta);
    
    if (result != 0) {
        pthread_rwlock_unlock(&g_multidisk_ctx->lock);
        buckets_error("Failed to read healthy xl.meta for healing");
        return -1;
    }
    
    /* Write reference copy to all disks that need healing */
    int healed_count = 0;
    
    for (int i = 0; i < set->disk_count; i++) {
        if (!set->disk_online[i] || !set->disk_paths[i]) {
            continue;
        }
        
        /* Try to read current copy */
        buckets_xl_meta_t current_meta;
        if (buckets_read_xl_meta(set->disk_paths[i], object_path, &current_meta) == 0) {
            /* Check if it matches reference */
            if (current_meta.stat.size == reference_meta.stat.size &&
                strcmp(current_meta.stat.modTime, reference_meta.stat.modTime) == 0) {
                /* Already consistent */
                buckets_xl_meta_free(&current_meta);
                continue;
            }
            buckets_xl_meta_free(&current_meta);
        }
        
        /* Heal: write reference copy */
        if (buckets_write_xl_meta(set->disk_paths[i], object_path, &reference_meta) == 0) {
            healed_count++;
            buckets_info("Healed xl.meta on disk %d (set %d)", i, set_index);
        } else {
            buckets_error("Failed to heal xl.meta on disk %d", i);
        }
    }
    
    pthread_rwlock_unlock(&g_multidisk_ctx->lock);
    
    buckets_xl_meta_free(&reference_meta);
    
    if (healed_count > 0) {
        buckets_info("Healed %d disks for object: %s", healed_count, object_path);
    }
    
    return healed_count;
}

/**
 * Scrub all objects in a set (background verification)
 * 
 * @param set_index Set index
 * @param auto_heal Automatically heal inconsistencies if true
 * @return Number of inconsistencies found, -1 on error
 */
int buckets_multidisk_scrub_set(int set_index, bool auto_heal)
{
    if (!g_multidisk_ctx) {
        buckets_error("Multi-disk context not initialized");
        return -1;
    }
    
    if (set_index < 0 || set_index >= g_multidisk_ctx->set_count) {
        buckets_error("Invalid set index: %d", set_index);
        return -1;
    }
    
    buckets_info("Starting scrub of set %d (auto_heal=%d)", set_index, auto_heal);
    
    /* TODO: Implement full directory traversal and scrubbing
     * This is a placeholder that would:
     * 1. Enumerate all objects on disks in the set
     * 2. Validate xl.meta consistency for each object
     * 3. Optionally heal inconsistencies
     * 4. Verify chunk checksums
     * 5. Report statistics
     */
    
    buckets_warn("Full scrubbing not yet implemented");
    return 0;
}

#pragma GCC diagnostic pop
