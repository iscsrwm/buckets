/**
 * Async I/O Implementation
 * 
 * Asynchronous disk I/O using libuv's thread pool.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include "buckets.h"
#include "async_io.h"

/* ===================================================================
 * Initialization
 * ===================================================================*/

int async_io_init(async_io_ctx_t *ctx, uv_loop_t *loop)
{
    if (!ctx || !loop) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    memset(ctx, 0, sizeof(*ctx));
    ctx->loop = loop;
    ctx->initialized = true;
    
    return BUCKETS_OK;
}

void async_io_cleanup(async_io_ctx_t *ctx)
{
    if (!ctx) return;
    
    ctx->loop = NULL;
    ctx->initialized = false;
}

/* ===================================================================
 * Internal Helpers
 * ===================================================================*/

/* Create directories recursively */
static int mkdir_recursive(const char *path, int mode)
{
    char tmp[PATH_MAX];
    char *p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
    }
    
    return 0;
}

/* Atomic write: write to temp file then rename */
static int atomic_write_file(const char *path, const void *data, size_t size)
{
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, getpid());
    
    /* Write to temp file */
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }
    
    ssize_t written = 0;
    while (written < (ssize_t)size) {
        ssize_t n = write(fd, (const char*)data + written, size - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp_path);
            return -1;
        }
        written += n;
    }
    
    /* Sync to disk */
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    
    close(fd);
    
    /* Atomic rename */
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    
    return 0;
}

/* Simple write file */
static int simple_write_file(const char *path, const void *data, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }
    
    ssize_t written = 0;
    while (written < (ssize_t)size) {
        ssize_t n = write(fd, (const char*)data + written, size - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        written += n;
    }
    
    close(fd);
    return 0;
}

/* Read entire file */
static int read_file_contents(const char *path, void **data, size_t *size, size_t max_size)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    
    size_t file_size = (size_t)st.st_size;
    if (max_size > 0 && file_size > max_size) {
        file_size = max_size;
    }
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    
    void *buf = buckets_malloc(file_size);
    if (!buf) {
        close(fd);
        return -1;
    }
    
    ssize_t total_read = 0;
    while (total_read < (ssize_t)file_size) {
        ssize_t n = read(fd, (char*)buf + total_read, file_size - total_read);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            buckets_free(buf);
            return -1;
        }
        if (n == 0) break;  /* EOF */
        total_read += n;
    }
    
    close(fd);
    
    *data = buf;
    *size = (size_t)total_read;
    return 0;
}

/* ===================================================================
 * File Write Operations
 * ===================================================================*/

static void write_work_cb(uv_work_t *req)
{
    async_write_req_t *wr = (async_write_req_t*)req->data;
    
    /* Create parent directory if needed */
    char *last_slash = strrchr(wr->path, '/');
    if (last_slash) {
        char dir_path[PATH_MAX];
        size_t dir_len = (size_t)(last_slash - wr->path);
        if (dir_len > 0 && dir_len < sizeof(dir_path)) {
            memcpy(dir_path, wr->path, dir_len);
            dir_path[dir_len] = '\0';
            mkdir_recursive(dir_path, 0755);
        }
    }
    
    /* Write file */
    if (wr->atomic) {
        wr->status = atomic_write_file(wr->path, wr->data, wr->size);
    } else {
        wr->status = simple_write_file(wr->path, wr->data, wr->size);
    }
}

static void write_done_cb(uv_work_t *req, int status)
{
    async_write_req_t *wr = (async_write_req_t*)req->data;
    
    if (status != 0) {
        wr->status = status;
    }
    
    /* Call user callback */
    if (wr->callback) {
        wr->callback(wr, wr->status);
    }
    
    /* Cleanup */
    buckets_free(wr->path);
    if (wr->owns_data && wr->data) {
        buckets_free(wr->data);
    }
    buckets_free(wr);
}

int async_io_write_file(async_io_ctx_t *ctx,
                        const char *path,
                        const void *data,
                        size_t size,
                        bool owns_data,
                        bool atomic,
                        async_write_cb callback,
                        void *user_data)
{
    if (!ctx || !ctx->initialized || !path || !data) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    async_write_req_t *wr = buckets_calloc(1, sizeof(async_write_req_t));
    if (!wr) {
        return BUCKETS_ERR_NOMEM;
    }
    
    wr->ctx = ctx;
    wr->path = buckets_strdup(path);
    if (!wr->path) {
        buckets_free(wr);
        return BUCKETS_ERR_NOMEM;
    }
    
    if (owns_data) {
        wr->data = (void*)data;
    } else {
        wr->data = buckets_malloc(size);
        if (!wr->data) {
            buckets_free(wr->path);
            buckets_free(wr);
            return BUCKETS_ERR_NOMEM;
        }
        memcpy(wr->data, data, size);
        wr->owns_data = true;
    }
    
    wr->size = size;
    wr->owns_data = owns_data || !owns_data;  /* Always own after copy */
    wr->atomic = atomic;
    wr->callback = callback;
    wr->user_data = user_data;
    wr->work.data = wr;
    
    int ret = uv_queue_work(ctx->loop, &wr->work, write_work_cb, write_done_cb);
    if (ret != 0) {
        if (wr->owns_data) buckets_free(wr->data);
        buckets_free(wr->path);
        buckets_free(wr);
        return BUCKETS_ERR_IO;
    }
    
    return BUCKETS_OK;
}

/* ===================================================================
 * File Read Operations
 * ===================================================================*/

static void read_work_cb(uv_work_t *req)
{
    async_read_req_t *rr = (async_read_req_t*)req->data;
    
    rr->status = read_file_contents(rr->path, &rr->data, &rr->size, rr->max_size);
}

static void read_done_cb(uv_work_t *req, int status)
{
    async_read_req_t *rr = (async_read_req_t*)req->data;
    
    if (status != 0) {
        rr->status = status;
    }
    
    /* Call user callback */
    if (rr->callback) {
        rr->callback(rr, rr->status, rr->data, rr->size);
    }
    
    /* Cleanup request - data ownership transfers to callback */
    buckets_free(rr->path);
    buckets_free(rr);
}

int async_io_read_file(async_io_ctx_t *ctx,
                       const char *path,
                       size_t max_size,
                       async_read_cb callback,
                       void *user_data)
{
    if (!ctx || !ctx->initialized || !path) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    async_read_req_t *rr = buckets_calloc(1, sizeof(async_read_req_t));
    if (!rr) {
        return BUCKETS_ERR_NOMEM;
    }
    
    rr->ctx = ctx;
    rr->path = buckets_strdup(path);
    if (!rr->path) {
        buckets_free(rr);
        return BUCKETS_ERR_NOMEM;
    }
    
    rr->max_size = max_size;
    rr->callback = callback;
    rr->user_data = user_data;
    rr->work.data = rr;
    
    int ret = uv_queue_work(ctx->loop, &rr->work, read_work_cb, read_done_cb);
    if (ret != 0) {
        buckets_free(rr->path);
        buckets_free(rr);
        return BUCKETS_ERR_IO;
    }
    
    return BUCKETS_OK;
}

/* ===================================================================
 * Directory Operations
 * ===================================================================*/

static void mkdir_work_cb(uv_work_t *req)
{
    async_mkdir_req_t *mr = (async_mkdir_req_t*)req->data;
    
    if (mr->recursive) {
        mr->status = mkdir_recursive(mr->path, mr->mode);
    } else {
        mr->status = (mkdir(mr->path, mr->mode) == 0 || errno == EEXIST) ? 0 : -1;
    }
}

static void mkdir_done_cb(uv_work_t *req, int status)
{
    async_mkdir_req_t *mr = (async_mkdir_req_t*)req->data;
    
    if (status != 0) {
        mr->status = status;
    }
    
    if (mr->callback) {
        mr->callback(mr->user_data, mr->status);
    }
    
    buckets_free(mr->path);
    buckets_free(mr);
}

int async_io_mkdir(async_io_ctx_t *ctx,
                   const char *path,
                   int mode,
                   bool recursive,
                   void (*callback)(void *user_data, int status),
                   void *user_data)
{
    if (!ctx || !ctx->initialized || !path) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    async_mkdir_req_t *mr = buckets_calloc(1, sizeof(async_mkdir_req_t));
    if (!mr) {
        return BUCKETS_ERR_NOMEM;
    }
    
    mr->ctx = ctx;
    mr->path = buckets_strdup(path);
    if (!mr->path) {
        buckets_free(mr);
        return BUCKETS_ERR_NOMEM;
    }
    
    mr->mode = mode;
    mr->recursive = recursive;
    mr->callback = callback;
    mr->user_data = user_data;
    mr->work.data = mr;
    
    int ret = uv_queue_work(ctx->loop, &mr->work, mkdir_work_cb, mkdir_done_cb);
    if (ret != 0) {
        buckets_free(mr->path);
        buckets_free(mr);
        return BUCKETS_ERR_IO;
    }
    
    return BUCKETS_OK;
}

/* ===================================================================
 * Chunk Operations
 * ===================================================================*/

static void chunk_write_work_cb(uv_work_t *req)
{
    async_chunk_write_req_t *cw = (async_chunk_write_req_t*)req->data;
    
    /* Build chunk path: {disk_path}/{object_path}/part.{index} */
    char chunk_path[PATH_MAX];
    snprintf(chunk_path, sizeof(chunk_path), "%s/%s/part.%u",
             cw->disk_path, cw->object_path, cw->chunk_index);
    
    /* Create parent directory */
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/%s",
             cw->disk_path, cw->object_path);
    mkdir_recursive(dir_path, 0755);
    
    /* Write chunk atomically */
    cw->status = atomic_write_file(chunk_path, cw->data, cw->size);
}

static void chunk_write_done_cb(uv_work_t *req, int status)
{
    async_chunk_write_req_t *cw = (async_chunk_write_req_t*)req->data;
    
    if (status != 0) {
        cw->status = status;
    }
    
    if (cw->callback) {
        cw->callback(cw->user_data, cw->status);
    }
    
    /* Cleanup */
    buckets_free(cw->disk_path);
    buckets_free(cw->object_path);
    if (cw->owns_data && cw->data) {
        buckets_free(cw->data);
    }
    buckets_free(cw);
}

int async_io_write_chunk(async_io_ctx_t *ctx,
                         const char *disk_path,
                         const char *object_path,
                         uint32_t chunk_index,
                         const void *data,
                         size_t size,
                         bool owns_data,
                         void (*callback)(void *user_data, int status),
                         void *user_data)
{
    if (!ctx || !ctx->initialized || !disk_path || !object_path || !data) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    async_chunk_write_req_t *cw = buckets_calloc(1, sizeof(async_chunk_write_req_t));
    if (!cw) {
        return BUCKETS_ERR_NOMEM;
    }
    
    cw->ctx = ctx;
    cw->disk_path = buckets_strdup(disk_path);
    cw->object_path = buckets_strdup(object_path);
    
    if (!cw->disk_path || !cw->object_path) {
        buckets_free(cw->disk_path);
        buckets_free(cw->object_path);
        buckets_free(cw);
        return BUCKETS_ERR_NOMEM;
    }
    
    if (owns_data) {
        cw->data = (void*)data;
        cw->owns_data = true;
    } else {
        cw->data = buckets_malloc(size);
        if (!cw->data) {
            buckets_free(cw->disk_path);
            buckets_free(cw->object_path);
            buckets_free(cw);
            return BUCKETS_ERR_NOMEM;
        }
        memcpy(cw->data, data, size);
        cw->owns_data = true;
    }
    
    cw->size = size;
    cw->chunk_index = chunk_index;
    cw->callback = callback;
    cw->user_data = user_data;
    cw->work.data = cw;
    
    int ret = uv_queue_work(ctx->loop, &cw->work, chunk_write_work_cb, chunk_write_done_cb);
    if (ret != 0) {
        if (cw->owns_data) buckets_free(cw->data);
        buckets_free(cw->disk_path);
        buckets_free(cw->object_path);
        buckets_free(cw);
        return BUCKETS_ERR_IO;
    }
    
    return BUCKETS_OK;
}

static void chunk_read_work_cb(uv_work_t *req)
{
    async_chunk_read_req_t *cr = (async_chunk_read_req_t*)req->data;
    
    /* Build chunk path */
    char chunk_path[PATH_MAX];
    snprintf(chunk_path, sizeof(chunk_path), "%s/%s/part.%u",
             cr->disk_path, cr->object_path, cr->chunk_index);
    
    cr->status = read_file_contents(chunk_path, &cr->data, &cr->size, 0);
}

static void chunk_read_done_cb(uv_work_t *req, int status)
{
    async_chunk_read_req_t *cr = (async_chunk_read_req_t*)req->data;
    
    if (status != 0) {
        cr->status = status;
    }
    
    if (cr->callback) {
        cr->callback(cr->user_data, cr->status, cr->data, cr->size);
    }
    
    /* Cleanup - data ownership transfers to callback */
    buckets_free(cr->disk_path);
    buckets_free(cr->object_path);
    buckets_free(cr);
}

int async_io_read_chunk(async_io_ctx_t *ctx,
                        const char *disk_path,
                        const char *object_path,
                        uint32_t chunk_index,
                        void (*callback)(void *user_data, int status, 
                                        void *data, size_t size),
                        void *user_data)
{
    if (!ctx || !ctx->initialized || !disk_path || !object_path) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    async_chunk_read_req_t *cr = buckets_calloc(1, sizeof(async_chunk_read_req_t));
    if (!cr) {
        return BUCKETS_ERR_NOMEM;
    }
    
    cr->ctx = ctx;
    cr->disk_path = buckets_strdup(disk_path);
    cr->object_path = buckets_strdup(object_path);
    
    if (!cr->disk_path || !cr->object_path) {
        buckets_free(cr->disk_path);
        buckets_free(cr->object_path);
        buckets_free(cr);
        return BUCKETS_ERR_NOMEM;
    }
    
    cr->chunk_index = chunk_index;
    cr->callback = callback;
    cr->user_data = user_data;
    cr->work.data = cr;
    
    int ret = uv_queue_work(ctx->loop, &cr->work, chunk_read_work_cb, chunk_read_done_cb);
    if (ret != 0) {
        buckets_free(cr->disk_path);
        buckets_free(cr->object_path);
        buckets_free(cr);
        return BUCKETS_ERR_IO;
    }
    
    return BUCKETS_OK;
}

/* ===================================================================
 * Batch Operations
 * ===================================================================*/

/* Internal: called when each batch item completes */
static void batch_item_complete(async_batch_write_t *batch, int status)
{
    bool all_done = false;
    int success, failed;
    
    uv_mutex_lock(&batch->lock);
    
    batch->completed_count++;
    if (status == 0) {
        batch->success_count++;
    } else {
        batch->failed_count++;
    }
    
    if (batch->completed_count >= batch->total_count) {
        all_done = true;
    }
    
    success = batch->success_count;
    failed = batch->failed_count;
    
    uv_mutex_unlock(&batch->lock);
    
    if (all_done) {
        /* Call completion callback */
        if (batch->callback) {
            batch->callback(batch->user_data, success, failed);
        }
        
        /* Cleanup batch */
        uv_mutex_destroy(&batch->lock);
        buckets_free(batch);
    }
}

/* Batch write callback wrapper */
static void batch_write_cb(async_write_req_t *req, int status)
{
    async_batch_write_t *batch = (async_batch_write_t*)req->user_data;
    batch_item_complete(batch, status);
}

/* Batch chunk write callback wrapper */
static void batch_chunk_write_cb(void *user_data, int status)
{
    async_batch_write_t *batch = (async_batch_write_t*)user_data;
    batch_item_complete(batch, status);
}

async_batch_write_t* async_io_batch_write_start(async_io_ctx_t *ctx,
                                                 int count,
                                                 async_batch_write_cb callback,
                                                 void *user_data)
{
    if (!ctx || !ctx->initialized || count <= 0) {
        return NULL;
    }
    
    async_batch_write_t *batch = buckets_calloc(1, sizeof(async_batch_write_t));
    if (!batch) {
        return NULL;
    }
    
    batch->ctx = ctx;
    batch->total_count = count;
    batch->callback = callback;
    batch->user_data = user_data;
    
    if (uv_mutex_init(&batch->lock) != 0) {
        buckets_free(batch);
        return NULL;
    }
    
    return batch;
}

int async_io_batch_write_add(async_batch_write_t *batch,
                             const char *path,
                             const void *data,
                             size_t size,
                             bool owns_data,
                             bool atomic)
{
    if (!batch || !path || !data) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Queue the write with batch as user_data */
    async_write_req_t *wr = buckets_calloc(1, sizeof(async_write_req_t));
    if (!wr) {
        return BUCKETS_ERR_NOMEM;
    }
    
    wr->ctx = batch->ctx;
    wr->path = buckets_strdup(path);
    if (!wr->path) {
        buckets_free(wr);
        return BUCKETS_ERR_NOMEM;
    }
    
    if (owns_data) {
        wr->data = (void*)data;
        wr->owns_data = true;
    } else {
        wr->data = buckets_malloc(size);
        if (!wr->data) {
            buckets_free(wr->path);
            buckets_free(wr);
            return BUCKETS_ERR_NOMEM;
        }
        memcpy(wr->data, data, size);
        wr->owns_data = true;
    }
    
    wr->size = size;
    wr->atomic = atomic;
    wr->callback = batch_write_cb;
    wr->user_data = batch;
    wr->work.data = wr;
    
    int ret = uv_queue_work(batch->ctx->loop, &wr->work, write_work_cb, write_done_cb);
    if (ret != 0) {
        if (wr->owns_data) buckets_free(wr->data);
        buckets_free(wr->path);
        buckets_free(wr);
        return BUCKETS_ERR_IO;
    }
    
    return BUCKETS_OK;
}

int async_io_batch_write_add_chunk(async_batch_write_t *batch,
                                    const char *disk_path,
                                    const char *object_path,
                                    uint32_t chunk_index,
                                    const void *data,
                                    size_t size,
                                    bool owns_data)
{
    if (!batch || !disk_path || !object_path || !data) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    return async_io_write_chunk(batch->ctx, disk_path, object_path, chunk_index,
                                data, size, owns_data,
                                batch_chunk_write_cb, batch);
}
