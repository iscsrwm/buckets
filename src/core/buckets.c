/**
 * Buckets Core Implementation
 * 
 * Core initialization, memory management, logging, utilities
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <strings.h>
#include "buckets.h"
#include "buckets_cache.h"

/* Global state */
static bool g_initialized = false;
static buckets_log_level_t g_log_level = BUCKETS_LOG_INFO;
static FILE *g_log_file = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Memory management */
void* buckets_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr && size > 0) {
        buckets_fatal("Out of memory: failed to allocate %zu bytes", size);
        abort();
    }
    return ptr;
}

void* buckets_calloc(size_t count, size_t size) {
    void *ptr = calloc(count, size);
    if (!ptr && (count * size) > 0) {
        buckets_fatal("Out of memory: failed to allocate %zu bytes", count * size);
        abort();
    }
    return ptr;
}

void* buckets_realloc(void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0) {
        buckets_fatal("Out of memory: failed to reallocate to %zu bytes", size);
        abort();
    }
    return new_ptr;
}

void buckets_free(void *ptr) {
    free(ptr);
}

/* String utilities */
char* buckets_strdup(const char *str) {
    if (!str) return NULL;
    
    size_t len = strlen(str) + 1;
    char *copy = buckets_malloc(len);
    memcpy(copy, str, len);
    return copy;
}

int buckets_strcmp(const char *s1, const char *s2) {
    return strcmp(s1 ? s1 : "", s2 ? s2 : "");
}

char* buckets_format(const char *fmt, ...) {
    va_list args, args_copy;
    
    va_start(args, fmt);
    va_copy(args_copy, args);
    
    /* Calculate required size */
    int size = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    
    if (size < 0) {
        va_end(args_copy);
        return NULL;
    }
    
    /* Allocate and format */
    char *str = buckets_malloc(size + 1);
    vsnprintf(str, size + 1, fmt, args_copy);
    va_end(args_copy);
    
    return str;
}

/* Logging */
static const char* log_level_string(buckets_log_level_t level) {
    switch (level) {
        case BUCKETS_LOG_DEBUG: return "DEBUG";
        case BUCKETS_LOG_INFO:  return "INFO ";
        case BUCKETS_LOG_WARN:  return "WARN ";
        case BUCKETS_LOG_ERROR: return "ERROR";
        case BUCKETS_LOG_FATAL: return "FATAL";
        default: return "?????";
    }
}

void buckets_log(buckets_log_level_t level, const char *fmt, ...) {
    if (level < g_log_level) {
        return;
    }
    
    pthread_mutex_lock(&g_log_mutex);
    
    /* Get timestamp */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    /* Format log line */
    va_list args;
    va_start(args, fmt);
    
    /* Write to stderr */
    fprintf(stderr, "[%s] %s: ", timestamp, log_level_string(level));
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
    
    /* Write to file if configured */
    if (g_log_file) {
        fprintf(g_log_file, "[%s] %s: ", timestamp, log_level_string(level));
        va_start(args, fmt);  /* Restart va_list for second use */
        vfprintf(g_log_file, fmt, args);
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
    }
    
    va_end(args);
    pthread_mutex_unlock(&g_log_mutex);
}

void buckets_set_log_level(buckets_log_level_t level) {
    g_log_level = level;
}

buckets_log_level_t buckets_get_log_level(void) {
    return g_log_level;
}

buckets_log_level_t buckets_parse_log_level(const char *level_str) {
    if (!level_str) return BUCKETS_LOG_INFO;
    
    if (strcasecmp(level_str, "DEBUG") == 0) return BUCKETS_LOG_DEBUG;
    if (strcasecmp(level_str, "INFO") == 0) return BUCKETS_LOG_INFO;
    if (strcasecmp(level_str, "WARN") == 0) return BUCKETS_LOG_WARN;
    if (strcasecmp(level_str, "ERROR") == 0) return BUCKETS_LOG_ERROR;
    if (strcasecmp(level_str, "FATAL") == 0) return BUCKETS_LOG_FATAL;
    
    return BUCKETS_LOG_INFO;  /* Default */
}

int buckets_set_log_file(const char *path) {
    pthread_mutex_lock(&g_log_mutex);
    
    /* Close existing log file if open */
    if (g_log_file && g_log_file != stderr) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    
    /* Open new log file */
    if (path) {
        g_log_file = fopen(path, "a");  /* Append mode */
        if (!g_log_file) {
            pthread_mutex_unlock(&g_log_mutex);
            fprintf(stderr, "Failed to open log file: %s\n", path);
            return BUCKETS_ERR_IO;
        }
    }
    
    pthread_mutex_unlock(&g_log_mutex);
    return BUCKETS_OK;
}

void buckets_log_init(void) {
    /* Read BUCKETS_LOG_LEVEL environment variable */
    const char *level_str = getenv("BUCKETS_LOG_LEVEL");
    if (level_str) {
        g_log_level = buckets_parse_log_level(level_str);
    }
    
    /* Read BUCKETS_LOG_FILE environment variable */
    const char *log_file = getenv("BUCKETS_LOG_FILE");
    if (log_file) {
        buckets_set_log_file(log_file);
    }
}

/* Initialization and cleanup */
int buckets_init(void) {
    if (g_initialized) {
        buckets_warn("Buckets already initialized");
        return 0;
    }
    
    buckets_debug("Initializing Buckets v%s", BUCKETS_VERSION);
    
    /* Initialize cache subsystems */
    buckets_format_cache_init();
    buckets_topology_cache_init();
    
    /* TODO: Initialize remaining subsystems */
    /* - Hash ring */
    /* - Location registry */
    /* - Network layer */
    /* - Storage layer */
    
    g_initialized = true;
    buckets_debug("Buckets initialization complete");
    
    return 0;
}

void buckets_cleanup(void) {
    if (!g_initialized) {
        return;
    }
    
    buckets_debug("Cleaning up Buckets");
    
    /* Cleanup subsystems in reverse order of initialization */
    /* TODO: Cleanup storage layer, network layer, registry, hash ring */
    
    /* Cleanup cache subsystems */
    buckets_topology_cache_cleanup();
    buckets_format_cache_cleanup();
    
    g_initialized = false;
    buckets_debug("Buckets cleanup complete");
}

/* Version information */
const char* buckets_version(void) {
    return BUCKETS_VERSION;
}

int buckets_version_major(void) {
    return BUCKETS_VERSION_MAJOR;
}

int buckets_version_minor(void) {
    return BUCKETS_VERSION_MINOR;
}

int buckets_version_patch(void) {
    return BUCKETS_VERSION_PATCH;
}
