/**
 * Storage Layer Profiling
 * 
 * Detailed timing breakdown for PUT operations to identify bottlenecks.
 */

#ifndef STORAGE_PROFILE_H
#define STORAGE_PROFILE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>

/* Enable profiling */
#define STORAGE_PROFILING_ENABLED 1

typedef struct {
    /* Operation counts */
    uint64_t total_puts;
    uint64_t total_gets;
    
    /* PUT timing breakdown (microseconds) */
    uint64_t put_erasure_encode_time_sum;
    uint64_t put_erasure_encode_count;
    
    uint64_t put_local_write_time_sum;
    uint64_t put_local_write_count;
    
    uint64_t put_remote_write_time_sum;
    uint64_t put_remote_write_count;
    
    uint64_t put_fsync_time_sum;
    uint64_t put_fsync_count;
    
    uint64_t put_metadata_write_time_sum;
    uint64_t put_metadata_write_count;
    
    uint64_t put_total_time_sum;
    uint64_t put_total_time_count;
    
    /* GET timing breakdown */
    uint64_t get_read_time_sum;
    uint64_t get_read_count;
    
    uint64_t get_decode_time_sum;
    uint64_t get_decode_count;
    
    /* Last snapshot */
    struct timespec last_snapshot;
    
    pthread_mutex_t lock;
} storage_profile_t;

extern storage_profile_t g_storage_profile;

/* Initialize profiling */
void storage_profile_init(void);

/* PUT operation tracking */
void storage_profile_put_start(void);
void storage_profile_put_erasure_encode(uint64_t time_us);
void storage_profile_put_local_write(uint64_t time_us);
void storage_profile_put_remote_write(uint64_t time_us);
void storage_profile_put_fsync(uint64_t time_us);
void storage_profile_put_metadata_write(uint64_t time_us);
void storage_profile_put_end(uint64_t total_time_us);

/* GET operation tracking */
void storage_profile_get_read(uint64_t time_us);
void storage_profile_get_decode(uint64_t time_us);

/* Snapshot and print */
void storage_profile_snapshot(void);
void storage_profile_print(void);

/* Timing helper */
static inline uint64_t storage_profile_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

#endif /* STORAGE_PROFILE_H */
