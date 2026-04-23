/**
 * io_uring Async I/O Interface
 * 
 * Provides async I/O operations using io_uring for high-performance
 * disk operations without blocking threads.
 */

#ifndef BUCKETS_IO_URING_H
#define BUCKETS_IO_URING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

/* Opaque context type */
typedef struct buckets_io_uring_context buckets_io_uring_context_t;

/* I/O operation types */
typedef enum {
    BUCKETS_IO_OP_READ,
    BUCKETS_IO_OP_WRITE,
    BUCKETS_IO_OP_PREAD,
    BUCKETS_IO_OP_PWRITE,
    BUCKETS_IO_OP_FSYNC,
    BUCKETS_IO_OP_FDATASYNC
} buckets_io_op_type_t;

/* I/O operation result */
typedef struct {
    buckets_io_op_type_t op_type;
    int fd;
    void *user_data;          /* User-provided context */
    ssize_t result;           /* Bytes transferred or error code */
    int error;                /* errno value if result < 0 */
} buckets_io_result_t;

/* Completion callback - called when I/O completes */
typedef void (*buckets_io_completion_cb)(buckets_io_result_t *result);

/* Configuration */
typedef struct {
    uint32_t queue_depth;     /* io_uring queue depth (default: 256) */
    uint32_t batch_size;      /* Submit batch size (default: 32) */
    bool sq_poll;             /* Use kernel polling (IORING_SETUP_SQPOLL) */
    bool io_poll;             /* Use polled I/O (IORING_SETUP_IOPOLL) */
} buckets_io_uring_config_t;

/* Statistics */
typedef struct {
    uint64_t total_ops;       /* Total operations submitted */
    uint64_t completed_ops;   /* Total operations completed */
    uint64_t failed_ops;      /* Operations that failed */
    uint64_t bytes_read;      /* Total bytes read */
    uint64_t bytes_written;   /* Total bytes written */
} buckets_io_uring_stats_t;

/**
 * Initialize io_uring context
 * 
 * @param config Configuration (NULL for defaults)
 * @return Context or NULL on error
 */
buckets_io_uring_context_t* buckets_io_uring_init(const buckets_io_uring_config_t *config);

/**
 * Cleanup io_uring context
 * 
 * Waits for all pending operations to complete before cleanup.
 */
void buckets_io_uring_cleanup(buckets_io_uring_context_t *ctx);

/**
 * Submit async read operation
 * 
 * @param ctx io_uring context
 * @param fd File descriptor
 * @param buf Buffer to read into
 * @param count Number of bytes to read
 * @param callback Completion callback
 * @param user_data User context passed to callback
 * @return 0 on success, -1 on error
 */
int buckets_io_uring_read_async(buckets_io_uring_context_t *ctx,
                                int fd,
                                void *buf,
                                size_t count,
                                buckets_io_completion_cb callback,
                                void *user_data);

/**
 * Submit async write operation
 * 
 * @param ctx io_uring context
 * @param fd File descriptor
 * @param buf Buffer to write from
 * @param count Number of bytes to write
 * @param callback Completion callback
 * @param user_data User context passed to callback
 * @return 0 on success, -1 on error
 */
int buckets_io_uring_write_async(buckets_io_uring_context_t *ctx,
                                 int fd,
                                 const void *buf,
                                 size_t count,
                                 buckets_io_completion_cb callback,
                                 void *user_data);

/**
 * Submit async pwrite operation (write at offset)
 * 
 * @param ctx io_uring context
 * @param fd File descriptor
 * @param buf Buffer to write from
 * @param count Number of bytes to write
 * @param offset File offset to write at
 * @param callback Completion callback
 * @param user_data User context passed to callback
 * @return 0 on success, -1 on error
 */
int buckets_io_uring_pwrite_async(buckets_io_uring_context_t *ctx,
                                  int fd,
                                  const void *buf,
                                  size_t count,
                                  off_t offset,
                                  buckets_io_completion_cb callback,
                                  void *user_data);

/**
 * Submit async fsync operation
 * 
 * @param ctx io_uring context
 * @param fd File descriptor
 * @param datasync If true, use fdatasync semantics
 * @param callback Completion callback
 * @param user_data User context passed to callback
 * @return 0 on success, -1 on error
 */
int buckets_io_uring_fsync_async(buckets_io_uring_context_t *ctx,
                                 int fd,
                                 bool datasync,
                                 buckets_io_completion_cb callback,
                                 void *user_data);

/**
 * Submit all pending operations to kernel
 * 
 * @param ctx io_uring context
 * @return Number of operations submitted, -1 on error
 */
int buckets_io_uring_submit(buckets_io_uring_context_t *ctx);

/**
 * Wait for and process completed operations
 * 
 * This calls completion callbacks for all completed operations.
 * 
 * @param ctx io_uring context
 * @param min_complete Minimum operations to wait for (0 = don't wait)
 * @param timeout_ms Timeout in milliseconds (-1 = infinite)
 * @return Number of operations completed, -1 on error
 */
int buckets_io_uring_process_completions(buckets_io_uring_context_t *ctx,
                                         uint32_t min_complete,
                                         int timeout_ms);

/**
 * Submit and wait for completion (convenience function)
 * 
 * Equivalent to submit() followed by process_completions().
 * 
 * @param ctx io_uring context
 * @param timeout_ms Timeout in milliseconds (-1 = infinite)
 * @return Number of operations completed, -1 on error
 */
int buckets_io_uring_submit_and_wait(buckets_io_uring_context_t *ctx,
                                     int timeout_ms);

/**
 * Get statistics
 */
void buckets_io_uring_get_stats(buckets_io_uring_context_t *ctx,
                                buckets_io_uring_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_IO_URING_H */
