# Buckets Cluster and State Management Architecture

**Version:** 1.0  
**Date:** February 25, 2026  
**Reference:** Based on MinIO analysis and Buckets requirements

---

## Overview

This document describes how Buckets manages cluster state, topology, and configuration. It covers the data structures, persistence mechanisms, and initialization sequences needed for a distributed object storage cluster.

---

## Core Concepts

### Cluster Hierarchy

```
Cluster (identified by deployment_id)
  └─ Pools (1 or more)
      └─ Sets (erasure coding groups)
          └─ Disks (individual storage devices)
```

**Key Properties:**
- **Deployment ID**: Unique cluster-wide identifier (UUID), immutable
- **Pool**: Group of erasure sets, can be added dynamically
- **Set**: Erasure coding group (4-16 disks), fixed after creation
- **Disk**: Individual storage device with unique UUID

---

## 1. Persistent State Files

### A. Format Metadata (`format.json`)

**Purpose**: Disk-level metadata defining cluster membership and topology

**Location**: `<disk>/.buckets.sys/format.json`

**Structure**:
```c
typedef struct {
    char version[16];           // Format version ("1")
    char format_type[16];       // Backend type ("erasure")
    char deployment_id[37];     // Cluster UUID (36 chars + null)
} buckets_format_meta_t;

typedef struct {
    buckets_format_meta_t meta;
    
    struct {
        char version[16];        // Erasure format version ("3")
        char this_disk[37];      // This disk's UUID
        
        // Set topology: [set_count][disks_per_set] = disk_uuid
        char ***sets;
        int set_count;
        int disks_per_set;
        
        char distribution_algo[32];  // "SIPMOD+PARITY"
    } erasure;
} buckets_format_t;
```

**Example JSON**:
```json
{
  "version": "1",
  "format": "erasure",
  "id": "f47ac10b-58cc-4372-a567-0e02b2c3d479",
  "erasure": {
    "version": "3",
    "this": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
    "sets": [
      [
        "disk-uuid-1",
        "disk-uuid-2",
        "disk-uuid-3",
        "disk-uuid-4"
      ],
      [
        "disk-uuid-5",
        "disk-uuid-6",
        "disk-uuid-7",
        "disk-uuid-8"
      ]
    ],
    "distribution_algo": "SIPMOD+PARITY"
  }
}
```

**Key Invariants**:
1. All disks in cluster have same `deployment_id`
2. All disks in cluster have same `sets` structure
3. Each disk has unique `this` UUID
4. Set topology is immutable after initialization

### B. Topology Metadata (`topology.json`)

**Purpose**: Dynamic cluster topology for consistent hashing (Buckets-specific)

**Location**: `<disk>/.buckets.sys/topology.json`

**Structure**:
```c
typedef enum {
    SET_STATE_ACTIVE,
    SET_STATE_DRAINING,
    SET_STATE_REMOVED
} buckets_set_state_t;

typedef struct {
    char endpoint[256];      // "http://node1:9000/mnt/disk1"
    char uuid[37];           // Disk UUID
    u64 capacity;            // Bytes
} buckets_disk_info_t;

typedef struct {
    int idx;                      // Set index
    buckets_set_state_t state;    // Active, draining, removed
    buckets_disk_info_t *disks;   // Array of disks
    int disk_count;
} buckets_set_topology_t;

typedef struct {
    int idx;                           // Pool index
    buckets_set_topology_t *sets;      // Array of sets
    int set_count;
} buckets_pool_topology_t;

typedef struct {
    int version;                       // Topology version (1)
    i64 generation;                    // Increments on changes
    char deployment_id[37];            // Cluster UUID
    int vnode_factor;                  // Virtual nodes per set (150)
    
    buckets_pool_topology_t *pools;    // Array of pools
    int pool_count;
} buckets_cluster_topology_t;
```

**Example JSON**:
```json
{
  "version": 1,
  "generation": 42,
  "deployment_id": "f47ac10b-58cc-4372-a567-0e02b2c3d479",
  "vnode_factor": 150,
  "pools": [
    {
      "idx": 0,
      "sets": [
        {
          "idx": 0,
          "state": "active",
          "disks": [
            {
              "endpoint": "http://node1:9000/mnt/disk1",
              "uuid": "disk-uuid-1",
              "capacity": 10995116277760
            }
          ]
        }
      ]
    }
  ]
}
```

### C. Pool Metadata (`pool.bin`)

**Purpose**: Pool status and decommission state (optional for MVP)

**Location**: `<disk>/.buckets.sys/pool.bin`

**Structure**:
```c
typedef struct {
    i64 start_time;              // Unix timestamp (microseconds)
    i64 start_size;              // Bytes at start
    i64 total_size;              // Total bytes to migrate
    i64 current_size;            // Bytes remaining
    i64 bytes_done;              // Bytes migrated
    bool complete;
    bool failed;
    bool canceled;
} buckets_decommission_info_t;

typedef struct {
    int id;                               // Pool index
    char cmdline[1024];                   // Command line used
    i64 last_update;                      // Unix timestamp (microseconds)
    buckets_decommission_info_t *decom;   // NULL if not decommissioning
} buckets_pool_status_t;

typedef struct {
    int version;                          // Metadata version (1)
    buckets_pool_status_t *pools;         // Array of pool statuses
    int pool_count;
} buckets_pool_meta_t;
```

---

## 2. In-Memory State

### A. Cluster Configuration

```c
typedef struct {
    char *url_string;        // "http://node1:9000/mnt/disk1"
    bool is_local;           // Is this endpoint on current node?
    int pool_idx;            // Pool index
    int set_idx;             // Set index within pool
    int disk_idx;            // Disk index within set
} buckets_endpoint_t;

typedef struct {
    buckets_endpoint_t *endpoints;
    int endpoint_count;
    int set_count;
    int drives_per_set;
    char cmdline[1024];
} buckets_pool_endpoints_t;

typedef struct {
    buckets_pool_endpoints_t *pools;
    int pool_count;
} buckets_endpoint_pools_t;
```

### B. Runtime Server State

```c
typedef struct {
    buckets_set_topology_t *sets;        // Erasure sets
    buckets_endpoint_t *endpoints;       // Pool endpoints
    int set_count;
    int set_drive_count;
    char distribution_algo[32];
    int default_parity_count;
} buckets_erasure_sets_t;

typedef struct {
    pthread_rwlock_t pool_meta_mutex;
    buckets_pool_meta_t pool_meta;
    
    u8 deployment_id[16];                     // Binary deployment ID
    char distribution_algo[32];
    
    buckets_erasure_sets_t **server_pools;    // Array of pools
    int pool_count;
    
    // Location registry and consistent hash ring
    void *location_registry;
    void *consistent_hash_ring;
} buckets_server_pools_t;
```

### C. Global State Variables

```c
// Global cluster configuration (initialized at startup)
extern buckets_endpoint_pools_t *g_endpoints;
extern u8 g_deployment_id[16];
extern char g_local_node_name[256];

// Global server pools instance
extern buckets_server_pools_t *g_server_pools;
```

---

## 3. Initialization Sequence

### Startup Flow

```
┌─────────────────────────────────────────────────────────────┐
│ 1. PARSE COMMAND LINE                                        │
│    - Read endpoints from CLI/ENV                             │
│    - Parse pool configuration                                │
│    - Create endpoint structures                              │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. RESOLVE ENDPOINTS                                         │
│    - DNS resolution for all endpoints                        │
│    - Determine local vs remote endpoints                     │
│    - Wait for DNS in orchestrated environments               │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. INITIALIZE STORAGE                                        │
│    - Create StorageAPI for each endpoint                     │
│    - Local: Direct disk I/O                                  │
│    - Remote: RPC client                                      │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. LOAD OR CREATE FORMAT                                    │
│    - Load format.json from all disks                         │
│    - Check consistency (deployment ID, topology)             │
│    - If unformatted: Initialize new cluster                  │
│    - Wait for quorum                                         │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 5. VALIDATE FORMAT                                           │
│    - Ensure all disks have matching deployment ID           │
│    - Validate set structure                                  │
│    - Verify disk UUIDs in correct positions                  │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 6. CREATE ERASURE SETS                                       │
│    - Group disks into erasure sets                           │
│    - Initialize erasure coding engine                        │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 7. LOAD TOPOLOGY (Buckets-specific)                         │
│    - Load topology.json from any disk                        │
│    - If not found: Create from format.json                   │
│    - Initialize consistent hash ring                         │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 8. INITIALIZE LOCATION REGISTRY                              │
│    - Create .buckets-registry bucket                         │
│    - Initialize LRU cache                                    │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 9. START NETWORK SERVICES                                    │
│    - Initialize peer RPC (Grid)                              │
│    - Register RPC handlers                                   │
│    - Establish peer connections                              │
└─────────────────────────────────────────────────────────────┘
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ 10. READY                                                    │
│     - Server accepts requests                                │
└─────────────────────────────────────────────────────────────┘
```

### Key Functions (to be implemented)

```c
// Format management
buckets_format_t* buckets_format_load(buckets_storage_t *disk);
int buckets_format_save(buckets_storage_t *disk, buckets_format_t *format);
buckets_format_t* buckets_format_create_new(int set_count, int drives_per_set);
int buckets_format_validate(buckets_format_t **formats, int count);

// Topology management
buckets_cluster_topology_t* buckets_topology_load(buckets_storage_t *disk);
int buckets_topology_save(buckets_storage_t *disk, buckets_cluster_topology_t *topology);
buckets_cluster_topology_t* buckets_topology_from_format(buckets_format_t *format);

// Endpoint resolution
int buckets_endpoints_resolve(buckets_endpoint_pools_t *endpoints);
int buckets_endpoints_update_local(buckets_endpoint_pools_t *endpoints);

// Initialization
buckets_server_pools_t* buckets_server_pools_new(buckets_endpoint_pools_t *endpoints);
int buckets_server_pools_init(buckets_server_pools_t *pools);
```

---

## 4. State Persistence Strategy

### Write Operations

**Atomic Write Pattern**:
```c
int buckets_atomic_write(buckets_storage_t *disk, const char *path, 
                         const void *data, size_t len)
{
    // 1. Generate temporary filename
    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp.%s", path, buckets_uuid_generate());
    
    // 2. Write to temporary file
    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, data, len);
    fsync(fd);  // Ensure data on disk
    close(fd);
    
    // 3. Atomic rename
    rename(temp_path, path);
    
    // 4. Sync directory
    int dir_fd = open(dirname(path), O_RDONLY);
    fsync(dir_fd);
    close(dir_fd);
    
    return 0;
}
```

**Quorum Write Pattern**:
```c
int buckets_quorum_write(buckets_storage_t **disks, int disk_count,
                         const char *path, const void *data, size_t len)
{
    int success_count = 0;
    int quorum = (disk_count / 2) + 1;
    
    // Write to all disks in parallel
    for (int i = 0; i < disk_count; i++) {
        if (buckets_atomic_write(disks[i], path, data, len) == 0) {
            success_count++;
        }
    }
    
    // Check if quorum achieved
    if (success_count >= quorum) {
        return BUCKETS_OK;
    }
    
    return BUCKETS_ERR_QUORUM;
}
```

### Read Operations

**Quorum Read Pattern**:
```c
buckets_result_t buckets_quorum_read(buckets_storage_t **disks, int disk_count,
                                     const char *path)
{
    buckets_result_t result;
    int quorum = disk_count / 2;
    
    // Hash table: content_hash -> count
    buckets_hash_table_t *votes = buckets_hash_table_new(16, hash_bytes, cmp_bytes);
    
    // Read from all disks
    for (int i = 0; i < disk_count; i++) {
        void *data;
        size_t len;
        
        if (buckets_read_file(disks[i], path, &data, &len) == 0) {
            u64 hash = buckets_hash_data(data, len);
            int *count = buckets_hash_table_get(votes, &hash);
            
            if (count) {
                (*count)++;
            } else {
                int *new_count = buckets_malloc(sizeof(int));
                *new_count = 1;
                buckets_hash_table_insert(votes, &hash, new_count);
            }
            
            // Check if quorum reached
            if (*count >= quorum) {
                result.data = data;
                result.error = BUCKETS_OK;
                buckets_hash_table_free(votes);
                return result;
            }
        }
    }
    
    result.data = NULL;
    result.error = BUCKETS_ERR_QUORUM;
    buckets_hash_table_free(votes);
    return result;
}
```

---

## 5. Synchronization Across Nodes

### Notification System

When state changes (topology, pool metadata), notify all peers:

```c
typedef struct {
    buckets_peer_client_t **clients;
    int client_count;
} buckets_notification_sys_t;

// Broadcast topology reload to all peers
int buckets_notify_topology_reload(buckets_notification_sys_t *sys, 
                                   buckets_cluster_topology_t *topology)
{
    for (int i = 0; i < sys->client_count; i++) {
        // Async RPC call to peer
        buckets_peer_client_reload_topology(sys->clients[i], topology);
    }
    return BUCKETS_OK;
}
```

### Generation-Based Consistency

Use generation numbers to detect stale reads:

```c
typedef struct {
    buckets_cluster_topology_t *topology;
    i64 generation;
    pthread_rwlock_t lock;
} buckets_topology_cache_t;

buckets_cluster_topology_t* buckets_topology_get(buckets_topology_cache_t *cache)
{
    pthread_rwlock_rdlock(&cache->lock);
    buckets_cluster_topology_t *topo = cache->topology;
    pthread_rwlock_unlock(&cache->lock);
    return topo;
}

int buckets_topology_update(buckets_topology_cache_t *cache, 
                            buckets_cluster_topology_t *new_topology)
{
    pthread_rwlock_wrlock(&cache->lock);
    
    // Only update if generation is newer
    if (new_topology->generation > cache->topology->generation) {
        buckets_topology_free(cache->topology);
        cache->topology = new_topology;
        cache->generation = new_topology->generation;
    }
    
    pthread_rwlock_unlock(&cache->lock);
    return BUCKETS_OK;
}
```

---

## 6. Error Handling

### Disk Failure Scenarios

| Scenario | Detection | Recovery |
|----------|-----------|----------|
| **Single disk offline** | Health check fails | Heal from other disks in set |
| **Below read quorum** | Can't achieve N/2 reads | Return error, wait for disks |
| **Below write quorum** | Can't achieve N/2+1 writes | Return error, don't commit |
| **Format mismatch** | Deployment ID differs | Block startup, manual fix |
| **Set topology mismatch** | Sets array differs | Block startup, manual fix |

### State Corruption Recovery

**Format Corruption**:
```c
int buckets_format_heal(buckets_storage_t **disks, int disk_count)
{
    // 1. Load formats from all disks
    buckets_format_t **formats = load_all_formats(disks, disk_count);
    
    // 2. Find quorum format
    buckets_format_t *quorum_format = find_quorum_format(formats, disk_count);
    
    // 3. Repair corrupted disks
    for (int i = 0; i < disk_count; i++) {
        if (!formats_equal(formats[i], quorum_format)) {
            buckets_info("Healing disk %d with quorum format", i);
            buckets_format_save(disks[i], quorum_format);
        }
    }
    
    return BUCKETS_OK;
}
```

---

## 7. Implementation Priority

### Phase 1: Basic State Management (Week 1-2)
- [ ] `buckets_format_t` structure and JSON serialization
- [ ] `buckets_format_load()` and `buckets_format_save()`
- [ ] `buckets_endpoint_t` and endpoint resolution
- [ ] Atomic file write/read utilities

### Phase 2: Cluster Initialization (Week 3-4)
- [ ] `buckets_format_create_new()` - Fresh cluster init
- [ ] `buckets_format_validate()` - Consistency checking
- [ ] Quorum read/write functions
- [ ] Format healing logic

### Phase 3: Topology Management (Week 5-6)
- [ ] `buckets_cluster_topology_t` structure
- [ ] `buckets_topology_load()` and `buckets_topology_save()`
- [ ] `buckets_topology_from_format()` - Bootstrap from format.json
- [ ] Generation-based consistency

### Phase 4: Server Pool State (Week 7-8)
- [ ] `buckets_server_pools_t` structure
- [ ] Pool initialization and lifecycle
- [ ] Pool metadata persistence
- [ ] Integration with erasure sets

---

## 8. Testing Strategy

### Unit Tests
- Format serialization/deserialization
- Atomic write operations
- Quorum read/write logic
- Format validation rules

### Integration Tests
- Multi-disk format initialization
- Format healing with disk failures
- Topology generation changes
- Cross-node synchronization

### Failure Tests
- Disk failures during format write
- Network partitions during sync
- Corrupted format.json recovery
- Deployment ID mismatch handling

---

## References

- MinIO `format-erasure.go` - Format structure and persistence
- MinIO `erasure-server-pool.go` - Pool management
- MinIO `endpoint.go` - Endpoint resolution
- `SCALE_AND_DATA_PLACEMENT.md` - Buckets architecture overview

---

**Status**: Architecture defined, ready for implementation  
**Next**: Implement format.json structures and persistence layer
