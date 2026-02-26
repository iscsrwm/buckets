/**
 * Storage Layer Performance Benchmarks
 * 
 * Measures performance characteristics of the storage layer including:
 * - Single-disk object operations (PUT/GET/DELETE)
 * - Multi-disk quorum operations
 * - Erasure coding encode/decode
 * - Metadata cache performance
 * - Scalability across different object sizes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_cluster.h"
#include "buckets_erasure.h"
#include "buckets_crypto.h"

/* Benchmark configuration */
#define BENCH_WARMUP_ITERS 10
#define BENCH_MEASURE_ITERS 100
#define BENCH_SMALL_SIZE (4 * 1024)           /* 4KB */
#define BENCH_MEDIUM_SIZE (128 * 1024)        /* 128KB */
#define BENCH_LARGE_SIZE (1 * 1024 * 1024)    /* 1MB */
#define BENCH_XLARGE_SIZE (10 * 1024 * 1024)  /* 10MB */

/* Test data directory */
#define BENCH_DATA_DIR "/tmp/buckets-bench"

/* Color output */
#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"

/* Timing utilities */
static inline double get_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1e6 + (double)tv.tv_usec;
}

/* Format throughput */
static void format_throughput(double bytes_per_sec, char *buf, size_t buf_size)
{
    if (bytes_per_sec >= 1e9) {
        snprintf(buf, buf_size, "%.2f GB/s", bytes_per_sec / 1e9);
    } else if (bytes_per_sec >= 1e6) {
        snprintf(buf, buf_size, "%.2f MB/s", bytes_per_sec / 1e6);
    } else if (bytes_per_sec >= 1e3) {
        snprintf(buf, buf_size, "%.2f KB/s", bytes_per_sec / 1e3);
    } else {
        snprintf(buf, buf_size, "%.2f B/s", bytes_per_sec);
    }
}

/* Format latency */
static void format_latency(double us, char *buf, size_t buf_size)
{
    if (us >= 1e6) {
        snprintf(buf, buf_size, "%.2f s", us / 1e6);
    } else if (us >= 1e3) {
        snprintf(buf, buf_size, "%.2f ms", us / 1e3);
    } else {
        snprintf(buf, buf_size, "%.2f μs", us);
    }
}

/* Generate random data */
static u8* generate_random_data(size_t size)
{
    u8 *data = buckets_malloc(size);
    if (!data) {
        return NULL;
    }
    
    /* Simple PRNG for repeatable results */
    for (size_t i = 0; i < size; i++) {
        data[i] = (u8)((i * 17 + 42) % 256);
    }
    
    return data;
}

/* Cleanup benchmark directory */
static void cleanup_bench_dir(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", BENCH_DATA_DIR);
    system(cmd);
}

/* Setup benchmark directory */
static bool setup_bench_dir(void)
{
    cleanup_bench_dir();
    
    if (mkdir(BENCH_DATA_DIR, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create benchmark directory: %s\n", strerror(errno));
        return false;
    }
    
    return true;
}

/* ========================================================================
 * Benchmark 1: Single-Disk Object Operations
 * ======================================================================== */

typedef struct {
    double put_latency_us;
    double get_latency_us;
    double delete_latency_us;
    double put_throughput_mbps;
    double get_throughput_mbps;
} single_disk_result_t;

static void bench_single_disk_operations(size_t obj_size, const char *size_label)
{
    printf("\n" COLOR_CYAN "→ Single-Disk Operations (%s)" COLOR_RESET "\n", size_label);
    
    /* Setup storage */
    buckets_storage_config_t config = {
        .data_dir = BENCH_DATA_DIR,
        .inline_threshold = 128 * 1024,
        .ec_data_blocks = 8,
        .ec_parity_blocks = 4
    };
    
    if (!buckets_storage_init(&config)) {
        fprintf(stderr, "Failed to initialize storage\n");
        return;
    }
    
    /* Generate test data */
    u8 *data = generate_random_data(obj_size);
    if (!data) {
        fprintf(stderr, "Failed to generate test data\n");
        buckets_storage_cleanup();
        return;
    }
    
    /* Warmup */
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
        char bucket[64], object[64];
        snprintf(bucket, sizeof(bucket), "warmup-bucket");
        snprintf(object, sizeof(object), "warmup-obj-%d", i);
        buckets_storage_put(bucket, object, data, obj_size, NULL);
    }
    
    /* Benchmark PUT */
    double put_start = get_time_us();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        char bucket[64], object[64];
        snprintf(bucket, sizeof(bucket), "bench-bucket");
        snprintf(object, sizeof(object), "obj-%d", i);
        buckets_storage_put(bucket, object, data, obj_size, NULL);
    }
    double put_end = get_time_us();
    double put_total_us = put_end - put_start;
    double put_avg_us = put_total_us / BENCH_MEASURE_ITERS;
    double put_throughput = (double)(obj_size * BENCH_MEASURE_ITERS) / (put_total_us / 1e6);
    
    /* Benchmark GET */
    double get_start = get_time_us();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        char bucket[64], object[64];
        snprintf(bucket, sizeof(bucket), "bench-bucket");
        snprintf(object, sizeof(object), "obj-%d", i);
        size_t size;
        u8 *retrieved = buckets_storage_get(bucket, object, &size);
        if (retrieved) {
            buckets_free(retrieved);
        }
    }
    double get_end = get_time_us();
    double get_total_us = get_end - get_start;
    double get_avg_us = get_total_us / BENCH_MEASURE_ITERS;
    double get_throughput = (double)(obj_size * BENCH_MEASURE_ITERS) / (get_total_us / 1e6);
    
    /* Benchmark DELETE */
    double del_start = get_time_us();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        char bucket[64], object[64];
        snprintf(bucket, sizeof(bucket), "bench-bucket");
        snprintf(object, sizeof(object), "obj-%d", i);
        buckets_storage_delete(bucket, object);
    }
    double del_end = get_time_us();
    double del_total_us = del_end - del_start;
    double del_avg_us = del_total_us / BENCH_MEASURE_ITERS;
    
    /* Format and print results */
    char put_lat_str[64], get_lat_str[64], del_lat_str[64];
    char put_thr_str[64], get_thr_str[64];
    
    format_latency(put_avg_us, put_lat_str, sizeof(put_lat_str));
    format_latency(get_avg_us, get_lat_str, sizeof(get_lat_str));
    format_latency(del_avg_us, del_lat_str, sizeof(del_lat_str));
    format_throughput(put_throughput, put_thr_str, sizeof(put_thr_str));
    format_throughput(get_throughput, get_thr_str, sizeof(get_thr_str));
    
    printf("  PUT:    %s/op  (%s)\n", put_lat_str, put_thr_str);
    printf("  GET:    %s/op  (%s)\n", get_lat_str, get_thr_str);
    printf("  DELETE: %s/op\n", del_lat_str);
    printf("  Ops/sec: PUT=%.0f, GET=%.0f, DELETE=%.0f\n",
           1e6 / put_avg_us, 1e6 / get_avg_us, 1e6 / del_avg_us);
    
    /* Cleanup */
    buckets_free(data);
    buckets_storage_cleanup();
}

/* ========================================================================
 * Benchmark 2: Erasure Coding Performance
 * ======================================================================== */

static void bench_erasure_coding(size_t data_size, const char *size_label)
{
    printf("\n" COLOR_CYAN "→ Erasure Coding (8+4, %s)" COLOR_RESET "\n", size_label);
    
    const int k = 8, m = 4;
    buckets_ec_ctx_t *ctx = buckets_ec_init(k, m);
    if (!ctx) {
        fprintf(stderr, "Failed to initialize erasure coding context\n");
        return;
    }
    
    /* Generate test data */
    u8 *data = generate_random_data(data_size);
    if (!data) {
        fprintf(stderr, "Failed to generate test data\n");
        buckets_ec_free(ctx);
        return;
    }
    
    /* Calculate chunk size */
    size_t chunk_size = buckets_ec_calc_chunk_size(data_size, k);
    size_t aligned_size = chunk_size * k;
    
    /* Allocate encoding buffers */
    u8 **chunks = buckets_malloc((k + m) * sizeof(u8*));
    for (int i = 0; i < k + m; i++) {
        chunks[i] = buckets_malloc(chunk_size);
    }
    
    /* Warmup */
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
        buckets_ec_encode(ctx, data, data_size, chunks, chunk_size);
    }
    
    /* Benchmark ENCODE */
    double enc_start = get_time_us();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        buckets_ec_encode(ctx, data, data_size, chunks, chunk_size);
    }
    double enc_end = get_time_us();
    double enc_total_us = enc_end - enc_start;
    double enc_avg_us = enc_total_us / BENCH_MEASURE_ITERS;
    double enc_throughput = (double)(data_size * BENCH_MEASURE_ITERS) / (enc_total_us / 1e6);
    
    /* Benchmark DECODE (all chunks available) */
    u8 *decoded = buckets_malloc(aligned_size);
    double dec_start = get_time_us();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        buckets_ec_decode(ctx, chunks, chunk_size, decoded, data_size);
    }
    double dec_end = get_time_us();
    double dec_total_us = dec_end - dec_start;
    double dec_avg_us = dec_total_us / BENCH_MEASURE_ITERS;
    double dec_throughput = (double)(data_size * BENCH_MEASURE_ITERS) / (dec_total_us / 1e6);
    
    /* Format and print results */
    char enc_lat_str[64], dec_lat_str[64];
    char enc_thr_str[64], dec_thr_str[64];
    
    format_latency(enc_avg_us, enc_lat_str, sizeof(enc_lat_str));
    format_latency(dec_avg_us, dec_lat_str, sizeof(dec_lat_str));
    format_throughput(enc_throughput, enc_thr_str, sizeof(enc_thr_str));
    format_throughput(dec_throughput, dec_thr_str, sizeof(dec_thr_str));
    
    printf("  ENCODE: %s/op  (%s)\n", enc_lat_str, enc_thr_str);
    printf("  DECODE: %s/op  (%s)\n", dec_lat_str, dec_thr_str);
    printf("  Overhead: %.1f%% storage, %.1f%% encode time\n",
           (double)m / k * 100.0,
           (enc_avg_us / (enc_avg_us + dec_avg_us)) * 100.0);
    
    /* Cleanup */
    buckets_free(decoded);
    for (int i = 0; i < k + m; i++) {
        buckets_free(chunks[i]);
    }
    buckets_free(chunks);
    buckets_free(data);
    buckets_ec_free(ctx);
}

/* ========================================================================
 * Benchmark 3: Metadata Cache Performance
 * ======================================================================== */

static void bench_metadata_cache(void)
{
    printf("\n" COLOR_CYAN "→ Metadata Cache Performance" COLOR_RESET "\n");
    
    buckets_metadata_cache_t *cache = buckets_metadata_cache_init(10000, 300);
    if (!cache) {
        fprintf(stderr, "Failed to initialize metadata cache\n");
        return;
    }
    
    /* Create sample xl.meta */
    buckets_xlmeta_t meta = {0};
    meta.stat.size = 1024;
    meta.stat.mod_time = time(NULL);
    strcpy(meta.erasure.algorithm, "ReedSolomon");
    meta.erasure.data_blocks = 8;
    meta.erasure.parity_blocks = 4;
    meta.erasure.block_size = 1024 * 1024;
    
    /* Populate cache with test data */
    const int cache_entries = 1000;
    for (int i = 0; i < cache_entries; i++) {
        char key[128];
        snprintf(key, sizeof(key), "bucket/object-%d", i);
        buckets_metadata_cache_put(cache, key, &meta);
    }
    
    /* Benchmark cache HIT */
    double hit_start = get_time_us();
    int hits = 0;
    for (int i = 0; i < BENCH_MEASURE_ITERS * 10; i++) {
        char key[128];
        snprintf(key, sizeof(key), "bucket/object-%d", i % cache_entries);
        buckets_xlmeta_t *retrieved = buckets_metadata_cache_get(cache, key);
        if (retrieved) {
            hits++;
            /* Don't free - cache owns it */
        }
    }
    double hit_end = get_time_us();
    double hit_total_us = hit_end - hit_start;
    double hit_avg_us = hit_total_us / (BENCH_MEASURE_ITERS * 10);
    
    /* Benchmark cache MISS */
    double miss_start = get_time_us();
    int misses = 0;
    for (int i = 0; i < BENCH_MEASURE_ITERS * 10; i++) {
        char key[128];
        snprintf(key, sizeof(key), "bucket/object-miss-%d", i);
        buckets_xlmeta_t *retrieved = buckets_metadata_cache_get(cache, key);
        if (!retrieved) {
            misses++;
        }
    }
    double miss_end = get_time_us();
    double miss_total_us = miss_end - miss_start;
    double miss_avg_us = miss_total_us / (BENCH_MEASURE_ITERS * 10);
    
    /* Get cache statistics */
    buckets_metadata_cache_stats_t stats;
    buckets_metadata_cache_get_stats(cache, &stats);
    
    /* Format and print results */
    char hit_lat_str[64], miss_lat_str[64];
    format_latency(hit_avg_us, hit_lat_str, sizeof(hit_lat_str));
    format_latency(miss_avg_us, miss_lat_str, sizeof(miss_lat_str));
    
    printf("  Cache HIT:  %s/op  (%.0f ops/sec)\n", hit_lat_str, 1e6 / hit_avg_us);
    printf("  Cache MISS: %s/op  (%.0f ops/sec)\n", miss_lat_str, 1e6 / miss_avg_us);
    printf("  Hit ratio: %.1f%% (%zu hits, %zu misses)\n",
           stats.hits * 100.0 / (stats.hits + stats.misses),
           stats.hits, stats.misses);
    printf("  Speedup: %.1fx faster on cache hit\n", miss_avg_us / hit_avg_us);
    
    /* Cleanup */
    buckets_metadata_cache_free(cache);
}

/* ========================================================================
 * Benchmark 4: Cryptographic Hash Performance
 * ======================================================================== */

static void bench_crypto_hash(size_t data_size, const char *size_label)
{
    printf("\n" COLOR_CYAN "→ Cryptographic Hash (%s)" COLOR_RESET "\n", size_label);
    
    u8 *data = generate_random_data(data_size);
    if (!data) {
        fprintf(stderr, "Failed to generate test data\n");
        return;
    }
    
    u8 hash[32];
    
    /* Benchmark BLAKE2b-256 */
    double blake2b_start = get_time_us();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        buckets_blake2b_256(hash, data, data_size);
    }
    double blake2b_end = get_time_us();
    double blake2b_total_us = blake2b_end - blake2b_start;
    double blake2b_avg_us = blake2b_total_us / BENCH_MEASURE_ITERS;
    double blake2b_throughput = (double)(data_size * BENCH_MEASURE_ITERS) / (blake2b_total_us / 1e6);
    
    /* Benchmark SHA-256 */
    double sha256_start = get_time_us();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        buckets_sha256(hash, data, data_size);
    }
    double sha256_end = get_time_us();
    double sha256_total_us = sha256_end - sha256_start;
    double sha256_avg_us = sha256_total_us / BENCH_MEASURE_ITERS;
    double sha256_throughput = (double)(data_size * BENCH_MEASURE_ITERS) / (sha256_total_us / 1e6);
    
    /* Format and print results */
    char blake2b_lat_str[64], sha256_lat_str[64];
    char blake2b_thr_str[64], sha256_thr_str[64];
    
    format_latency(blake2b_avg_us, blake2b_lat_str, sizeof(blake2b_lat_str));
    format_latency(sha256_avg_us, sha256_lat_str, sizeof(sha256_lat_str));
    format_throughput(blake2b_throughput, blake2b_thr_str, sizeof(blake2b_thr_str));
    format_throughput(sha256_throughput, sha256_thr_str, sizeof(sha256_thr_str));
    
    printf("  BLAKE2b-256: %s/op  (%s)\n", blake2b_lat_str, blake2b_thr_str);
    printf("  SHA-256:     %s/op  (%s)\n", sha256_lat_str, sha256_thr_str);
    printf("  Speedup: BLAKE2b is %.2fx faster than SHA-256\n", sha256_avg_us / blake2b_avg_us);
    
    buckets_free(data);
}

/* ========================================================================
 * Main Benchmark Suite
 * ======================================================================== */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    
    /* Disable debug logging for clean benchmark output */
    setenv("BUCKETS_LOG_LEVEL", "ERROR", 1);
    
    printf(COLOR_BOLD "\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Buckets Storage Layer Performance Benchmarks\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf(COLOR_RESET);
    
    printf("\nConfiguration:\n");
    printf("  Warmup iterations:  %d\n", BENCH_WARMUP_ITERS);
    printf("  Measure iterations: %d\n", BENCH_MEASURE_ITERS);
    printf("  Test data dir:      %s\n", BENCH_DATA_DIR);
    
    /* Setup */
    if (!setup_bench_dir()) {
        return 1;
    }
    
    /* Initialize buckets */
    if (!buckets_init()) {
        fprintf(stderr, "Failed to initialize buckets\n");
        return 1;
    }
    
    /* Run benchmarks */
    printf(COLOR_BOLD "\n━━━ Object Operations ━━━" COLOR_RESET "\n");
    bench_single_disk_operations(BENCH_SMALL_SIZE, "4KB");
    bench_single_disk_operations(BENCH_MEDIUM_SIZE, "128KB");
    bench_single_disk_operations(BENCH_LARGE_SIZE, "1MB");
    bench_single_disk_operations(BENCH_XLARGE_SIZE, "10MB");
    
    printf(COLOR_BOLD "\n━━━ Erasure Coding ━━━" COLOR_RESET "\n");
    bench_erasure_coding(BENCH_SMALL_SIZE, "4KB");
    bench_erasure_coding(BENCH_MEDIUM_SIZE, "128KB");
    bench_erasure_coding(BENCH_LARGE_SIZE, "1MB");
    bench_erasure_coding(BENCH_XLARGE_SIZE, "10MB");
    
    printf(COLOR_BOLD "\n━━━ Metadata Cache ━━━" COLOR_RESET "\n");
    bench_metadata_cache();
    
    printf(COLOR_BOLD "\n━━━ Cryptographic Hashing ━━━" COLOR_RESET "\n");
    bench_crypto_hash(BENCH_SMALL_SIZE, "4KB");
    bench_crypto_hash(BENCH_MEDIUM_SIZE, "128KB");
    bench_crypto_hash(BENCH_LARGE_SIZE, "1MB");
    bench_crypto_hash(BENCH_XLARGE_SIZE, "10MB");
    
    /* Cleanup */
    buckets_cleanup();
    cleanup_bench_dir();
    
    printf(COLOR_BOLD "\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Benchmarks Complete\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf(COLOR_RESET "\n");
    
    return 0;
}
