/**
 * io_uring Async I/O Implementation
 * 
 * Provides high-performance async I/O using io_uring.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <liburing.h>

#include "buckets.h"
#include "buckets_io_uring.h"

/* Per-operation context */
typedef struct {
    buckets_io_op_type_t op_type;
    int fd;
    void *buf;
    size_t count;
    off_t offset;
    buckets_io_completion_cb callback;
    void *user_data;
} io_op_context_t;

/* io_uring context */
struct buckets_io_uring_context {
    struct io_uring ring;
    buckets_io_uring_config_t config;
    buckets_io_uring_stats_t stats;
    pthread_mutex_t lock;
    pthread_t poller_thread;
    bool poller_running;
    bool initialized;
    bool shutdown_requested;
};

/* Default configuration */
static const buckets_io_uring_config_t DEFAULT_CONFIG = {
    .queue_depth = 256,
    .batch_size = 32,
    .sq_poll = false,      /* Kernel polling - can reduce latency but uses CPU */
    .io_poll = false       /* Polled I/O - for NVMe/high-speed storage */
};

/**
 * Background thread that continuously polls for io_uring completions
 */
static void* io_uring_poller_thread(void *arg)
{
    buckets_io_uring_context_t *ctx = (buckets_io_uring_context_t*)arg;
    
    buckets_info("io_uring poller thread started");
    
    int poll_count = 0;
    
    while (!ctx->shutdown_requested) {
        /* Process completions with a timeout so we can check shutdown periodically */
        struct __kernel_timespec ts = {
            .tv_sec = 0,
            .tv_nsec = 100000000  /* 100ms */
        };
        
        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe_timeout(&ctx->ring, &cqe, &ts);
        
        poll_count++;
        
        if (ret == -ETIME) {
            /* Timeout - check for shutdown and continue */
            if (poll_count % 50 == 0) {  /* Log every 5 seconds */
                buckets_info("io_uring poller: no completions (timeout)");
            }
            continue;
        }
        
        if (ret < 0) {
            if (ret != -EINTR) {
                buckets_error("io_uring_wait_cqe_timeout failed: %s", strerror(-ret));
            }
            continue;
        }
        
        buckets_info("io_uring poller: got completion, res=%d", cqe->res);
        
        /* Process this completion */
        io_op_context_t *op_ctx = io_uring_cqe_get_data(cqe);
        if (op_ctx) {
            buckets_io_result_t result = {
                .op_type = op_ctx->op_type,
                .fd = op_ctx->fd,
                .user_data = op_ctx->user_data,
                .result = cqe->res,
                .error = cqe->res < 0 ? -cqe->res : 0
            };
            
            /* Update statistics */
            pthread_mutex_lock(&ctx->lock);
            ctx->stats.completed_ops++;
            if (result.result < 0) {
                ctx->stats.failed_ops++;
            } else {
                if (op_ctx->op_type == BUCKETS_IO_OP_READ || op_ctx->op_type == BUCKETS_IO_OP_PREAD) {
                    ctx->stats.bytes_read += result.result;
                } else if (op_ctx->op_type == BUCKETS_IO_OP_WRITE || op_ctx->op_type == BUCKETS_IO_OP_PWRITE) {
                    ctx->stats.bytes_written += result.result;
                }
            }
            pthread_mutex_unlock(&ctx->lock);
            
            /* Call completion callback */
            if (op_ctx->callback) {
                op_ctx->callback(&result);
            }
            
            buckets_free(op_ctx);
        }
        
        io_uring_cqe_seen(&ctx->ring, cqe);
        
        /* Process any additional completions that are ready (non-blocking) */
        unsigned head;
        unsigned i = 0;
        io_uring_for_each_cqe(&ctx->ring, head, cqe) {
            op_ctx = io_uring_cqe_get_data(cqe);
            if (op_ctx) {
                buckets_io_result_t result = {
                    .op_type = op_ctx->op_type,
                    .fd = op_ctx->fd,
                    .user_data = op_ctx->user_data,
                    .result = cqe->res,
                    .error = cqe->res < 0 ? -cqe->res : 0
                };
                
                pthread_mutex_lock(&ctx->lock);
                ctx->stats.completed_ops++;
                if (result.result < 0) {
                    ctx->stats.failed_ops++;
                } else {
                    if (op_ctx->op_type == BUCKETS_IO_OP_READ || op_ctx->op_type == BUCKETS_IO_OP_PREAD) {
                        ctx->stats.bytes_read += result.result;
                    } else if (op_ctx->op_type == BUCKETS_IO_OP_WRITE || op_ctx->op_type == BUCKETS_IO_OP_PWRITE) {
                        ctx->stats.bytes_written += result.result;
                    }
                }
                pthread_mutex_unlock(&ctx->lock);
                
                if (op_ctx->callback) {
                    op_ctx->callback(&result);
                }
                
                buckets_free(op_ctx);
            }
            i++;
        }
        
        if (i > 0) {
            io_uring_cq_advance(&ctx->ring, i);
        }
    }
    
    buckets_info("io_uring poller thread stopped");
    return NULL;
}

buckets_io_uring_context_t* buckets_io_uring_init(const buckets_io_uring_config_t *config)
{
    buckets_io_uring_context_t *ctx = buckets_calloc(1, sizeof(*ctx));
    if (!ctx) {
        buckets_error("Failed to allocate io_uring context");
        return NULL;
    }
    
    /* Use provided config or defaults */
    if (config) {
        ctx->config = *config;
    } else {
        ctx->config = DEFAULT_CONFIG;
    }
    
    /* Initialize io_uring */
    struct io_uring_params params = {0};
    
    if (ctx->config.sq_poll) {
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 2000;  /* 2 second idle before kernel thread sleeps */
    }
    if (ctx->config.io_poll) {
        params.flags |= IORING_SETUP_IOPOLL;
    }
    
    int ret = io_uring_queue_init_params(ctx->config.queue_depth, &ctx->ring, &params);
    if (ret < 0) {
        buckets_error("io_uring_queue_init failed: %s", strerror(-ret));
        buckets_free(ctx);
        return NULL;
    }
    
    pthread_mutex_init(&ctx->lock, NULL);
    ctx->initialized = true;
    ctx->shutdown_requested = false;
    ctx->poller_running = false;
    
    /* Start background poller thread */
    ret = pthread_create(&ctx->poller_thread, NULL, io_uring_poller_thread, ctx);
    if (ret != 0) {
        buckets_error("Failed to create io_uring poller thread: %s", strerror(ret));
        io_uring_queue_exit(&ctx->ring);
        buckets_free(ctx);
        return NULL;
    }
    
    ctx->poller_running = true;
    
    buckets_info("io_uring initialized: queue_depth=%u, sq_poll=%d, io_poll=%d, poller_thread=yes",
                ctx->config.queue_depth, ctx->config.sq_poll, ctx->config.io_poll);
    
    return ctx;
}

void buckets_io_uring_cleanup(buckets_io_uring_context_t *ctx)
{
    if (!ctx) return;
    
    if (ctx->initialized) {
        /* Stop poller thread */
        if (ctx->poller_running) {
            ctx->shutdown_requested = true;
            pthread_join(ctx->poller_thread, NULL);
            ctx->poller_running = false;
        }
        
        /* Wait for all pending operations to complete */
        io_uring_queue_exit(&ctx->ring);
        pthread_mutex_destroy(&ctx->lock);
    }
    
    buckets_free(ctx);
}

static int submit_io_op(buckets_io_uring_context_t *ctx,
                       buckets_io_op_type_t op_type,
                       int fd,
                       void *buf,
                       size_t count,
                       off_t offset,
                       bool datasync,
                       buckets_io_completion_cb callback,
                       void *user_data)
{
    if (!ctx || !ctx->initialized) {
        errno = EINVAL;
        return -1;
    }
    
    /* Allocate operation context */
    io_op_context_t *op_ctx = buckets_malloc(sizeof(*op_ctx));
    if (!op_ctx) {
        return -1;
    }
    
    op_ctx->op_type = op_type;
    op_ctx->fd = fd;
    op_ctx->buf = buf;
    op_ctx->count = count;
    op_ctx->offset = offset;
    op_ctx->callback = callback;
    op_ctx->user_data = user_data;
    
    /* Get submission queue entry */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        buckets_error("io_uring queue full");
        buckets_free(op_ctx);
        errno = EAGAIN;
        return -1;
    }
    
    /* Prepare operation based on type */
    switch (op_type) {
        case BUCKETS_IO_OP_READ:
            io_uring_prep_read(sqe, fd, buf, count, -1);
            break;
            
        case BUCKETS_IO_OP_WRITE:
            io_uring_prep_write(sqe, fd, buf, count, -1);
            break;
            
        case BUCKETS_IO_OP_PREAD:
            io_uring_prep_read(sqe, fd, buf, count, offset);
            break;
            
        case BUCKETS_IO_OP_PWRITE:
            io_uring_prep_write(sqe, fd, buf, count, offset);
            break;
            
        case BUCKETS_IO_OP_FSYNC:
        case BUCKETS_IO_OP_FDATASYNC:
            if (datasync) {
                io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
            } else {
                io_uring_prep_fsync(sqe, fd, 0);
            }
            break;
            
        default:
            buckets_error("Unknown I/O operation type: %d", op_type);
            buckets_free(op_ctx);
            errno = EINVAL;
            return -1;
    }
    
    /* Attach operation context to sqe */
    io_uring_sqe_set_data(sqe, op_ctx);
    
    buckets_info("io_uring: submitted op_type=%d fd=%d size=%zu", op_type, fd, count);
    
    /* Update statistics */
    pthread_mutex_lock(&ctx->lock);
    ctx->stats.total_ops++;
    pthread_mutex_unlock(&ctx->lock);
    
    return 0;
}

int buckets_io_uring_read_async(buckets_io_uring_context_t *ctx,
                                int fd,
                                void *buf,
                                size_t count,
                                buckets_io_completion_cb callback,
                                void *user_data)
{
    return submit_io_op(ctx, BUCKETS_IO_OP_READ, fd, buf, count, 0, false,
                       callback, user_data);
}

int buckets_io_uring_write_async(buckets_io_uring_context_t *ctx,
                                 int fd,
                                 const void *buf,
                                 size_t count,
                                 buckets_io_completion_cb callback,
                                 void *user_data)
{
    return submit_io_op(ctx, BUCKETS_IO_OP_WRITE, fd, (void*)buf, count, 0, false,
                       callback, user_data);
}

int buckets_io_uring_pwrite_async(buckets_io_uring_context_t *ctx,
                                  int fd,
                                  const void *buf,
                                  size_t count,
                                  off_t offset,
                                  buckets_io_completion_cb callback,
                                  void *user_data)
{
    return submit_io_op(ctx, BUCKETS_IO_OP_PWRITE, fd, (void*)buf, count, offset, false,
                       callback, user_data);
}

int buckets_io_uring_fsync_async(buckets_io_uring_context_t *ctx,
                                 int fd,
                                 bool datasync,
                                 buckets_io_completion_cb callback,
                                 void *user_data)
{
    buckets_io_op_type_t op_type = datasync ? BUCKETS_IO_OP_FDATASYNC : BUCKETS_IO_OP_FSYNC;
    return submit_io_op(ctx, op_type, fd, NULL, 0, 0, datasync,
                       callback, user_data);
}

int buckets_io_uring_submit(buckets_io_uring_context_t *ctx)
{
    if (!ctx || !ctx->initialized) {
        errno = EINVAL;
        return -1;
    }
    
    int ret = io_uring_submit(&ctx->ring);
    if (ret < 0) {
        buckets_error("io_uring_submit failed: %s", strerror(-ret));
    } else {
        buckets_info("io_uring: submitted %d operations to kernel", ret);
    }
    
    return ret;
}

int buckets_io_uring_process_completions(buckets_io_uring_context_t *ctx,
                                         uint32_t min_complete,
                                         int timeout_ms)
{
    if (!ctx || !ctx->initialized) {
        errno = EINVAL;
        return -1;
    }
    
    struct __kernel_timespec ts;
    struct __kernel_timespec *ts_ptr = NULL;
    
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        ts_ptr = &ts;
    }
    
    struct io_uring_cqe *cqe;
    int completed = 0;
    
    /* Wait for minimum completions */
    if (min_complete > 0) {
        int ret = io_uring_wait_cqe_timeout(&ctx->ring, &cqe, ts_ptr);
        if (ret < 0 && ret != -ETIME) {
            buckets_error("io_uring_wait_cqe_timeout failed: %s", strerror(-ret));
            return -1;
        }
        if (ret == 0) {
            /* Process this completion */
            io_op_context_t *op_ctx = io_uring_cqe_get_data(cqe);
            if (op_ctx) {
                buckets_io_result_t result = {
                    .op_type = op_ctx->op_type,
                    .fd = op_ctx->fd,
                    .user_data = op_ctx->user_data,
                    .result = cqe->res,
                    .error = cqe->res < 0 ? -cqe->res : 0
                };
                
                /* Update statistics */
                pthread_mutex_lock(&ctx->lock);
                ctx->stats.completed_ops++;
                if (result.result < 0) {
                    ctx->stats.failed_ops++;
                } else {
                    if (op_ctx->op_type == BUCKETS_IO_OP_READ || op_ctx->op_type == BUCKETS_IO_OP_PREAD) {
                        ctx->stats.bytes_read += result.result;
                    } else if (op_ctx->op_type == BUCKETS_IO_OP_WRITE || op_ctx->op_type == BUCKETS_IO_OP_PWRITE) {
                        ctx->stats.bytes_written += result.result;
                    }
                }
                pthread_mutex_unlock(&ctx->lock);
                
                /* Call completion callback */
                if (op_ctx->callback) {
                    op_ctx->callback(&result);
                }
                
                buckets_free(op_ctx);
            }
            
            io_uring_cqe_seen(&ctx->ring, cqe);
            completed++;
        }
    }
    
    /* Process all available completions (non-blocking) */
    unsigned head;
    unsigned i = 0;
    io_uring_for_each_cqe(&ctx->ring, head, cqe) {
        io_op_context_t *op_ctx = io_uring_cqe_get_data(cqe);
        if (op_ctx) {
            buckets_io_result_t result = {
                .op_type = op_ctx->op_type,
                .fd = op_ctx->fd,
                .user_data = op_ctx->user_data,
                .result = cqe->res,
                .error = cqe->res < 0 ? -cqe->res : 0
            };
            
            /* Update statistics */
            pthread_mutex_lock(&ctx->lock);
            ctx->stats.completed_ops++;
            if (result.result < 0) {
                ctx->stats.failed_ops++;
            } else {
                if (op_ctx->op_type == BUCKETS_IO_OP_READ || op_ctx->op_type == BUCKETS_IO_OP_PREAD) {
                    ctx->stats.bytes_read += result.result;
                } else if (op_ctx->op_type == BUCKETS_IO_OP_WRITE || op_ctx->op_type == BUCKETS_IO_OP_PWRITE) {
                    ctx->stats.bytes_written += result.result;
                }
            }
            pthread_mutex_unlock(&ctx->lock);
            
            /* Call completion callback */
            if (op_ctx->callback) {
                op_ctx->callback(&result);
            }
            
            buckets_free(op_ctx);
        }
        i++;
    }
    
    if (i > 0) {
        io_uring_cq_advance(&ctx->ring, i);
        completed += i;
    }
    
    return completed;
}

int buckets_io_uring_submit_and_wait(buckets_io_uring_context_t *ctx,
                                     int timeout_ms)
{
    int submitted = buckets_io_uring_submit(ctx);
    if (submitted < 0) {
        return submitted;
    }
    
    return buckets_io_uring_process_completions(ctx, 1, timeout_ms);
}

void buckets_io_uring_get_stats(buckets_io_uring_context_t *ctx,
                                buckets_io_uring_stats_t *stats)
{
    if (!ctx || !stats) return;
    
    pthread_mutex_lock(&ctx->lock);
    *stats = ctx->stats;
    pthread_mutex_unlock(&ctx->lock);
}
