/**
 * Group Commit Implementation
 * 
 * Implements batched fsync to reduce disk sync operations.
 * Uses per-file descriptor batching with hybrid count/time triggers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>

#include "buckets.h"
#include "buckets_group_commit.h"

/* Per-file descriptor batch state */
typedef struct {
    int fd;                          /* File descriptor */
    uint32_t pending_count;          /* Number of pending writes */
    struct timeval last_sync;        /* Time of last sync */
    bool active;                     /* Is this slot active? */
    pthread_mutex_t lock;            /* Per-fd lock for thread safety */
} fd_batch_state_t;

/* Group commit context */
struct buckets_group_commit_context {
    buckets_group_commit_config_t config;
    fd_batch_state_t fd_states[BUCKETS_GC_MAX_OPEN_FILES];
    pthread_mutex_t global_lock;     /* Lock for fd_states array */
    buckets_group_commit_stats_t stats;
    pthread_mutex_t stats_lock;      /* Lock for statistics */
};

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

/**
 * Calculate time difference in milliseconds
 */
static uint64_t time_diff_ms(const struct timeval *start)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    
    uint64_t start_ms = (uint64_t)start->tv_sec * 1000 + start->tv_usec / 1000;
    uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
    
    return now_ms - start_ms;
}

/**
 * Find or create fd_batch_state for given fd
 */
static fd_batch_state_t* get_fd_state(buckets_group_commit_context_t *ctx, int fd)
{
    pthread_mutex_lock(&ctx->global_lock);
    
    /* First, try to find existing state */
    for (int i = 0; i < BUCKETS_GC_MAX_OPEN_FILES; i++) {
        if (ctx->fd_states[i].active && ctx->fd_states[i].fd == fd) {
            pthread_mutex_unlock(&ctx->global_lock);
            return &ctx->fd_states[i];
        }
    }
    
    /* Not found, allocate new slot */
    for (int i = 0; i < BUCKETS_GC_MAX_OPEN_FILES; i++) {
        if (!ctx->fd_states[i].active) {
            ctx->fd_states[i].fd = fd;
            ctx->fd_states[i].pending_count = 0;
            gettimeofday(&ctx->fd_states[i].last_sync, NULL);
            ctx->fd_states[i].active = true;
            pthread_mutex_unlock(&ctx->global_lock);
            return &ctx->fd_states[i];
        }
    }
    
    pthread_mutex_unlock(&ctx->global_lock);
    buckets_warn("Group commit: No free fd slots (max %d)", BUCKETS_GC_MAX_OPEN_FILES);
    return NULL;
}

/**
 * Perform sync operation (fsync or fdatasync)
 */
static int do_sync(buckets_group_commit_context_t *ctx, int fd)
{
    int ret;
    
    if (ctx->config.durability == BUCKETS_DURABILITY_NONE) {
        /* No sync needed */
        return 0;
    }
    
    if (ctx->config.use_fdatasync) {
        ret = fdatasync(fd);
    } else {
        ret = fsync(fd);
    }
    
    if (ret != 0) {
        buckets_error("Group commit: sync failed for fd %d: %s", fd, strerror(errno));
        return -1;
    }
    
    return 0;
}

/**
 * Check if sync should be triggered
 */
static bool should_sync(buckets_group_commit_context_t *ctx, fd_batch_state_t *state)
{
    /* Immediate durability always syncs */
    if (ctx->config.durability == BUCKETS_DURABILITY_IMMEDIATE) {
        return true;
    }
    
    /* No durability never syncs */
    if (ctx->config.durability == BUCKETS_DURABILITY_NONE) {
        return false;
    }
    
    /* Check batch size */
    if (state->pending_count >= ctx->config.batch_size) {
        return true;
    }
    
    /* Check time window */
    uint64_t elapsed = time_diff_ms(&state->last_sync);
    if (elapsed >= ctx->config.batch_time_ms) {
        return true;
    }
    
    return false;
}

/**
 * Flush fd state (internal)
 */
static int flush_fd_state(buckets_group_commit_context_t *ctx, fd_batch_state_t *state)
{
    if (state->pending_count == 0) {
        return 0;  /* Nothing to flush */
    }
    
    /* Perform sync */
    int ret = do_sync(ctx, state->fd);
    if (ret != 0) {
        return -1;
    }
    
    /* Update statistics */
    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.total_syncs++;
    ctx->stats.batched_syncs++;
    pthread_mutex_unlock(&ctx->stats_lock);
    
    /* Reset state */
    uint32_t flushed_count = state->pending_count;
    state->pending_count = 0;
    gettimeofday(&state->last_sync, NULL);
    
    buckets_debug("Group commit: Flushed %u writes for fd %d", flushed_count, state->fd);
    
    return 0;
}

/* ===================================================================
 * Public API Implementation
 * ===================================================================*/

buckets_group_commit_context_t* buckets_group_commit_init(const buckets_group_commit_config_t *config)
{
    buckets_group_commit_context_t *ctx = buckets_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    
    /* Copy configuration or use defaults */
    if (config) {
        ctx->config = *config;
    } else {
        ctx->config = buckets_group_commit_default_config();
    }
    
    /* Initialize global lock */
    pthread_mutex_init(&ctx->global_lock, NULL);
    pthread_mutex_init(&ctx->stats_lock, NULL);
    
    /* Initialize per-fd locks */
    for (int i = 0; i < BUCKETS_GC_MAX_OPEN_FILES; i++) {
        ctx->fd_states[i].active = false;
        pthread_mutex_init(&ctx->fd_states[i].lock, NULL);
    }
    
    /* Initialize statistics */
    memset(&ctx->stats, 0, sizeof(ctx->stats));
    
    buckets_info("Group commit initialized: batch_size=%u, batch_time_ms=%u, "
                 "durability=%d, fdatasync=%s",
                 ctx->config.batch_size,
                 ctx->config.batch_time_ms,
                 ctx->config.durability,
                 ctx->config.use_fdatasync ? "yes" : "no");
    
    return ctx;
}

void buckets_group_commit_cleanup(buckets_group_commit_context_t *ctx)
{
    if (!ctx) {
        return;
    }
    
    /* Flush all pending writes */
    buckets_info("Group commit cleanup: flushing all pending writes");
    buckets_group_commit_flush_all(ctx);
    
    /* Destroy locks */
    pthread_mutex_destroy(&ctx->global_lock);
    pthread_mutex_destroy(&ctx->stats_lock);
    
    for (int i = 0; i < BUCKETS_GC_MAX_OPEN_FILES; i++) {
        pthread_mutex_destroy(&ctx->fd_states[i].lock);
    }
    
    /* Print final statistics */
    buckets_info("Group commit stats: writes=%lu, syncs=%lu, avg_batch=%.2f",
                 ctx->stats.total_writes,
                 ctx->stats.total_syncs,
                 ctx->stats.total_syncs > 0 ? 
                     (double)ctx->stats.total_writes / ctx->stats.total_syncs : 0.0);
    
    buckets_free(ctx);
}

const buckets_group_commit_config_t* buckets_group_commit_get_config(buckets_group_commit_context_t *ctx)
{
    return ctx ? &ctx->config : NULL;
}

ssize_t buckets_group_commit_write(buckets_group_commit_context_t *ctx,
                                   int fd,
                                   const void *buf,
                                   size_t count)
{
    if (!ctx || fd < 0 || !buf) {
        errno = EINVAL;
        return -1;
    }
    
    /* Perform write */
    ssize_t written = write(fd, buf, count);
    if (written < 0) {
        return -1;
    }
    
    /* Update statistics */
    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.total_writes++;
    ctx->stats.bytes_written += written;
    pthread_mutex_unlock(&ctx->stats_lock);
    
    /* Get fd state */
    fd_batch_state_t *state = get_fd_state(ctx, fd);
    if (!state) {
        /* Fallback: immediate sync */
        do_sync(ctx, fd);
        return written;
    }
    
    /* Lock this fd's state */
    pthread_mutex_lock(&state->lock);
    
    /* Increment pending count */
    state->pending_count++;
    
    /* Check if we should sync */
    if (should_sync(ctx, state)) {
        flush_fd_state(ctx, state);
    }
    
    pthread_mutex_unlock(&state->lock);
    
    return written;
}

ssize_t buckets_group_commit_pwrite(buckets_group_commit_context_t *ctx,
                                    int fd,
                                    const void *buf,
                                    size_t count,
                                    off_t offset)
{
    if (!ctx || fd < 0 || !buf) {
        errno = EINVAL;
        return -1;
    }
    
    /* Perform write */
    ssize_t written = pwrite(fd, buf, count, offset);
    if (written < 0) {
        return -1;
    }
    
    /* Update statistics */
    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.total_writes++;
    ctx->stats.bytes_written += written;
    pthread_mutex_unlock(&ctx->stats_lock);
    
    /* Get fd state */
    fd_batch_state_t *state = get_fd_state(ctx, fd);
    if (!state) {
        /* Fallback: immediate sync */
        do_sync(ctx, fd);
        return written;
    }
    
    /* Lock this fd's state */
    pthread_mutex_lock(&state->lock);
    
    /* Increment pending count */
    state->pending_count++;
    
    /* Check if we should sync */
    if (should_sync(ctx, state)) {
        flush_fd_state(ctx, state);
    }
    
    pthread_mutex_unlock(&state->lock);
    
    return written;
}

int buckets_group_commit_flush_fd(buckets_group_commit_context_t *ctx, int fd)
{
    if (!ctx || fd < 0) {
        return -1;
    }
    
    fd_batch_state_t *state = get_fd_state(ctx, fd);
    if (!state) {
        return 0;  /* No pending writes */
    }
    
    pthread_mutex_lock(&state->lock);
    
    /* Update statistics */
    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.explicit_flushes++;
    pthread_mutex_unlock(&ctx->stats_lock);
    
    int ret = flush_fd_state(ctx, state);
    pthread_mutex_unlock(&state->lock);
    
    return ret;
}

int buckets_group_commit_flush_all(buckets_group_commit_context_t *ctx)
{
    if (!ctx) {
        return -1;
    }
    
    int errors = 0;
    
    pthread_mutex_lock(&ctx->global_lock);
    
    for (int i = 0; i < BUCKETS_GC_MAX_OPEN_FILES; i++) {
        if (ctx->fd_states[i].active) {
            pthread_mutex_lock(&ctx->fd_states[i].lock);
            if (flush_fd_state(ctx, &ctx->fd_states[i]) != 0) {
                errors++;
            }
            pthread_mutex_unlock(&ctx->fd_states[i].lock);
        }
    }
    
    pthread_mutex_unlock(&ctx->global_lock);
    
    return errors > 0 ? -1 : 0;
}

int buckets_group_commit_get_stats(buckets_group_commit_context_t *ctx,
                                   buckets_group_commit_stats_t *stats)
{
    if (!ctx || !stats) {
        return -1;
    }
    
    pthread_mutex_lock(&ctx->stats_lock);
    *stats = ctx->stats;
    
    /* Calculate average batch size */
    if (ctx->stats.total_syncs > 0) {
        stats->avg_batch_size = (double)ctx->stats.total_writes / ctx->stats.total_syncs;
    } else {
        stats->avg_batch_size = 0.0;
    }
    
    pthread_mutex_unlock(&ctx->stats_lock);
    
    return 0;
}

void buckets_group_commit_reset_stats(buckets_group_commit_context_t *ctx)
{
    if (!ctx) {
        return;
    }
    
    pthread_mutex_lock(&ctx->stats_lock);
    memset(&ctx->stats, 0, sizeof(ctx->stats));
    pthread_mutex_unlock(&ctx->stats_lock);
}

buckets_group_commit_config_t buckets_group_commit_default_config(void)
{
    buckets_group_commit_config_t config = {
        .batch_size = BUCKETS_GC_DEFAULT_BATCH_SIZE,
        .batch_time_ms = BUCKETS_GC_DEFAULT_BATCH_TIME_MS,
        .use_fdatasync = true,
        .durability = BUCKETS_DURABILITY_BATCHED
    };
    return config;
}

buckets_group_commit_config_t buckets_group_commit_config_for_durability(
    buckets_durability_level_t durability)
{
    buckets_group_commit_config_t config = buckets_group_commit_default_config();
    config.durability = durability;
    
    /* Adjust batch parameters based on durability */
    switch (durability) {
    case BUCKETS_DURABILITY_NONE:
        config.batch_size = 0;        /* Never sync */
        config.batch_time_ms = 0;
        break;
    case BUCKETS_DURABILITY_IMMEDIATE:
        config.batch_size = 1;        /* Sync after every write */
        config.batch_time_ms = 0;
        break;
    case BUCKETS_DURABILITY_BATCHED:
        /* Use defaults */
        break;
    }
    
    return config;
}
