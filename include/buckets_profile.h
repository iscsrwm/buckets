/**
 * Performance Profiling Utilities
 * 
 * Lightweight timing macros for profiling critical paths.
 */

#ifndef BUCKETS_PROFILE_H
#define BUCKETS_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>
#include <stdio.h>

/* Enable profiling by default (can disable with -DBUCKETS_NO_PROFILE) */
#ifndef BUCKETS_NO_PROFILE
#define BUCKETS_PROFILE_ENABLED 1
#endif

#ifdef BUCKETS_PROFILE_ENABLED

#define PROFILE_START(name) \
    struct timespec profile_##name##_start; \
    clock_gettime(CLOCK_MONOTONIC, &profile_##name##_start)

#define PROFILE_END(name, fmt, ...) \
    do { \
        struct timespec profile_##name##_end; \
        clock_gettime(CLOCK_MONOTONIC, &profile_##name##_end); \
        double profile_##name##_ms = \
            (profile_##name##_end.tv_sec - profile_##name##_start.tv_sec) * 1000.0 + \
            (profile_##name##_end.tv_nsec - profile_##name##_start.tv_nsec) / 1e6; \
        buckets_info("[PROFILE][" #name "] %.3f ms - " fmt, profile_##name##_ms, ##__VA_ARGS__); \
    } while (0)

#define PROFILE_CHECKPOINT(name, checkpoint_name, fmt, ...) \
    do { \
        struct timespec profile_##name##_checkpoint; \
        clock_gettime(CLOCK_MONOTONIC, &profile_##name##_checkpoint); \
        double profile_##name##_ms = \
            (profile_##name##_checkpoint.tv_sec - profile_##name##_start.tv_sec) * 1000.0 + \
            (profile_##name##_checkpoint.tv_nsec - profile_##name##_start.tv_nsec) / 1e6; \
        buckets_info("[PROFILE][" #name "][" checkpoint_name "] %.3f ms - " fmt, profile_##name##_ms, ##__VA_ARGS__); \
    } while (0)

#define PROFILE_MARK(fmt, ...) \
    buckets_info("[PROFILE] " fmt, ##__VA_ARGS__)

#else

#define PROFILE_START(name) do {} while (0)
#define PROFILE_END(name, fmt, ...) do {} while (0)
#define PROFILE_CHECKPOINT(name, checkpoint_name, fmt, ...) do {} while (0)
#define PROFILE_MARK(fmt, ...) do {} while (0)

#endif /* BUCKETS_PROFILE_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_PROFILE_H */
