/**
 * Phase 4 Storage Layer Performance Benchmarks
 * 
 * Focuses on benchmarking completed components:
 * - Erasure coding (encode/decode with ISA-L)
 * - Cryptographic hashing (BLAKE2b vs SHA-256)
 * - Storage primitives (layout, metadata serialization)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "buckets.h"
#include "buckets_erasure.h"
#include "buckets_crypto.h"

/* Benchmark configuration */
#define BENCH_WARMUP_ITERS 10
#define BENCH_MEASURE_ITERS 100
#define BENCH_SMALL_SIZE (4 * 1024)           /* 4KB */
#define BENCH_MEDIUM_SIZE (128 * 1024)        /* 128KB */
#define BENCH_LARGE_SIZE (1 * 1024 * 1024)    /* 1MB */
#define BENCH_XLARGE_SIZE (10 * 1024 * 1024)  /* 10MB */

/* Color output */
#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_GREEN   "\033[32m"
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

/* ========================================================================
 * Benchmark 1: Erasure Coding Performance (8+4)
 * ======================================================================== */

static void bench_erasure_coding(size_t data_size, const char *size_label)
{
    printf("\n" COLOR_CYAN "→ Erasure Coding (8+4, %s)" COLOR_RESET "\n", size_label);
    
    const u32 k = 8, m = 4;
    
    /* Allocate context */
    buckets_ec_ctx_t ctx;
    if (buckets_ec_init(&ctx, k, m) != 0) {
        fprintf(stderr, "Failed to initialize erasure coding context\n");
        return;
    }
    
    /* Generate test data */
    u8 *data = generate_random_data(data_size);
    if (!data) {
        fprintf(stderr, "Failed to generate test data\n");
        return;
    }
    
    /* Calculate chunk size */
    size_t chunk_size = buckets_ec_calc_chunk_size(data_size, k);
    size_t aligned_size = chunk_size * k;
    
    /* Allocate encoding buffers */
    u8 **data_chunks = buckets_malloc(k * sizeof(u8*));
    u8 **parity_chunks = buckets_malloc(m * sizeof(u8*));
    for (u32 i = 0; i < k; i++) {
        data_chunks[i] = buckets_malloc(chunk_size);
    }
    for (u32 i = 0; i < m; i++) {
        parity_chunks[i] = buckets_malloc(chunk_size);
    }
    
    /* Warmup */
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
        buckets_ec_encode(&ctx, data, data_size, chunk_size, data_chunks, parity_chunks);
    }
    
    /* Benchmark ENCODE */
    double enc_start = get_time_us();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        buckets_ec_encode(&ctx, data, data_size, chunk_size, data_chunks, parity_chunks);
    }
    double enc_end = get_time_us();
    double enc_total_us = enc_end - enc_start;
    double enc_avg_us = enc_total_us / BENCH_MEASURE_ITERS;
    double enc_throughput = (double)(data_size * BENCH_MEASURE_ITERS) / (enc_total_us / 1e6);
    
    /* Prepare for decode (all chunks available) */
    u8 **all_chunks = buckets_malloc((k + m) * sizeof(u8*));
    for (u32 i = 0; i < k; i++) {
        all_chunks[i] = data_chunks[i];
    }
    for (u32 i = 0; i < m; i++) {
        all_chunks[k + i] = parity_chunks[i];
    }
    
    /* Benchmark DECODE */
    u8 *decoded = buckets_malloc(aligned_size);
    double dec_start = get_time_us();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        buckets_ec_decode(&ctx, all_chunks, chunk_size, decoded, data_size);
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
    printf("  Storage overhead: %.1f%% (12 chunks for 8 chunks of data)\n", (double)m / k * 100.0);
    printf("  Encoding overhead: %.1f%% of total time\n",
           (enc_avg_us / (enc_avg_us + dec_avg_us)) * 100.0);
    
    /* Cleanup */
    buckets_free(decoded);
    buckets_free(all_chunks);
    for (u32 i = 0; i < k; i++) {
        buckets_free(data_chunks[i]);
    }
    for (u32 i = 0; i < m; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_free(data_chunks);
    buckets_free(parity_chunks);
    buckets_free(data);
}

/* ========================================================================
 * Benchmark 2: Cryptographic Hash Performance
 * ======================================================================== */

static void bench_crypto_hash(size_t data_size, const char *size_label)
{
    printf("\n" COLOR_CYAN "→ Cryptographic Hashing (%s)" COLOR_RESET "\n", size_label);
    
    u8 *data = generate_random_data(data_size);
    if (!data) {
        fprintf(stderr, "Failed to generate test data\n");
        return;
    }
    
    u8 hash[32];
    
    /* Warmup */
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
        buckets_blake2b_256(hash, data, data_size);
        buckets_sha256(hash, data, data_size);
    }
    
    /* Benchmark BLAKE2b-256 */
    double blake2b_start = get_time_us();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        buckets_blake2b_256(hash, data, data_size);
    }
    double blake2b_end = get_time_us();
    double blake2b_total_us = blake2b_end - blake2b_start;
    double blake2b_avg_us = blake2b_total_us / BENCH_MEASURE_ITERS;
    double blake2b_throughput = (double)(data_size * BENCH_MEASURE_ITERS) / (blake2b_total_us / 1e6);
    
    /* Benchmark SHA-256 (OpenSSL hardware-accelerated) */
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
    
    if (blake2b_avg_us < sha256_avg_us) {
        printf("  Winner: BLAKE2b is %.2fx faster than SHA-256\n", sha256_avg_us / blake2b_avg_us);
    } else {
        printf("  Winner: SHA-256 is %.2fx faster than BLAKE2b (HW acceleration?)\n", blake2b_avg_us / sha256_avg_us);
    }
    
    buckets_free(data);
}

/* ========================================================================
 * Benchmark 3: Erasure Reconstruction (Missing Chunks)
 * ======================================================================== */

static void bench_reconstruction(size_t data_size, const char *size_label)
{
    printf("\n" COLOR_CYAN "→ Chunk Reconstruction (8+4, %s)" COLOR_RESET "\n", size_label);
    
    const u32 k = 8, m = 4;
    
    buckets_ec_ctx_t ctx;
    if (buckets_ec_init(&ctx, k, m) != 0) {
        fprintf(stderr, "Failed to initialize erasure coding context\n");
        return;
    }
    
    u8 *data = generate_random_data(data_size);
    if (!data) {
        fprintf(stderr, "Failed to generate test data\n");
        return;
    }
    
    size_t chunk_size = buckets_ec_calc_chunk_size(data_size, k);
    size_t aligned_size = chunk_size * k;
    
    /* Allocate encoding buffers */
    u8 **data_chunks = buckets_malloc(k * sizeof(u8*));
    u8 **parity_chunks = buckets_malloc(m * sizeof(u8*));
    for (u32 i = 0; i < k; i++) {
        data_chunks[i] = buckets_malloc(chunk_size);
    }
    for (u32 i = 0; i < m; i++) {
        parity_chunks[i] = buckets_malloc(chunk_size);
    }
    
    /* Encode once */
    buckets_ec_encode(&ctx, data, data_size, chunk_size, data_chunks, parity_chunks);
    
    /* Prepare decode with 2 missing data chunks */
    u8 **all_chunks = buckets_malloc((k + m) * sizeof(u8*));
    for (u32 i = 0; i < k; i++) {
        all_chunks[i] = (i < 2) ? NULL : data_chunks[i];  /* First 2 missing */
    }
    for (u32 i = 0; i < m; i++) {
        all_chunks[k + i] = parity_chunks[i];
    }
    
    /* Warmup */
    u8 *decoded = buckets_malloc(aligned_size);
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
        buckets_ec_decode(&ctx, all_chunks, chunk_size, decoded, data_size);
    }
    
    /* Benchmark RECONSTRUCTION (2 missing data chunks) */
    double rec_start = get_time_us();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        buckets_ec_decode(&ctx, all_chunks, chunk_size, decoded, data_size);
    }
    double rec_end = get_time_us();
    double rec_total_us = rec_end - rec_start;
    double rec_avg_us = rec_total_us / BENCH_MEASURE_ITERS;
    double rec_throughput = (double)(data_size * BENCH_MEASURE_ITERS) / (rec_total_us / 1e6);
    
    /* Format and print results */
    char rec_lat_str[64], rec_thr_str[64];
    format_latency(rec_avg_us, rec_lat_str, sizeof(rec_lat_str));
    format_throughput(rec_throughput, rec_thr_str, sizeof(rec_thr_str));
    
    printf("  RECONSTRUCT: %s/op  (%s)\n", rec_lat_str, rec_thr_str);
    printf("  Scenario: 2 of 8 data chunks missing (read from 10 of 12 disks)\n");
    printf("  Tolerance: Can survive loss of up to 4 chunks\n");
    
    /* Cleanup */
    buckets_free(decoded);
    buckets_free(all_chunks);
    for (u32 i = 0; i < k; i++) {
        buckets_free(data_chunks[i]);
    }
    for (u32 i = 0; i < m; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_free(data_chunks);
    buckets_free(parity_chunks);
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
    printf("  Buckets Phase 4 Storage Layer Benchmarks\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf(COLOR_RESET);
    
    printf("\nConfiguration:\n");
    printf("  Warmup iterations:  %d\n", BENCH_WARMUP_ITERS);
    printf("  Measure iterations: %d\n", BENCH_MEASURE_ITERS);
    printf("  Erasure coding:     8+4 (Reed-Solomon with ISA-L)\n");
    printf("  Hashing:            BLAKE2b-256 vs SHA-256 (OpenSSL)\n");
    
    /* Initialize buckets */
    if (buckets_init() != 0) {
        fprintf(stderr, "Failed to initialize buckets\n");
        return 1;
    }
    
    /* Run benchmarks */
    printf(COLOR_BOLD "\n━━━ Erasure Coding (Encode/Decode) ━━━" COLOR_RESET "\n");
    bench_erasure_coding(BENCH_SMALL_SIZE, "4KB");
    bench_erasure_coding(BENCH_MEDIUM_SIZE, "128KB");
    bench_erasure_coding(BENCH_LARGE_SIZE, "1MB");
    bench_erasure_coding(BENCH_XLARGE_SIZE, "10MB");
    
    printf(COLOR_BOLD "\n━━━ Chunk Reconstruction (Missing Disks) ━━━" COLOR_RESET "\n");
    bench_reconstruction(BENCH_SMALL_SIZE, "4KB");
    bench_reconstruction(BENCH_MEDIUM_SIZE, "128KB");
    bench_reconstruction(BENCH_LARGE_SIZE, "1MB");
    bench_reconstruction(BENCH_XLARGE_SIZE, "10MB");
    
    printf(COLOR_BOLD "\n━━━ Cryptographic Hashing ━━━" COLOR_RESET "\n");
    bench_crypto_hash(BENCH_SMALL_SIZE, "4KB");
    bench_crypto_hash(BENCH_MEDIUM_SIZE, "128KB");
    bench_crypto_hash(BENCH_LARGE_SIZE, "1MB");
    bench_crypto_hash(BENCH_XLARGE_SIZE, "10MB");
    
    /* Cleanup */
    buckets_cleanup();
    
    printf(COLOR_BOLD "\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  Benchmarks Complete\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf(COLOR_RESET "\n");
    
    return 0;
}
