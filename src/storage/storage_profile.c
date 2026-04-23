/**
 * Storage Layer Profiling Implementation
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "buckets.h"
#include "storage_profile.h"

storage_profile_t g_storage_profile;

void storage_profile_init(void) {
    memset(&g_storage_profile, 0, sizeof(g_storage_profile));
    pthread_mutex_init(&g_storage_profile.lock, NULL);
    clock_gettime(CLOCK_MONOTONIC, &g_storage_profile.last_snapshot);
}

void storage_profile_put_start(void) {
    pthread_mutex_lock(&g_storage_profile.lock);
    g_storage_profile.total_puts++;
    pthread_mutex_unlock(&g_storage_profile.lock);
}

void storage_profile_put_erasure_encode(uint64_t time_us) {
    pthread_mutex_lock(&g_storage_profile.lock);
    g_storage_profile.put_erasure_encode_time_sum += time_us;
    g_storage_profile.put_erasure_encode_count++;
    pthread_mutex_unlock(&g_storage_profile.lock);
}

void storage_profile_put_local_write(uint64_t time_us) {
    pthread_mutex_lock(&g_storage_profile.lock);
    g_storage_profile.put_local_write_time_sum += time_us;
    g_storage_profile.put_local_write_count++;
    pthread_mutex_unlock(&g_storage_profile.lock);
}

void storage_profile_put_remote_write(uint64_t time_us) {
    pthread_mutex_lock(&g_storage_profile.lock);
    g_storage_profile.put_remote_write_time_sum += time_us;
    g_storage_profile.put_remote_write_count++;
    pthread_mutex_unlock(&g_storage_profile.lock);
}

void storage_profile_put_fsync(uint64_t time_us) {
    pthread_mutex_lock(&g_storage_profile.lock);
    g_storage_profile.put_fsync_time_sum += time_us;
    g_storage_profile.put_fsync_count++;
    pthread_mutex_unlock(&g_storage_profile.lock);
}

void storage_profile_put_metadata_write(uint64_t time_us) {
    pthread_mutex_lock(&g_storage_profile.lock);
    g_storage_profile.put_metadata_write_time_sum += time_us;
    g_storage_profile.put_metadata_write_count++;
    pthread_mutex_unlock(&g_storage_profile.lock);
}

void storage_profile_put_end(uint64_t total_time_us) {
    pthread_mutex_lock(&g_storage_profile.lock);
    g_storage_profile.put_total_time_sum += total_time_us;
    g_storage_profile.put_total_time_count++;
    pthread_mutex_unlock(&g_storage_profile.lock);
}

void storage_profile_get_read(uint64_t time_us) {
    pthread_mutex_lock(&g_storage_profile.lock);
    g_storage_profile.total_gets++;
    g_storage_profile.get_read_time_sum += time_us;
    g_storage_profile.get_read_count++;
    pthread_mutex_unlock(&g_storage_profile.lock);
}

void storage_profile_get_decode(uint64_t time_us) {
    pthread_mutex_lock(&g_storage_profile.lock);
    g_storage_profile.get_decode_time_sum += time_us;
    g_storage_profile.get_decode_count++;
    pthread_mutex_unlock(&g_storage_profile.lock);
}

void storage_profile_snapshot(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    pthread_mutex_lock(&g_storage_profile.lock);
    
    /* Calculate elapsed since last snapshot */
    double elapsed = (now.tv_sec - g_storage_profile.last_snapshot.tv_sec) +
                     (now.tv_nsec - g_storage_profile.last_snapshot.tv_nsec) / 1e9;
    
    if (elapsed < 10.0) {
        pthread_mutex_unlock(&g_storage_profile.lock);
        return;  /* Only print every 10 seconds */
    }
    
    g_storage_profile.last_snapshot = now;
    pthread_mutex_unlock(&g_storage_profile.lock);
    
    storage_profile_print();
}

void storage_profile_print(void) {
    pthread_mutex_lock(&g_storage_profile.lock);
    
    if (g_storage_profile.put_total_time_count == 0) {
        pthread_mutex_unlock(&g_storage_profile.lock);
        return;  /* No data yet */
    }
    
    buckets_info("=== STORAGE PROFILING ===");
    buckets_info("Operations: %lu PUTs, %lu GETs",
                 g_storage_profile.total_puts,
                 g_storage_profile.total_gets);
    
    /* PUT timing breakdown */
    if (g_storage_profile.put_total_time_count > 0) {
        uint64_t avg_total = g_storage_profile.put_total_time_sum / 
                             g_storage_profile.put_total_time_count;
        buckets_info("PUT Total: avg=%lu us (%lu ops)",
                     avg_total, g_storage_profile.put_total_time_count);
        
        if (g_storage_profile.put_erasure_encode_count > 0) {
            uint64_t avg_encode = g_storage_profile.put_erasure_encode_time_sum /
                                  g_storage_profile.put_erasure_encode_count;
            double pct_encode = (double)g_storage_profile.put_erasure_encode_time_sum * 100.0 /
                                g_storage_profile.put_total_time_sum;
            buckets_info("  - Erasure Encode: avg=%lu us (%.1f%% of total)",
                         avg_encode, pct_encode);
        }
        
        if (g_storage_profile.put_local_write_count > 0) {
            uint64_t avg_local = g_storage_profile.put_local_write_time_sum /
                                 g_storage_profile.put_local_write_count;
            double pct_local = (double)g_storage_profile.put_local_write_time_sum * 100.0 /
                               g_storage_profile.put_total_time_sum;
            buckets_info("  - Local Write: avg=%lu us (%.1f%% of total)",
                         avg_local, pct_local);
        }
        
        if (g_storage_profile.put_remote_write_count > 0) {
            uint64_t avg_remote = g_storage_profile.put_remote_write_time_sum /
                                  g_storage_profile.put_remote_write_count;
            double pct_remote = (double)g_storage_profile.put_remote_write_time_sum * 100.0 /
                                g_storage_profile.put_total_time_sum;
            buckets_info("  - Remote Write: avg=%lu us (%.1f%% of total)",
                         avg_remote, pct_remote);
        }
        
        if (g_storage_profile.put_fsync_count > 0) {
            uint64_t avg_fsync = g_storage_profile.put_fsync_time_sum /
                                 g_storage_profile.put_fsync_count;
            double pct_fsync = (double)g_storage_profile.put_fsync_time_sum * 100.0 /
                               g_storage_profile.put_total_time_sum;
            buckets_info("  - fsync/GroupCommit: avg=%lu us (%.1f%% of total)",
                         avg_fsync, pct_fsync);
        }
        
        if (g_storage_profile.put_metadata_write_count > 0) {
            uint64_t avg_meta = g_storage_profile.put_metadata_write_time_sum /
                                g_storage_profile.put_metadata_write_count;
            double pct_meta = (double)g_storage_profile.put_metadata_write_time_sum * 100.0 /
                              g_storage_profile.put_total_time_sum;
            buckets_info("  - Metadata Write: avg=%lu us (%.1f%% of total)",
                         avg_meta, pct_meta);
        }
    }
    
    buckets_info("=========================");
    
    pthread_mutex_unlock(&g_storage_profile.lock);
}
