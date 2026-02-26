/**
 * Buckets - High-Performance S3-Compatible Object Storage
 * 
 * Main header file for the Buckets storage system.
 * 
 * Copyright (C) 2026 Buckets Project
 * Licensed under AGPLv3
 */

#ifndef BUCKETS_H
#define BUCKETS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Version information */
#define BUCKETS_VERSION_MAJOR 0
#define BUCKETS_VERSION_MINOR 1
#define BUCKETS_VERSION_PATCH 0
#define BUCKETS_VERSION "0.1.0-alpha"

/* Common types */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

/* Error codes */
typedef enum {
    BUCKETS_OK = 0,
    BUCKETS_ERR_NOMEM,
    BUCKETS_ERR_INVALID_ARG,
    BUCKETS_ERR_NOT_FOUND,
    BUCKETS_ERR_EXISTS,
    BUCKETS_ERR_IO,
    BUCKETS_ERR_NETWORK,
    BUCKETS_ERR_TIMEOUT,
    BUCKETS_ERR_QUORUM,
    BUCKETS_ERR_CORRUPT,
    BUCKETS_ERR_UNSUPPORTED,
    BUCKETS_ERR_CRYPTO,
} buckets_error_t;

/* Return result with error handling */
typedef struct {
    buckets_error_t error;
    void *data;
} buckets_result_t;

/* UUID type (128-bit) */
typedef struct {
    u8 bytes[16];
} buckets_uuid_t;

/* Timestamp (microseconds since epoch) */
typedef u64 buckets_time_t;

/* Common macros */
#define BUCKETS_UNUSED(x) ((void)(x))
#define BUCKETS_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define BUCKETS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define BUCKETS_MAX(a, b) ((a) > (b) ? (a) : (b))

/* Memory management */
void* buckets_malloc(size_t size);
void* buckets_calloc(size_t count, size_t size);
void* buckets_realloc(void *ptr, size_t size);
void  buckets_free(void *ptr);

/* String utilities */
char* buckets_strdup(const char *str);
int   buckets_strcmp(const char *s1, const char *s2);
char* buckets_format(const char *fmt, ...);

/* Logging */
typedef enum {
    BUCKETS_LOG_DEBUG,
    BUCKETS_LOG_INFO,
    BUCKETS_LOG_WARN,
    BUCKETS_LOG_ERROR,
    BUCKETS_LOG_FATAL,
} buckets_log_level_t;

void buckets_log(buckets_log_level_t level, const char *fmt, ...);

/* Logging configuration */
void buckets_set_log_level(buckets_log_level_t level);
buckets_log_level_t buckets_get_log_level(void);
buckets_log_level_t buckets_parse_log_level(const char *level_str);
int buckets_set_log_file(const char *path);
void buckets_log_init(void);

#define buckets_debug(...) buckets_log(BUCKETS_LOG_DEBUG, __VA_ARGS__)
#define buckets_info(...)  buckets_log(BUCKETS_LOG_INFO, __VA_ARGS__)
#define buckets_warn(...)  buckets_log(BUCKETS_LOG_WARN, __VA_ARGS__)
#define buckets_error(...) buckets_log(BUCKETS_LOG_ERROR, __VA_ARGS__)
#define buckets_fatal(...) buckets_log(BUCKETS_LOG_FATAL, __VA_ARGS__)

/* Initialization and cleanup */
int  buckets_init(void);
void buckets_cleanup(void);

/* Version info */
const char* buckets_version(void);
int buckets_version_major(void);
int buckets_version_minor(void);
int buckets_version_patch(void);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_H */
