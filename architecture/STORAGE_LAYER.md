# Buckets Storage Layer Architecture

## Status: DESIGN
**Version:** 1.0  
**Date:** February 25, 2026  
**Phase:** Week 12-16 (Storage Layer Implementation)

---

## Executive Summary

This document describes the **Storage Layer** design for Buckets, integrating all previously implemented components (erasure coding, hashing, cryptography) into a complete object storage system. The design follows MinIO's proven xl.meta approach while incorporating our flexible topology and erasure coding foundation.

### Key Design Principles

1. **Erasure-coded by default**: All objects split into K+M chunks using Reed-Solomon
2. **Cryptographic integrity**: Every chunk verified with BLAKE2b checksums
3. **Deterministic placement**: Consistent hashing determines chunk distribution
4. **Self-describing metadata**: Each object carries complete recovery information
5. **Atomic operations**: Write-then-rename pattern for consistency
6. **Inline small objects**: Objects <128KB stored inline in metadata for efficiency

### Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    Buckets Storage Layer                     │
├─────────────────────────────────────────────────────────────┤
│  Object API                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │
│  │ PutObject()  │  │ GetObject()  │  │ DeleteObject │     │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘     │
│         │                  │                  │              │
├─────────┼──────────────────┼──────────────────┼─────────────┤
│  Storage Engine                               │              │
│  ┌──────▼───────┐  ┌──────▼───────┐  ┌──────▼───────┐     │
│  │   Erasure    │  │  Chunk I/O   │  │  Metadata    │     │
│  │   Encoder    │  │   Manager    │  │   Manager    │     │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘     │
│         │                  │                  │              │
│  ┌──────▼──────────────────▼──────────────────▼───────┐    │
│  │              Disk Layout Manager                    │    │
│  │  • Path generation  • Directory structure           │    │
│  │  • Hash prefix      • Atomic operations             │    │
│  └──────┬──────────────────────────────────────────────┘    │
│         │                                                    │
├─────────┼────────────────────────────────────────────────────┤
│  Foundation (Already Implemented)                            │
│  ┌──────▼───────┐  ┌──────────────┐  ┌──────────────┐     │
│  │ Reed-Solomon │  │ BLAKE2b/SHA  │  │  SipHash/    │     │
│  │ (ISA-L)      │  │  Checksums   │  │  xxHash      │     │
│  └──────────────┘  └──────────────┘  └──────────────┘     │
└─────────────────────────────────────────────────────────────┘
```

---

## Table of Contents

1. [Disk Layout Design](#1-disk-layout-design)
2. [Object Metadata Format](#2-object-metadata-format)
3. [Erasure Coding Integration](#3-erasure-coding-integration)
4. [Chunk Distribution Strategy](#4-chunk-distribution-strategy)
5. [Read Operation Flow](#5-read-operation-flow)
6. [Write Operation Flow](#6-write-operation-flow)
7. [Data Structures](#7-data-structures)
8. [API Design](#8-api-design)
9. [Performance Considerations](#9-performance-considerations)
10. [Implementation Plan](#10-implementation-plan)

---

## 1. Disk Layout Design

### 1.1 Directory Structure

Following MinIO's proven xl.meta approach with Buckets-specific adaptations:

```
/disk1/
├── .buckets.sys/              # System metadata (already implemented)
│   ├── format.json            # Disk format (Week 2)
│   └── topology.json          # Cluster topology (Week 3)
│
└── .buckets/                  # Object storage root
    └── data/                  # Data directory
        └── <hash-prefix>/     # 2-character hash prefix (00-ff)
            └── <object-hash>/ # Full object hash (xxHash-64)
                ├── xl.meta    # Object metadata (self-describing)
                ├── part.1     # Data chunk 1 (or inline data)
                ├── part.2     # Data chunk 2
                ├── ...
                ├── part.K     # Data chunk K
                ├── part.K+1   # Parity chunk 1
                ├── ...
                └── part.K+M   # Parity chunk M
```

### 1.2 Path Generation Algorithm

```c
// Example: bucket="mybucket", object="photos/2024/image.jpg"
// Full object key: "mybucket/photos/2024/image.jpg"

// Step 1: Hash the object key
u64 hash = xxhash_hash(object_key, deployment_id);

// Step 2: Generate hash prefix (first 2 hex digits)
char prefix[3];
snprintf(prefix, sizeof(prefix), "%02x", (hash >> 56) & 0xFF);

// Step 3: Generate full object hash (16 hex characters)
char object_hash[17];
snprintf(object_hash, sizeof(object_hash), "%016lx", hash);

// Step 4: Construct path
// /disk/.buckets/data/a7/a7b3c9d2e5f81234/xl.meta
sprintf(path, "%s/.buckets/data/%s/%s/", 
        disk_path, prefix, object_hash);
```

**Benefits**:
- **256 top-level directories** (00-ff): Reduces inode contention
- **Unique object hash**: No collisions (64-bit xxHash with deployment ID seed)
- **Fast lookup**: O(1) path computation, no metadata scan needed
- **Filesystem friendly**: Balanced distribution across directories

### 1.3 File Naming Convention

```
xl.meta     - Object metadata (always present, 4KB-64KB typical)
part.1      - First data chunk (K data chunks total)
part.2      - Second data chunk
...
part.K      - Last data chunk
part.K+1    - First parity chunk (M parity chunks total)
...
part.K+M    - Last parity chunk
```

**Special Cases**:
- **Inline objects** (<128KB): Data stored in `xl.meta`, no part files
- **Large objects** (>5MB): May use multipart format (future enhancement)
- **Zero-byte objects**: Only `xl.meta` exists, no part files

---

## 2. Object Metadata Format

### 2.1 xl.meta Structure (JSON)

Based on MinIO's xl.meta v2 format, adapted for Buckets:

```json
{
  "version": 1,
  "format": "xl",
  "stat": {
    "size": 1048576,
    "modTime": "2026-02-25T10:30:00Z"
  },
  "erasure": {
    "algorithm": "ReedSolomon",
    "data": 8,
    "parity": 4,
    "blockSize": 131072,
    "index": 1,
    "distribution": [1, 3, 5, 7, 9, 11, 2, 4, 6, 8, 10, 12],
    "checksums": [
      {"algo": "BLAKE2b-256", "hash": "a7b3c9d2..."},
      {"algo": "BLAKE2b-256", "hash": "e5f81234..."},
      ...
    ]
  },
  "meta": {
    "content-type": "image/jpeg",
    "etag": "9b3d8f7a6c5e4d3b2a1f0e9d8c7b6a5f",
    "x-amz-meta-*": "user metadata..."
  },
  "inline": "base64-encoded-data-if-small"
}
```

### 2.2 Field Descriptions

**`version`**: Format version (starts at 1, increment on breaking changes)

**`format`**: Always "xl" (xl = erasure coded)

**`stat`**: Object statistics
- `size`: Actual object size in bytes (before erasure encoding)
- `modTime`: Last modification time (ISO 8601 format)

**`erasure`**: Erasure coding configuration
- `algorithm`: Always "ReedSolomon" for now
- `data`: Number of data chunks (K)
- `parity`: Number of parity chunks (M)
- `blockSize`: Size of each chunk in bytes (SIMD-aligned)
- `index`: Disk index within the erasure set (1-based)
- `distribution`: Chunk placement order (see §4.2)
- `checksums`: Array of BLAKE2b-256 hashes (one per chunk)
  - Stored in chunk order: [chunk1, chunk2, ..., chunkK+M]
  - Used for chunk verification on read

**`meta`**: S3-compatible metadata
- `content-type`: MIME type
- `etag`: S3-style ETag (MD5 or SHA-256 hex)
- `x-amz-meta-*`: User-defined metadata

**`inline`** (optional): Base64-encoded object data for small objects
- Present only if object size < 128KB
- Eliminates need for separate part files
- Reduces I/O operations for small objects

### 2.3 xl.meta Storage Format

**On-disk format**: Compact MessagePack (future) or JSON (initial implementation)

```c
// Write xl.meta atomically
int write_xl_meta(const char *object_path, const xl_meta_t *meta) {
    // Serialize to JSON
    char *json = xl_meta_to_json(meta);
    
    // Write atomically (temp + rename)
    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s/xl.meta.tmp.%d", 
             object_path, getpid());
    
    atomic_write_file(tmp_path, json, strlen(json));
    rename(tmp_path, "%s/xl.meta", object_path);
    
    free(json);
    return 0;
}
```

**Size considerations**:
- Typical xl.meta: 4-8 KB (no inline data)
- With inline data (<128KB object): 8-140 KB
- Large objects (many chunks): Up to 64 KB (checksums dominate)

---

## 3. Erasure Coding Integration

### 3.1 Encoding Strategy

**Default Configuration**: 8+4 (8 data, 4 parity)
- 50% storage overhead
- Tolerates 4 simultaneous disk failures
- Good balance of reliability vs. cost

**Supported Configurations**:
```c
// Small clusters (4-6 disks)
BUCKETS_EC_4_2    // 4+2: 50% overhead, 2 failures

// Medium clusters (8-12 disks)
BUCKETS_EC_8_4    // 8+4: 50% overhead, 4 failures (default)

// Large clusters (12-16 disks)
BUCKETS_EC_12_4   // 12+4: 33% overhead, 4 failures
BUCKETS_EC_16_4   // 16+4: 25% overhead, 4 failures
```

**Configuration selection**: Based on cluster size (from topology.json)
```c
u32 k, m;
if (cluster_size >= 16) {
    k = 16; m = 4;  // 25% overhead
} else if (cluster_size >= 12) {
    k = 12; m = 4;  // 33% overhead
} else if (cluster_size >= 8) {
    k = 8; m = 4;   // 50% overhead (default)
} else {
    k = 4; m = 2;   // 50% overhead (minimum)
}
```

### 3.2 Chunk Size Calculation

```c
// Calculate optimal chunk size for erasure coding
size_t calculate_chunk_size(size_t object_size, u32 k) {
    // Use ISA-L helper (already implemented)
    size_t chunk_size = buckets_ec_calc_chunk_size(object_size, k);
    
    // Clamp to reasonable range
    const size_t MIN_CHUNK = 64 * 1024;       // 64 KB
    const size_t MAX_CHUNK = 10 * 1024 * 1024; // 10 MB
    
    if (chunk_size < MIN_CHUNK) chunk_size = MIN_CHUNK;
    if (chunk_size > MAX_CHUNK) chunk_size = MAX_CHUNK;
    
    return chunk_size;
}
```

**Rationale**:
- **64 KB minimum**: Efficient disk I/O, good SIMD utilization
- **10 MB maximum**: Limits memory usage during encode/decode
- **SIMD-aligned**: ISA-L helper ensures 16-byte alignment

### 3.3 Inline Data Threshold

**Small object optimization**: Objects < 128KB stored inline in xl.meta

```c
#define INLINE_THRESHOLD  (128 * 1024)  // 128 KB

bool should_inline_object(size_t object_size) {
    return object_size < INLINE_THRESHOLD;
}
```

**Benefits**:
- **Reduced I/O**: Single metadata read instead of K+M chunk reads
- **Lower latency**: No chunk assembly needed
- **Fewer files**: Reduces inode usage (common for small objects)

**Trade-offs**:
- **Larger xl.meta**: Up to 140 KB vs. typical 4-8 KB
- **No erasure coding**: Inline data not erasure-coded (relies on xl.meta replication)
- **Memory**: Must read entire xl.meta into memory

**MinIO insight**: MinIO uses 256 KB threshold, we use 128 KB to be conservative

---

## 4. Chunk Distribution Strategy

### 4.1 Placement Algorithm

**Goal**: Distribute K+M chunks across disks deterministically

**Input**:
- Object key (e.g., "mybucket/photo.jpg")
- Cluster topology (N disks, S sets, D disks_per_set)
- Erasure config (K data, M parity)

**Algorithm**:
```c
// Step 1: Determine which erasure set this object belongs to
u64 object_hash = siphash_hash(object_key, deployment_id);
u32 set_index = object_hash % num_sets;

// Step 2: Get disks for this set (from topology)
disk_t *disks[K+M];
get_set_disks(topology, set_index, disks, K+M);

// Step 3: Generate chunk distribution order
// Uses Jump Consistent Hash for deterministic permutation
u32 distribution[K+M];
for (u32 i = 0; i < K+M; i++) {
    // Pseudo-random but deterministic permutation
    u32 chunk_hash = xxhash_hash_u64(object_hash ^ i);
    distribution[i] = jump_hash(chunk_hash, K+M);
}

// Step 4: Write chunks to disks in distribution order
for (u32 i = 0; i < K+M; i++) {
    u32 disk_idx = distribution[i];
    write_chunk_to_disk(disks[disk_idx], chunk_data[i]);
}
```

**Key properties**:
- **Deterministic**: Same object always maps to same disks
- **Balanced**: Chunks evenly distributed across disks in set
- **Reconstructable**: Distribution order stored in xl.meta

### 4.2 Distribution Array in xl.meta

The `distribution` field in xl.meta encodes chunk placement:

```json
"erasure": {
  "distribution": [1, 3, 5, 7, 9, 11, 2, 4, 6, 8, 10, 12],
  ...
}
```

**Interpretation**:
- Array length = K+M (total chunks)
- `distribution[i]` = disk index (1-based) for chunk `i`
- Example: Chunk 0 → disk 1, Chunk 1 → disk 3, etc.

**Usage**:
- **Write**: Compute distribution once, store in xl.meta, write chunks
- **Read**: Load xl.meta, use distribution to find chunks on disks
- **Reconstruct**: If disk 3 fails, find all chunks on disk 3, reconstruct from others

### 4.3 Example: 8+4 on 12-disk cluster

```
Object: "mybucket/photo.jpg"
Object hash: 0xa7b3c9d2e5f81234
Set index: hash % 2 = 0 (set 0)
Set 0 disks: [disk1, disk2, ..., disk12]

Chunk distribution (deterministic permutation):
  Chunk 0 (data)    → disk 1
  Chunk 1 (data)    → disk 3
  Chunk 2 (data)    → disk 5
  Chunk 3 (data)    → disk 7
  Chunk 4 (data)    → disk 9
  Chunk 5 (data)    → disk 11
  Chunk 6 (data)    → disk 2
  Chunk 7 (data)    → disk 4
  Chunk 8 (parity)  → disk 6
  Chunk 9 (parity)  → disk 8
  Chunk 10 (parity) → disk 10
  Chunk 11 (parity) → disk 12

xl.meta on each disk:
  disk1/xl.meta → {"erasure": {"index": 1, "distribution": [1,3,5,7,9,11,2,4,6,8,10,12]}}
  disk2/xl.meta → {"erasure": {"index": 7, "distribution": [1,3,5,7,9,11,2,4,6,8,10,12]}}
  ...
```

**Disk failure scenario**:
- Disk 3 fails → Chunk 1 (data) missing
- Read xl.meta from disk 1, see distribution
- Collect chunks from disks: [1,5,7,9,11,2,4,6,8,10,12] (11 chunks)
- Reconstruct chunk 1 using erasure decoding
- Serve object successfully

---

## 5. Read Operation Flow

### 5.1 High-Level Read Algorithm

```c
int buckets_get_object(const char *bucket, const char *object, 
                       void **data, size_t *size) {
    // 1. Compute object path
    char path[PATH_MAX];
    compute_object_path(bucket, object, path);
    
    // 2. Read xl.meta from first available disk
    xl_meta_t meta;
    if (read_xl_meta(path, &meta) != 0) {
        return BUCKETS_ERR_NOT_FOUND;
    }
    
    // 3. Check if inline data
    if (meta.inline_data) {
        *data = base64_decode(meta.inline_data, size);
        return BUCKETS_OK;
    }
    
    // 4. Read chunks from disks
    u8 *chunks[meta.erasure.data + meta.erasure.parity];
    int result = read_chunks(&meta, path, chunks);
    
    if (result == 0) {
        // All chunks available, assemble directly
        *data = assemble_chunks(chunks, meta.erasure.data, 
                                meta.erasure.blockSize);
        *size = meta.stat.size;
    } else {
        // Some chunks missing, use erasure decoding
        *data = decode_with_erasure(&meta, chunks);
        *size = meta.stat.size;
    }
    
    // 5. Verify checksums
    if (!verify_checksums(*data, *size, &meta)) {
        return BUCKETS_ERR_CHECKSUM;
    }
    
    return BUCKETS_OK;
}
```

### 5.2 Detailed Read Steps

**Step 1: Path Computation** (O(1))
```c
// Compute object hash and path
u64 hash = xxhash_hash(object_key, deployment_id);
char prefix[3], object_hash[17];
snprintf(prefix, 3, "%02x", (hash >> 56) & 0xFF);
snprintf(object_hash, 17, "%016lx", hash);

// Construct path: /disk/.buckets/data/a7/a7b3c9d2e5f81234/
```

**Step 2: Metadata Read** (single disk I/O)
```c
// Read xl.meta from first available disk in set
xl_meta_t meta;
for (u32 i = 0; i < num_disks_in_set; i++) {
    if (read_xl_meta(disks[i], path, &meta) == 0) {
        break;  // Success
    }
}
```

**Step 3: Inline Data Fast Path** (if applicable)
```c
if (meta.inline_data) {
    // Decode base64 inline data directly
    return base64_decode(meta.inline_data, data, size);
}
```

**Step 4: Chunk Collection** (parallel disk I/O)
```c
// Read K+M chunks in parallel (or as many as available)
u8 *chunks[K+M];
u32 available_count = 0;

#pragma omp parallel for
for (u32 i = 0; i < K+M; i++) {
    u32 disk_idx = meta.erasure.distribution[i];
    chunks[i] = read_chunk(disks[disk_idx], path, i+1);
    if (chunks[i]) {
        atomic_increment(&available_count);
    }
}
```

**Step 5: Data Assembly or Reconstruction**
```c
if (available_count >= K) {
    if (available_count == K+M) {
        // Fast path: all chunks available
        data = assemble_chunks_no_decode(chunks, K, blockSize);
    } else {
        // Slow path: some chunks missing, use erasure decode
        buckets_ec_ctx_t ctx;
        buckets_ec_init(&ctx, K, M);
        buckets_ec_decode(&ctx, chunks, blockSize, data, size);
        buckets_ec_free(&ctx);
    }
} else {
    return BUCKETS_ERR_INSUFFICIENT_CHUNKS;
}
```

**Step 6: Checksum Verification**
```c
// Verify each chunk's BLAKE2b checksum
for (u32 i = 0; i < K+M; i++) {
    if (!chunks[i]) continue;
    
    u8 computed[32];
    blake2b(computed, 32, chunks[i], blockSize, NULL, 0);
    
    if (!buckets_hash_verify(computed, meta.erasure.checksums[i].hash)) {
        // Chunk corrupted, try reconstruction
        chunks[i] = NULL;
        available_count--;
        if (available_count < K) {
            return BUCKETS_ERR_CHECKSUM;
        }
    }
}
```

### 5.3 Read Performance Profile

**Best case** (all chunks available, no errors):
- 1 disk I/O for xl.meta (4-8 KB)
- K parallel disk I/Os for data chunks
- No erasure decoding needed
- **Latency**: ~5-10ms (typical HDD seek time)

**Worst case** (M chunks missing):
- 1 disk I/O for xl.meta
- K parallel disk I/Os (only K chunks available)
- Erasure decode required
- **Latency**: ~10-20ms (decode adds 5-10ms for MB-sized objects)

**Inline data** (<128KB objects):
- 1 disk I/O for xl.meta only
- No chunk assembly
- **Latency**: ~2-5ms

---

## 6. Write Operation Flow

### 6.1 High-Level Write Algorithm

```c
int buckets_put_object(const char *bucket, const char *object,
                       const void *data, size_t size) {
    // 1. Check if should inline
    if (should_inline_object(size)) {
        return write_inline_object(bucket, object, data, size);
    }
    
    // 2. Compute object path
    char path[PATH_MAX];
    compute_object_path(bucket, object, path);
    
    // 3. Initialize erasure coding
    buckets_ec_ctx_t ctx;
    u32 k, m;
    select_erasure_config(size, &k, &m);
    buckets_ec_init(&ctx, k, m);
    
    // 4. Encode data into chunks
    size_t chunk_size = calculate_chunk_size(size, k);
    u8 *data_chunks[k];
    u8 *parity_chunks[m];
    allocate_chunks(data_chunks, k, chunk_size);
    allocate_chunks(parity_chunks, m, chunk_size);
    
    buckets_ec_encode(&ctx, data, size, chunk_size, 
                      data_chunks, parity_chunks);
    
    // 5. Compute checksums for all chunks
    u8 checksums[k+m][32];
    for (u32 i = 0; i < k+m; i++) {
        u8 *chunk = (i < k) ? data_chunks[i] : parity_chunks[i-k];
        blake2b(checksums[i], 32, chunk, chunk_size, NULL, 0);
    }
    
    // 6. Create xl.meta
    xl_meta_t meta;
    populate_xl_meta(&meta, size, k, m, chunk_size, checksums);
    
    // 7. Write chunks to disks atomically
    write_result_t result = write_chunks_atomic(path, &meta, 
                                                 data_chunks, parity_chunks);
    
    // 8. Cleanup
    free_chunks(data_chunks, k);
    free_chunks(parity_chunks, m);
    buckets_ec_free(&ctx);
    
    return result;
}
```

### 6.2 Detailed Write Steps

**Step 1: Inline Decision**
```c
bool should_inline_object(size_t size) {
    return size < INLINE_THRESHOLD;  // 128 KB
}

int write_inline_object(const char *bucket, const char *object,
                        const void *data, size_t size) {
    // Encode data as base64
    char *base64 = base64_encode(data, size);
    
    // Create xl.meta with inline data
    xl_meta_t meta = {
        .version = 1,
        .format = "xl",
        .stat = {.size = size, .modTime = current_time()},
        .inline_data = base64
    };
    
    // Write xl.meta only (no part files)
    char path[PATH_MAX];
    compute_object_path(bucket, object, path);
    
    // Write to all disks in set for redundancy
    for (u32 i = 0; i < num_disks_in_set; i++) {
        write_xl_meta(disks[i], path, &meta);
    }
    
    free(base64);
    return BUCKETS_OK;
}
```

**Step 2: Erasure Configuration Selection**
```c
void select_erasure_config(size_t object_size, u32 *k, u32 *m) {
    // Get cluster size from topology
    u32 cluster_size = get_cluster_size(topology);
    
    // Select configuration based on cluster size
    if (cluster_size >= 16) {
        *k = 16; *m = 4;  // 25% overhead
    } else if (cluster_size >= 12) {
        *k = 12; *m = 4;  // 33% overhead
    } else if (cluster_size >= 8) {
        *k = 8; *m = 4;   // 50% overhead (default)
    } else {
        *k = 4; *m = 2;   // 50% overhead (minimum)
    }
}
```

**Step 3: Erasure Encoding** (already implemented in Week 10-11)
```c
// Use our existing erasure coding API
buckets_ec_ctx_t ctx;
buckets_ec_init(&ctx, k, m);

size_t chunk_size = buckets_ec_calc_chunk_size(object_size, k);

u8 *data_chunks[k], *parity_chunks[m];
// Allocate chunk buffers...

buckets_ec_encode(&ctx, data, object_size, chunk_size,
                  data_chunks, parity_chunks);
```

**Step 4: Checksum Computation** (BLAKE2b for each chunk)
```c
u8 checksums[k+m][32];

// Compute BLAKE2b-256 for each chunk
for (u32 i = 0; i < k; i++) {
    blake2b(checksums[i], 32, data_chunks[i], chunk_size, NULL, 0);
}
for (u32 i = 0; i < m; i++) {
    blake2b(checksums[k+i], 32, parity_chunks[i], chunk_size, NULL, 0);
}
```

**Step 5: xl.meta Creation**
```c
void populate_xl_meta(xl_meta_t *meta, size_t size, u32 k, u32 m,
                      size_t chunk_size, u8 checksums[][32]) {
    meta->version = 1;
    meta->format = "xl";
    meta->stat.size = size;
    meta->stat.modTime = current_time_iso8601();
    
    meta->erasure.algorithm = "ReedSolomon";
    meta->erasure.data = k;
    meta->erasure.parity = m;
    meta->erasure.blockSize = chunk_size;
    
    // Compute distribution (deterministic permutation)
    compute_chunk_distribution(object_key, k+m, meta->erasure.distribution);
    
    // Copy checksums
    for (u32 i = 0; i < k+m; i++) {
        meta->erasure.checksums[i].algo = "BLAKE2b-256";
        memcpy(meta->erasure.checksums[i].hash, checksums[i], 32);
    }
}
```

**Step 6: Atomic Write to Disks**
```c
int write_chunks_atomic(const char *path, const xl_meta_t *meta,
                        u8 **data_chunks, u8 **parity_chunks) {
    // Step 6a: Create object directory on all disks
    for (u32 i = 0; i < k+m; i++) {
        u32 disk_idx = meta->erasure.distribution[i];
        mkdir_recursive(disks[disk_idx], path);
    }
    
    // Step 6b: Write chunks to temp files
    char tmp_paths[k+m][PATH_MAX];
    for (u32 i = 0; i < k+m; i++) {
        u32 disk_idx = meta->erasure.distribution[i];
        snprintf(tmp_paths[i], PATH_MAX, "%s/part.%u.tmp.%d",
                 path, i+1, getpid());
        
        u8 *chunk = (i < k) ? data_chunks[i] : parity_chunks[i-k];
        atomic_write_file(disks[disk_idx], tmp_paths[i], 
                         chunk, meta->erasure.blockSize);
    }
    
    // Step 6c: Write xl.meta to temp files
    char meta_tmp_paths[k+m][PATH_MAX];
    for (u32 i = 0; i < k+m; i++) {
        u32 disk_idx = meta->erasure.distribution[i];
        snprintf(meta_tmp_paths[i], PATH_MAX, "%s/xl.meta.tmp.%d",
                 path, getpid());
        
        // Set disk index in xl.meta (each disk has own index)
        xl_meta_t disk_meta = *meta;
        disk_meta.erasure.index = i + 1;
        
        write_xl_meta(disks[disk_idx], meta_tmp_paths[i], &disk_meta);
    }
    
    // Step 6d: Atomic rename (commit point)
    for (u32 i = 0; i < k+m; i++) {
        u32 disk_idx = meta->erasure.distribution[i];
        
        // Rename part file
        rename(disks[disk_idx], tmp_paths[i], "%s/part.%u", path, i+1);
        
        // Rename xl.meta
        rename(disks[disk_idx], meta_tmp_paths[i], "%s/xl.meta", path);
    }
    
    return BUCKETS_OK;
}
```

### 6.3 Write Atomicity Guarantees

**Crash scenarios**:

1. **Crash before any renames**: Temp files left behind, cleaned up by scrubber
2. **Crash during renames**: Partial writes visible, but xl.meta not complete → object not readable until all renames complete
3. **Crash after all renames**: Write committed, object readable

**Quorum-based commit**: Require K successful writes before declaring success
```c
u32 success_count = 0;
for (u32 i = 0; i < k+m; i++) {
    if (write_chunk(i) == 0) {
        success_count++;
    }
}

if (success_count < k) {
    // Rollback: delete temp files
    rollback_write(path);
    return BUCKETS_ERR_WRITE_FAILED;
}

return BUCKETS_OK;  // At least K chunks written, object readable
```

### 6.4 Write Performance Profile

**Small objects** (<128KB, inline):
- Base64 encoding: ~1ms
- K+M xl.meta writes: ~5-10ms (parallel)
- **Total**: ~10-15ms

**Medium objects** (1MB, 8+4):
- Erasure encoding: ~5ms (ISA-L)
- BLAKE2b checksums: ~2ms (12 chunks)
- 12 chunk writes (parallel): ~20-30ms (HDD)
- 12 xl.meta writes (parallel): ~5-10ms
- **Total**: ~40-50ms

**Large objects** (100MB, 16+4):
- Erasure encoding: ~100ms (ISA-L)
- BLAKE2b checksums: ~20ms (20 chunks)
- 20 chunk writes (parallel): ~200-300ms (HDD)
- 20 xl.meta writes (parallel): ~10-20ms
- **Total**: ~350-450ms

---

## 7. Data Structures

### 7.1 Core Structures

```c
/**
 * Object metadata (xl.meta)
 */
typedef struct {
    u32 version;                    // Format version
    char format[8];                 // "xl"
    
    struct {
        size_t size;                // Object size (bytes)
        char modTime[32];           // ISO 8601 timestamp
    } stat;
    
    struct {
        char algorithm[16];         // "ReedSolomon"
        u32 data;                   // K (data chunks)
        u32 parity;                 // M (parity chunks)
        size_t blockSize;           // Chunk size (bytes)
        u32 index;                  // This disk's index (1-based)
        u32 *distribution;          // Chunk placement (length: K+M)
        
        struct {
            char algo[16];          // "BLAKE2b-256"
            u8 hash[32];            // Checksum
        } *checksums;               // Array of K+M checksums
    } erasure;
    
    struct {
        char *content_type;
        char *etag;
        // ... other S3 metadata
    } meta;
    
    char *inline_data;              // Base64 (optional, for small objects)
} xl_meta_t;

/**
 * Storage configuration
 */
typedef struct {
    char *data_dir;                 // /disk/.buckets/data/
    u32 inline_threshold;           // 128 KB default
    u32 default_ec_k;               // 8 (default)
    u32 default_ec_m;               // 4 (default)
    bool verify_checksums;          // true (default)
} storage_config_t;

/**
 * Object handle (for reads/writes)
 */
typedef struct {
    char *bucket;
    char *object;
    char path[PATH_MAX];            // Computed object path
    xl_meta_t meta;
    u8 **chunks;                    // Chunk data (K+M pointers)
    size_t chunk_size;
    bool is_open;
} object_handle_t;
```

### 7.2 Path Utilities

```c
/**
 * Compute object path from bucket + object key
 */
void compute_object_path(const char *bucket, const char *object,
                         char *path, size_t path_len);

/**
 * Compute hash prefix (00-ff)
 */
void compute_hash_prefix(u64 hash, char *prefix, size_t prefix_len);

/**
 * Compute full object hash (16 hex chars)
 */
void compute_object_hash(const char *object_key, char *hash, size_t hash_len);
```

---

## 8. API Design

### 8.1 Public Storage API (`include/buckets_storage.h`)

```c
/**
 * Initialize storage system
 */
int buckets_storage_init(const storage_config_t *config);

/**
 * Cleanup storage system
 */
void buckets_storage_cleanup(void);

/**
 * Put object (write)
 */
int buckets_put_object(const char *bucket, const char *object,
                       const void *data, size_t size,
                       const char *content_type);

/**
 * Get object (read)
 */
int buckets_get_object(const char *bucket, const char *object,
                       void **data, size_t *size);

/**
 * Delete object
 */
int buckets_delete_object(const char *bucket, const char *object);

/**
 * Head object (metadata only)
 */
int buckets_head_object(const char *bucket, const char *object,
                        xl_meta_t *meta);

/**
 * Stat object (size and modification time)
 */
int buckets_stat_object(const char *bucket, const char *object,
                        size_t *size, char *modTime);
```

### 8.2 Internal Storage API

```c
/**
 * xl.meta operations
 */
int read_xl_meta(const char *disk_path, const char *object_path,
                 xl_meta_t *meta);
int write_xl_meta(const char *disk_path, const char *object_path,
                  const xl_meta_t *meta);
void xl_meta_free(xl_meta_t *meta);

/**
 * Chunk operations
 */
int read_chunk(const char *disk_path, const char *object_path,
               u32 chunk_index, void **data, size_t *size);
int write_chunk(const char *disk_path, const char *object_path,
                u32 chunk_index, const void *data, size_t size);
int verify_chunk(const void *data, size_t size, const u8 *checksum);

/**
 * Erasure coding integration
 */
int encode_object(const void *data, size_t size, u32 k, u32 m,
                  u8 **data_chunks, u8 **parity_chunks, size_t *chunk_size);
int decode_object(u8 **chunks, u32 k, u32 m, size_t chunk_size,
                  void **data, size_t *size);
```

---

## 9. Performance Considerations

### 9.1 Optimization Strategies

**1. Inline Small Objects** (<128KB)
- Eliminates K+M chunk files
- Reduces read latency from ~10ms to ~2-5ms
- Saves ~60% of inodes for typical workloads (many small files)

**2. Parallel Disk I/O**
```c
// Use OpenMP for parallel chunk I/O
#pragma omp parallel for
for (u32 i = 0; i < k+m; i++) {
    write_chunk(disks[i], path, chunks[i]);
}
```

**3. Read-Ahead for Sequential Access**
```c
// Prefetch next chunks during sequential reads
if (sequential_access_detected()) {
    prefetch_chunks(object_path, current_offset + chunk_size);
}
```

**4. Checksum Verification (Optional)**
```c
// Allow disabling checksum verification for trusted environments
if (config->verify_checksums) {
    verify_all_chunks(chunks, checksums);
}
```

**5. Memory Pooling**
```c
// Reuse chunk buffers to avoid allocation overhead
static __thread u8 *chunk_pool[MAX_CHUNKS];
u8 *allocate_chunk(size_t size) {
    if (chunk_pool[0]) {
        return chunk_pool[0];  // Reuse
    }
    return buckets_malloc(size);
}
```

### 9.2 Benchmarking Targets

**Read Performance**:
- Inline objects (<128KB): <5ms (target: 2-3ms)
- Medium objects (1MB): <50ms (target: 30-40ms)
- Large objects (100MB): <3s (target: 2-2.5s)

**Write Performance**:
- Inline objects (<128KB): <20ms (target: 10-15ms)
- Medium objects (1MB): <100ms (target: 50-80ms)
- Large objects (100MB): <5s (target: 3-4s)

**Throughput** (8+4 configuration):
- Read: >500 MB/s (target: 800-1000 MB/s)
- Write: >300 MB/s (target: 500-700 MB/s)

---

## 10. Implementation Plan

### 10.1 Week 12: Disk I/O & Object Primitives

**Files to create**:
- `src/storage/object.c` (400 lines): Object read/write operations
- `src/storage/layout.c` (200 lines): Path computation and directory management
- `src/storage/chunk.c` (300 lines): Chunk I/O operations
- `src/storage/metadata.c` (400 lines): xl.meta serialization/deserialization
- `include/buckets_storage.h` (250 lines): Public API
- `tests/storage/test_object.c` (500 lines): Comprehensive test suite

**Tasks**:
1. ✅ Implement path computation (`compute_object_path`, `compute_hash_prefix`)
2. ✅ Implement xl.meta serialization (JSON format)
3. ✅ Implement chunk write operations (atomic writes)
4. ✅ Implement chunk read operations (with error handling)
5. ✅ Implement inline object optimization
6. ✅ Write unit tests (20+ tests)
7. ✅ Integration test: Write object, read back, verify

**Acceptance criteria**:
- All tests passing (100%)
- Write object → read back → data matches
- Checksum verification working
- Inline objects (<128KB) working
- xl.meta format matches design

### 10.2 Week 13: Object Metadata & Versioning

**Focus**: Extended attributes, S3 metadata, versioning support

**Tasks**:
- Implement S3-compatible metadata (content-type, etag, user metadata)
- Add versioning support (version ID generation)
- Implement HEAD operation (metadata-only)
- Add metadata caching layer
- Performance tuning

### 10.3 Week 14: Integration (Hashing + Crypto + Erasure)

**Focus**: Full integration of all components

**Tasks**:
- Integrate SipHash for set selection
- Integrate BLAKE2b for checksums
- Integrate Reed-Solomon for erasure coding
- End-to-end testing (write → read → verify)
- Performance benchmarking
- Documentation updates

### 10.4 Week 15-16: Storage Backend

**Focus**: Multi-disk management, health monitoring, scrubbing

**Tasks**:
- Disk health monitoring (SMART attributes)
- Space allocation tracking
- Background scrubber (detect bitrot)
- Automatic chunk reconstruction
- Disk failure simulation and recovery testing

---

## Appendix A: MinIO xl.meta Reference

From `minio-reference/cmd/xl-storage-format-v2.go`:

```go
// xlMetaV2 - XL meta version 2 (msgpack format)
type xlMetaV2 struct {
    Versions []xlMetaV2Version `msg:"Versions"`
    // Data blobs for inline data
    data []byte
}

type xlMetaV2Version struct {
    Type       VersionType       `msg:"Type"`
    ObjectV2   *xlMetaV2Object   `msg:"ObjectV2,omitempty"`
    DeleteMarker *xlMetaV2DeleteMarker `msg:"DeleteMarker,omitempty"`
}

type xlMetaV2Object struct {
    VersionID    [16]byte           `msg:"ID"`
    DataDir      [16]byte           `msg:"DDir"`
    ErasureAlgorithm ErasureAlgo    `msg:"EcAlgo"`
    ErasureM     int                `msg:"EcM"`
    ErasureN     int                `msg:"EcN"`
    ErasureBlockSize int            `msg:"EcBSize"`
    ErasureIndex int                `msg:"EcIndex"`
    ErasureDist  []uint8            `msg:"EcDist"`
    PartNumbers  []int              `msg:"PartNums,omitempty"`
    PartETags    []string           `msg:"PartETags,omitempty"`
    PartSizes    []int64            `msg:"PartSizes,omitempty"`
    Size         int64              `msg:"Size"`
    ModTime      int64              `msg:"MTime"`
    MetaSys      map[string][]byte  `msg:"MetaSys,omitempty"`
    MetaUser     map[string]string  `msg:"MetaUsr,omitempty"`
}
```

**Key insights from MinIO**:
- Uses MessagePack for compact serialization (we'll start with JSON)
- Supports versioning (we'll add in Week 13)
- Inline data stored in separate blob (we'll use base64 in JSON)
- Erasure config stored per-object (flexible)

---

## Appendix B: Disk Layout Examples

### Example 1: Small Object (10 KB, inline)

```
/disk1/.buckets/data/a7/a7b3c9d2e5f81234/
└── xl.meta  (16 KB - contains base64-encoded data)
```

### Example 2: Medium Object (1 MB, 8+4)

```
/disk1/.buckets/data/a7/a7b3c9d2e5f81234/
├── xl.meta   (8 KB)
├── part.1    (128 KB - data chunk 1)
└── ...

/disk2/.buckets/data/a7/a7b3c9d2e5f81234/
├── xl.meta   (8 KB)
├── part.7    (128 KB - data chunk 7)
└── ...

... (12 disks total, each with xl.meta + 1 chunk)
```

### Example 3: Large Object (100 MB, 16+4)

```
Chunk size: 6.25 MB per chunk (100 MB / 16)

/disk1/.buckets/data/a7/a7b3c9d2e5f81234/
├── xl.meta   (12 KB - 20 checksums)
└── part.1    (6.25 MB)

/disk2/.buckets/data/a7/a7b3c9d2e5f81234/
├── xl.meta   (12 KB)
└── part.2    (6.25 MB)

... (20 disks total, each with xl.meta + 1 chunk)
```

---

## Appendix C: Error Codes

```c
typedef enum {
    BUCKETS_OK = 0,
    BUCKETS_ERR_NOT_FOUND = -1,
    BUCKETS_ERR_CHECKSUM = -2,
    BUCKETS_ERR_INSUFFICIENT_CHUNKS = -3,
    BUCKETS_ERR_WRITE_FAILED = -4,
    BUCKETS_ERR_READ_FAILED = -5,
    BUCKETS_ERR_INVALID_META = -6,
    BUCKETS_ERR_NOMEM = -7,
    BUCKETS_ERR_IO = -8,
} buckets_error_t;
```

---

**End of Storage Layer Design Document**

This design provides a complete blueprint for implementing the storage layer in Weeks 12-16. All components integrate cleanly with existing work (erasure coding, hashing, crypto) and follow MinIO's proven patterns while adapting to Buckets' architecture.
