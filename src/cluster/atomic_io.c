/**
 * Atomic I/O Operations
 * 
 * Provides atomic file write/read operations using temp file + rename pattern
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <libgen.h>

#include "buckets.h"
#include "buckets_io.h"

int buckets_atomic_write(const char *path, const void *data, size_t size)
{
    if (!path || !data) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Create temp file path: <path>.tmp.<pid> */
    char *temp_path = buckets_format("%s.tmp.%d", path, getpid());
    if (!temp_path) {
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Ensure parent directory exists */
    char *path_copy = buckets_strdup(path);
    char *dir_name = dirname(path_copy);
    char *dir_path = buckets_strdup(dir_name);  /* Save dirname result before freeing path_copy */
    buckets_free(path_copy);
    
    int ret = buckets_ensure_directory(dir_path);
    
    if (ret != BUCKETS_OK) {
        buckets_free(dir_path);
        buckets_free(temp_path);
        return ret;
    }
    
    /* Write to temp file */
    FILE *fp = fopen(temp_path, "wb");
    if (!fp) {
        buckets_error("Failed to create temp file %s: %s", temp_path, strerror(errno));
        buckets_free(temp_path);
        return BUCKETS_ERR_IO;
    }
    
    size_t written = fwrite(data, 1, size, fp);
    if (written != size) {
        buckets_error("Failed to write %zu bytes to %s: %s", size, temp_path, strerror(errno));
        fclose(fp);
        unlink(temp_path);
        buckets_free(temp_path);
        return BUCKETS_ERR_IO;
    }
    
    /* Flush and sync to disk */
    if (fflush(fp) != 0) {
        buckets_error("Failed to flush %s: %s", temp_path, strerror(errno));
        fclose(fp);
        unlink(temp_path);
        buckets_free(temp_path);
        return BUCKETS_ERR_IO;
    }
    
    int fd = fileno(fp);
    if (fsync(fd) != 0) {
        buckets_error("Failed to sync %s: %s", temp_path, strerror(errno));
        fclose(fp);
        unlink(temp_path);
        buckets_free(temp_path);
        return BUCKETS_ERR_IO;
    }
    
    fclose(fp);
    
    /* Atomic rename */
    if (rename(temp_path, path) != 0) {
        buckets_error("Failed to rename %s to %s: %s", temp_path, path, strerror(errno));
        unlink(temp_path);
        buckets_free(dir_path);
        buckets_free(temp_path);
        return BUCKETS_ERR_IO;
    }
    
    /* Sync parent directory to persist rename */
    ret = buckets_sync_directory(dir_path);
    buckets_free(dir_path);
    buckets_free(temp_path);
    
    return ret;
}

int buckets_atomic_read(const char *path, void **data_out, size_t *size_out)
{
    if (!path || !data_out || !size_out) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    *data_out = NULL;
    *size_out = 0;
    
    /* Open file */
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        buckets_error("Failed to open %s: %s", path, strerror(errno));
        return BUCKETS_ERR_IO;
    }
    
    /* Get file size */
    if (fseek(fp, 0, SEEK_END) != 0) {
        buckets_error("Failed to seek %s: %s", path, strerror(errno));
        fclose(fp);
        return BUCKETS_ERR_IO;
    }
    
    long file_size = ftell(fp);
    if (file_size < 0) {
        buckets_error("Failed to get size of %s: %s", path, strerror(errno));
        fclose(fp);
        return BUCKETS_ERR_IO;
    }
    
    if (fseek(fp, 0, SEEK_SET) != 0) {
        buckets_error("Failed to rewind %s: %s", path, strerror(errno));
        fclose(fp);
        return BUCKETS_ERR_IO;
    }
    
    /* Allocate buffer */
    void *data = buckets_malloc(file_size + 1);  /* +1 for null terminator */
    
    /* Read entire file */
    size_t read_bytes = fread(data, 1, file_size, fp);
    if (read_bytes != (size_t)file_size) {
        buckets_error("Failed to read %ld bytes from %s: %s", file_size, path, strerror(errno));
        buckets_free(data);
        fclose(fp);
        return BUCKETS_ERR_IO;
    }
    
    fclose(fp);
    
    /* Null-terminate for convenience (if data is text) */
    ((char *)data)[file_size] = '\0';
    
    *data_out = data;
    *size_out = file_size;
    
    return BUCKETS_OK;
}

int buckets_ensure_directory(const char *path)
{
    if (!path) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Check if directory exists */
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return BUCKETS_OK;  /* Already exists */
        } else {
            buckets_error("%s exists but is not a directory", path);
            return BUCKETS_ERR_EXISTS;
        }
    }
    
    /* Create parent directories recursively */
    char *path_copy = buckets_strdup(path);
    char *parent = dirname(path_copy);
    
    if (strcmp(parent, ".") != 0 && strcmp(parent, "/") != 0) {
        int ret = buckets_ensure_directory(parent);
        if (ret != BUCKETS_OK) {
            buckets_free(path_copy);
            return ret;
        }
    }
    
    buckets_free(path_copy);
    
    /* Create directory with rwx for owner, rx for group/others */
    if (mkdir(path, 0755) != 0) {
        if (errno != EEXIST) {
            buckets_error("Failed to create directory %s: %s", path, strerror(errno));
            return BUCKETS_ERR_IO;
        }
    }
    
    return BUCKETS_OK;
}

int buckets_sync_directory(const char *path)
{
    if (!path) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        buckets_error("Failed to open directory %s: %s", path, strerror(errno));
        return BUCKETS_ERR_IO;
    }
    
    if (fsync(fd) != 0) {
        buckets_error("Failed to sync directory %s: %s", path, strerror(errno));
        close(fd);
        return BUCKETS_ERR_IO;
    }
    
    close(fd);
    return BUCKETS_OK;
}
