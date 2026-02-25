# MinIO Fine-Grained Scalability Architecture

## Status: PROPOSED
**Version:** 1.0  
**Date:** February 25, 2026  
**Authors:** Architecture Team

---

## Executive Summary

This document describes a comprehensive architectural redesign to enable **fine-grained scalability** in MinIO, allowing operators to add or remove individual nodes (1-2 at a time) rather than requiring full server pool operations. This addresses a critical operational limitation in the current architecture where dynamic node management is not supported.

### Decision Summary

After extensive analysis of the current MinIO data placement algorithm and evaluation of multiple alternatives, we have decided to implement a **hybrid architecture combining Location Registry with Consistent Hashing**.

**Key Design Choices:**
- **Location Registry**: Self-hosted on MinIO to track object locations explicitly
- **Consistent Hashing**: Virtual node ring for deterministic placement computation
- **Controlled Migration**: Background process to rebalance objects when topology changes
- **Fresh Start Acceptable**: No backward compatibility requirement with existing clusters
- **Read Latency Priority**: Optimize for fast object lookup (GET operations)

**Expected Outcomes:**
- Add/remove 1-2 nodes with ~20-30% data migration
- Sub-5ms read latency (vs current 10-50ms multi-pool fan-out)
- Zero-downtime node operations
- Migration completes in ~1 hour for 2TB affected data

---

## Table of Contents

1. [Current Architecture Analysis](#1-current-architecture-analysis)
2. [Problem Statement](#2-problem-statement)
3. [Design Requirements](#3-design-requirements)
4. [Evaluated Alternatives](#4-evaluated-alternatives)
5. [Selected Architecture](#5-selected-architecture)
6. [Detailed Component Design](#6-detailed-component-design)
7. [Data Structures](#7-data-structures)
8. [Core Operations](#8-core-operations)
9. [Migration Strategy](#9-migration-strategy)
10. [Fault Tolerance](#10-fault-tolerance)
11. [Performance Analysis](#11-performance-analysis)
12. [Implementation Plan](#12-implementation-plan)
13. [Risk Assessment](#13-risk-assessment)
14. [Open Questions](#14-open-questions)

---

## 1. Current Architecture Analysis

### 1.1 Existing Data Placement Algorithm

MinIO currently uses a **deterministic modulo-based hash** for object placement:

```go
// Current implementation in cmd/erasure-sets.go:660-699
func sipHashMod(key string, cardinality int, id [16]byte) int {
    k0, k1 := binary.LittleEndian.Uint64(id[0:8]), 
              binary.LittleEndian.Uint64(id[8:16])
    sum64 := siphash.Hash(k0, k1, []byte(key))
    return int(sum64 % uint64(cardinality))
}

// Object placement
setIndex = hash(objectName, deploymentID) % numberOfSets
```

**Key Characteristics:**
- **O(1) lookup**: Direct computation, no metadata lookup required
- **Deterministic**: Same object always maps to same set (given fixed topology)
- **Stateless**: No placement metadata to persist or synchronize
- **Immutable dependency**: Requires `numberOfSets` to remain constant

### 1.2 Pool-Based Scaling Model

Current scaling strategy:

```
Initial Cluster:
Pool 0: node{1...4}/data{1...4}  (16 disks, 4 sets of 4 drives)

Expansion (supported):
Pool 0: node{1...4}/data{1...4}
Pool 1: node{5...8}/data{1...4}  ← Add entire new pool

Node Failure (not supported):
Pool 0: node{1,2,4}/data{1...4}  ← Cannot remove node3
```

**Limitations:**
- ❌ Cannot add individual nodes within a pool
- ❌ Cannot remove failed nodes without decommissioning entire pool
- ❌ Pool expansion requires adding 4+ nodes simultaneously
- ❌ Multi-pool reads require fan-out to all pools (O(N) latency)

### 1.3 Why Current Design Exists

From code analysis (`cmd/erasure-sets.go:49`):
```go
// NOTE: There is no dynamic scaling allowed or intended in current design.
```

**Design Rationale:**
1. **Simplicity**: Pure function, no distributed state management
2. **Performance**: O(1) lookup vs O(log N) for alternatives
3. **Consistency**: No risk of placement metadata divergence
4. **Operational Reliability**: No external dependencies (etcd, ZooKeeper, etc.)

**Trade-off:** Operational flexibility sacrificed for architectural simplicity.

---

## 2. Problem Statement

### 2.1 User Requirements

Operators need to:
1. **Add individual nodes** (1-2 at a time) to existing clusters
2. **Remove failed nodes** gracefully without full pool decommission
3. **Scale incrementally** based on capacity needs
4. **Maintain high availability** during topology changes

### 2.2 Current Limitations

The modulo-based hash breaks when topology changes:

```
Before: hash("file.jpg") % 4 = 2  → Set 2 ✓
After:  hash("file.jpg") % 5 = 3  → Set 3 ✗ (object unreachable!)
```

**Fundamental Conflict:**
- Placement algorithm requires immutable `numberOfSets`
- Scalability requires mutable cluster topology
- **Cannot satisfy both with current architecture**

### 2.3 Business Impact

**Without fine-grained scaling:**
- Over-provisioning costs (must add 4+ nodes at once)
- Manual workarounds for node failures (replace hardware vs drain pool)
- Reduced operational agility
- Competitive disadvantage vs cloud-native storage systems

---

## 3. Design Requirements

### 3.1 Functional Requirements

| ID | Requirement | Priority |
|----|-------------|----------|
| FR-1 | Add individual nodes (1-2) to existing pool | **CRITICAL** |
| FR-2 | Remove individual nodes gracefully | **CRITICAL** |
| FR-3 | Zero data loss during topology changes | **CRITICAL** |
| FR-4 | Objects remain accessible during migration | **CRITICAL** |
| FR-5 | Support adding entire erasure sets (4-8 nodes) | HIGH |
| FR-6 | Migration pause/resume capability | MEDIUM |
| FR-7 | Rollback failed topology changes | MEDIUM |

### 3.2 Non-Functional Requirements

| ID | Requirement | Target | Priority |
|----|-------------|--------|----------|
| NFR-1 | Read latency (GET) | < 5ms | **CRITICAL** |
| NFR-2 | Write throughput degradation | < 10% | HIGH |
| NFR-3 | Data migration percentage per node add/remove | 20-30% | HIGH |
| NFR-4 | Migration time for 2TB affected data | ~1 hour | MEDIUM |
| NFR-5 | Cluster startup time | < 2 minutes | MEDIUM |
| NFR-6 | Memory overhead for metadata | < 100GB for 1B objects | MEDIUM |

### 3.3 Operational Requirements

- **Zero-downtime operations**: No cluster restart required for node changes
- **Observability**: Clear metrics and status for migrations
- **Backward compatibility**: NOT required (fresh start acceptable)
- **Deployment model**: Must work in Kubernetes and bare-metal

---

## 4. Evaluated Alternatives

### 4.1 Option Summary

| Approach | Migration % | Read Latency | Complexity | Verdict |
|----------|-------------|--------------|------------|---------|
| **Consistent Hashing** | ~20% | O(log N) ~20ns | Moderate | ✅ **Selected** |
| Rendezvous Hashing (HRW) | ~20% | O(N) ~5µs | Low | ❌ Too slow for 100+ sets |
| CRUSH Algorithm | ~15% | ~10-50µs | Very High | ❌ Overkill complexity |
| **Location Registry** | N/A | O(1) ~1-5ms | Moderate | ✅ **Selected** |
| Hybrid (Computed + Lookup) | Variable | ~100ns-5ms | High | ⚠️ Fallback option |
| Sub-Set Sharding | 50% | O(1) | Low | ❌ High migration cost |
| Append-Only Migration Log | Variable | O(1) + index | Low | ⚠️ Journal grows unbounded |

### 4.2 Why Consistent Hashing + Location Registry?

**Consistent Hashing** provides:
- Minimal data movement (~1/N objects affected per topology change)
- Deterministic placement computation
- Industry-proven algorithm (Cassandra, DynamoDB, Riak)

**Location Registry** provides:
- Optimal read performance (direct lookup, no fan-out)
- Flexibility to override computed placement
- Explicit audit trail of object locations

**Synergy:**
- Registry eliminates multi-pool fan-out (current performance issue)
- Consistent hashing minimizes migration overhead
- Combined approach balances performance and flexibility

### 4.3 Rejected Alternatives

**Rendezvous Hashing:**
- Requires computing hash for ALL sets per object lookup
- 100 sets × 50ns = 5µs per lookup (250× slower than modulo)
- Doesn't scale to large set counts

**CRUSH Algorithm:**
- ~3000 lines of complex placement logic
- Requires detailed topology hierarchy (rack/host/disk)
- Operational complexity incompatible with MinIO's simplicity philosophy
- Better suited for hyperscale (Ceph's use case)

**Pure Registry (No Hashing):**
- Single point of failure for all reads
- Registry unavailability = cluster unavailability
- No fallback if registry corrupted

---

## 5. Selected Architecture

### 5.1 High-Level Design

```
┌───────────────────────────────────────────────────────────┐
│                    MinIO Cluster                           │
├───────────────────────────────────────────────────────────┤
│                                                            │
│  ┌──────────────────────────────────────────────┐         │
│  │      Location Registry (Self-Hosted)         │         │
│  │   - Bucket: .minio-registry                  │         │
│  │   - Storage: MinIO erasure coding            │         │
│  │   - Cache: LRU 1M entries                    │         │
│  │   - Key: bucket/object/version-id.json       │         │
│  │   - Value: {pool, set, disks, generation}    │         │
│  └──────────────────────────────────────────────┘         │
│                      ▲                                     │
│                      │ Lookup/Record                       │
│  ┌───────────────────┴──────────────────────────┐         │
│  │         Placement Manager                    │         │
│  │  ┌────────────────────────────────────┐      │         │
│  │  │   Consistent Hash Ring             │      │         │
│  │  │   - Virtual nodes: 150 per set     │      │         │
│  │  │   - Algorithm: SipHash + binary    │      │         │
│  │  │   - Sorted ring for O(log N) lookup│      │         │
│  │  └────────────────────────────────────┘      │         │
│  │  ┌────────────────────────────────────┐      │         │
│  │  │   Topology Manager                 │      │         │
│  │  │   - Current generation             │      │         │
│  │  │   - Pool/set/disk inventory        │      │         │
│  │  │   - Set states (active/drain/removed)│    │         │
│  │  └────────────────────────────────────┘      │         │
│  │  ┌────────────────────────────────────┐      │         │
│  │  │   Migration Orchestrator           │      │         │
│  │  │   - Affected object scanner        │      │         │
│  │  │   - Migration workers (16 threads) │      │         │
│  │  │   - Progress tracker               │      │         │
│  │  └────────────────────────────────────┘      │         │
│  └───────────────────────────────────────────────┘         │
│                      │                                     │
│         ┌────────────┼────────────┐                        │
│         ▼            ▼            ▼                         │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐                   │
│  │  Pool 0  │ │  Pool 1  │ │  Pool 2  │                   │
│  │  Sets... │ │  Sets... │ │  Sets... │                   │
│  │  Disks...│ │  Disks...│ │  Disks...│                   │
│  └──────────┘ └──────────┘ └──────────┘                   │
└───────────────────────────────────────────────────────────┘
```

### 5.2 Core Principles

1. **Self-Hosted Registry**: No external dependencies
2. **Graceful Degradation**: Fall back to computed placement if registry unavailable
3. **Atomic Operations**: Object write + registry record = single transaction
4. **Eventual Consistency**: Migration runs asynchronously, objects accessible from old/new locations
5. **Generation Tracking**: Topology versioning prevents stale reads

### 5.3 Bootstrap Problem Solution

**Circular Dependency:**
- Registry stores object locations
- Registry itself is objects stored on MinIO
- How do we locate the registry?

**Solution:**
```go
// Well-known registry bucket on fixed location
const RegistryBucket = ".minio-registry"
const RegistryPool = 0
const RegistrySet = 0

// Registry bucket uses SIMPLE MODULO placement (never migrated)
// All other objects use registry-based placement
```

**Key Insight:** The registry bucket is a **special case** with hardcoded placement, breaking the circular dependency.

---

## 6. Detailed Component Design

### 6.1 Location Registry

#### Purpose
Track the physical location (pool, set, disks) of every object version in the cluster.

#### Storage Backend
**Self-hosted on MinIO** (`.minio-registry` bucket):
- Leverages existing erasure coding for durability
- No external dependencies
- Same SLA as data objects

#### Registry Schema

**Key Format:**
```
{bucket}/{object}/{version-id}.json
```

**Value Format:**
```json
{
  "bucket": "mybucket",
  "object": "photos/vacation.jpg",
  "version_id": "3fa9b0a8-2c8e-4c3a-8f3e-5c9d0e1f2a3b",
  "pool_idx": 1,
  "set_idx": 3,
  "disk_idxs": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
  "generation": 42,
  "mod_time": "2026-02-25T10:30:00Z",
  "size": 2048576
}
```

**Storage Overhead:**
- ~150 bytes per object version (JSON)
- 1 billion objects = 150GB registry data
- Compressed with gzip: ~50GB

#### Caching Strategy

```go
type RegistryCache struct {
    lru      *LRUCache     // 1M entries, ~200MB RAM
    ttl      time.Duration // 5 minutes
    hitRate  *metrics.Counter
}

// Cache key: "bucket/object/version-id"
// Cache invalidation: TTL + explicit broadcast on updates
```

**Expected Cache Hit Rate:** 99%+ for hot objects

#### API

```go
type LocationRegistry interface {
    // Record object location atomically
    Record(ctx context.Context, loc ObjectLocation) error
    
    // Lookup object location (cache-aware)
    Lookup(ctx context.Context, bucket, object, versionID string) (*ObjectLocation, error)
    
    // Update location during migration
    Update(ctx context.Context, bucket, object, versionID string, 
           updateFn func(*ObjectLocation)) error
    
    // Delete location record
    Delete(ctx context.Context, bucket, object, versionID string) error
    
    // Batch operations for efficiency
    RecordBatch(ctx context.Context, locations []ObjectLocation) error
    LookupBatch(ctx context.Context, keys []RegistryKey) ([]ObjectLocation, error)
}
```

### 6.2 Consistent Hash Ring

#### Purpose
Compute deterministic object placement that minimizes data movement when topology changes.

#### Algorithm

**Virtual Nodes:**
Each erasure set is represented by multiple virtual nodes (vnodes) on the hash ring.

```go
vnodeFactor = 150  // Virtual nodes per set

for each set S:
    for i = 0 to vnodeFactor-1:
        vnodeKey = "pool-{pool}-set-{set}-vnode-{i}"
        vnodeHash = sipHash(vnodeKey, deploymentID)
        ring.Add(vnodeHash, S)

ring.Sort()  // Sort by hash value for binary search
```

**Object Placement:**
```go
func ComputeSet(bucket, object string) (poolIdx, setIdx int) {
    objectKey = "{bucket}/{object}"
    objectHash = sipHash(objectKey, deploymentID)
    
    // Binary search for next vnode >= objectHash
    vnodeIdx = binarySearch(ring, objectHash)
    if vnodeIdx == len(ring):
        vnodeIdx = 0  // Wrap around
    
    return ring[vnodeIdx].poolIdx, ring[vnodeIdx].setIdx
}
```

**Complexity:** O(log(N × vnodeFactor)) = O(log(100 × 150)) = ~14 comparisons

#### Data Structure

```go
type ConsistentHashRing struct {
    vnodes       []VNode    // Sorted by hash
    vnodeFactor  int        // 150
    deploymentID [16]byte
    generation   int64      // Topology version
}

type VNode struct {
    hash    uint64
    poolIdx int
    setIdx  int
}
```

#### Ring Rebuilding

When topology changes (add/remove set):
1. Compute new vnode list
2. Sort by hash
3. Atomic swap with old ring
4. Broadcast to all peers

**Rebuild Time:** ~1ms for 100 sets × 150 vnodes

### 6.3 Topology Manager

#### Purpose
Track cluster topology and coordinate topology changes.

#### Topology Schema

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
              "uuid": "a1b2c3d4-...",
              "capacity": 10995116277760
            }
          ]
        }
      ]
    }
  ]
}
```

**Persistence:**
- Location: `.minio.sys/topology.json` on each disk
- Write quorum: N/2 + 1 disks
- Read quorum: N/2 disks

#### Set States

```go
type SetState int

const (
    SetActive    SetState = iota  // Accepting new writes
    SetDraining                   // No new writes, migrating out
    SetRemoved                    // Fully drained, excluded from ring
)
```

#### API

```go
type TopologyManager interface {
    // Get current topology
    GetTopology() *ClusterTopology
    
    // Add new set to pool
    AddSet(ctx context.Context, poolIdx int, disks []DiskInfo) error
    
    // Mark set for draining
    DrainSet(ctx context.Context, poolIdx, setIdx int) error
    
    // Remove drained set
    RemoveSet(ctx context.Context, poolIdx, setIdx int) error
    
    // Reload topology from disk
    ReloadTopology(ctx context.Context) error
}
```

### 6.4 Migration Orchestrator

#### Purpose
Move objects between sets when topology changes to converge to computed placement.

#### Migration Phases

**Phase 1: Identification**
```go
// Compare old and new hash rings
for each object:
    oldSet = oldRing.ComputeSet(object)
    newSet = newRing.ComputeSet(object)
    
    if oldSet != newSet:
        migrationQueue.Add(object, oldSet, newSet)
```

**Phase 2: Migration**
```go
for each object in migrationQueue:
    // 1. Read from source set
    data, metadata = ReadObject(object, oldSet)
    
    // 2. Write to destination set
    WriteObject(object, data, metadata, newSet)
    
    // 3. Update registry atomically
    registry.Update(object, func(loc) {
        loc.poolIdx = newPoolIdx
        loc.setIdx = newSetIdx
        loc.generation = currentGeneration
    })
    
    // 4. Delete from source
    DeleteObject(object, oldSet)
    
    // 5. Update progress
    migrationTracker.IncrementCompleted()
```

**Phase 3: Verification**
```go
// Periodic scrubber validates registry matches actual locations
for each registry entry:
    actualLocation = ScanDisks(entry.bucket, entry.object)
    if actualLocation != entry.location:
        LogInconsistency(entry)
        FixInconsistency(entry)
```

#### Parallelism

```go
type MigrationOrchestrator struct {
    workerCount int           // Default: 16
    queueDepth  int           // Default: 1000
    throttle    rate.Limiter  // Default: 100 MB/s per worker
}
```

**Concurrency Control:**
- Per-object locking (prevent concurrent migration)
- Bandwidth throttling (prevent I/O saturation)
- Priority lanes (user traffic > migration traffic)

#### Progress Tracking

```json
{
  "migration_id": "migration-gen-42-to-43",
  "start_time": "2026-02-25T10:00:00Z",
  "source_generation": 42,
  "target_generation": 43,
  "total_objects": 10000000,
  "migrated_objects": 2500000,
  "failed_objects": 42,
  "estimated_completion": "2026-02-25T11:30:00Z",
  "bytes_migrated": 2048000000000,
  "bytes_total": 8192000000000
}
```

**Persistence:** `.minio.sys/migration-{id}.json`

---

## 7. Data Structures

### 7.1 Core Types

```go
// ObjectLocation - Registry entry
type ObjectLocation struct {
    Bucket     string    `json:"bucket"`
    Object     string    `json:"object"`
    VersionID  string    `json:"version_id"`
    PoolIdx    int       `json:"pool_idx"`
    SetIdx     int       `json:"set_idx"`
    DiskIdxs   []int     `json:"disk_idxs"`
    Generation int64     `json:"generation"`
    ModTime    time.Time `json:"mod_time"`
    Size       int64     `json:"size"`
}

// ClusterTopology - Cluster structure
type ClusterTopology struct {
    Version      int            `json:"version"`
    Generation   int64          `json:"generation"`
    DeploymentID string         `json:"deployment_id"`
    VNodeFactor  int            `json:"vnode_factor"`
    Pools        []PoolTopology `json:"pools"`
}

// PoolTopology - Pool within cluster
type PoolTopology struct {
    Idx  int           `json:"idx"`
    Sets []SetTopology `json:"sets"`
}

// SetTopology - Erasure set
type SetTopology struct {
    Idx   int        `json:"idx"`
    State SetState   `json:"state"`
    Disks []DiskInfo `json:"disks"`
}

// DiskInfo - Individual disk
type DiskInfo struct {
    Endpoint string `json:"endpoint"`
    UUID     string `json:"uuid"`
    Capacity uint64 `json:"capacity"`
}

// ConsistentHashRing - Placement ring
type ConsistentHashRing struct {
    vnodes       []VNode
    vnodeFactor  int
    deploymentID [16]byte
    generation   int64
    mu           sync.RWMutex
}

// VNode - Virtual node on ring
type VNode struct {
    hash    uint64
    poolIdx int
    setIdx  int
}

// MigrationState - Migration progress
type MigrationState struct {
    ID                  string    `json:"migration_id"`
    StartTime           time.Time `json:"start_time"`
    SourceGeneration    int64     `json:"source_generation"`
    TargetGeneration    int64     `json:"target_generation"`
    TotalObjects        int64     `json:"total_objects"`
    MigratedObjects     int64     `json:"migrated_objects"`
    FailedObjects       int64     `json:"failed_objects"`
    BytesMigrated       int64     `json:"bytes_migrated"`
    BytesTotal          int64     `json:"bytes_total"`
    EstimatedCompletion time.Time `json:"estimated_completion"`
}
```

### 7.2 File Locations

| Component | File Path | Format | Replication |
|-----------|-----------|--------|-------------|
| Topology | `.minio.sys/topology.json` | JSON | Quorum write to all disks |
| Registry Entries | `.minio-registry/{bucket}/{object}/{version}.json` | JSON | Erasure coded |
| Migration State | `.minio.sys/migration-{id}.json` | JSON | Quorum write |
| Hash Ring | In-memory, rebuilt from topology | N/A | N/A |

---

## 8. Core Operations

### 8.1 Object Write (PUT)

```go
func PutObject(ctx context.Context, bucket, object string, 
               data io.Reader, opts ObjectOptions) (ObjectInfo, error) {
    
    // 1. Compute placement using hash ring
    poolIdx, setIdx := globalHashRing.ComputeSet(bucket, object)
    
    // 2. Acquire per-object write lock
    lockKey := fmt.Sprintf("%s/%s", bucket, object)
    unlock := acquireWriteLock(ctx, lockKey)
    defer unlock()
    
    // 3. Write object to computed location
    pool := globalServerPools[poolIdx]
    set := pool.sets[setIdx]
    objInfo, err := set.PutObject(ctx, bucket, object, data, opts)
    if err != nil {
        return ObjectInfo{}, fmt.Errorf("write failed: %w", err)
    }
    
    // 4. Record location in registry
    location := ObjectLocation{
        Bucket:     bucket,
        Object:     object,
        VersionID:  objInfo.VersionID,
        PoolIdx:    poolIdx,
        SetIdx:     setIdx,
        DiskIdxs:   set.GetDiskIndices(),
        Generation: globalHashRing.generation,
        ModTime:    objInfo.ModTime,
        Size:       objInfo.Size,
    }
    
    err = globalLocationRegistry.Record(ctx, location)
    if err != nil {
        // CRITICAL: Registry write failed
        // Object is orphaned - must rollback
        set.DeleteObject(ctx, bucket, object, opts)
        return ObjectInfo{}, fmt.Errorf("registry write failed: %w", err)
    }
    
    // 5. Update cache
    globalRegistryCache.Add(cacheKey(location), location)
    
    return objInfo, nil
}
```

**Atomicity Guarantee:**
- Object write + registry record = pseudo-transaction
- On registry failure, object is deleted (rollback)
- No orphaned objects

### 8.2 Object Read (GET)

```go
func GetObject(ctx context.Context, bucket, object string, 
               opts ObjectOptions) (*GetObjectReader, error) {
    
    // 1. Lookup location in registry (cache-aware)
    location, err := globalLocationRegistry.Lookup(ctx, bucket, object, opts.VersionID)
    
    if err == nil {
        // Registry hit - direct fetch
        pool := globalServerPools[location.PoolIdx]
        set := pool.sets[location.SetIdx]
        return set.GetObject(ctx, bucket, object, opts)
    }
    
    // 2. Registry miss - check if migration in progress
    if globalMigrationOrchestrator.InProgress() {
        // Fall back to computed location + source location
        return getObjectWithMigrationFallback(ctx, bucket, object, opts)
    }
    
    // 3. Critical error - registry unavailable
    return nil, fmt.Errorf("registry lookup failed: %w", err)
}

func getObjectWithMigrationFallback(ctx context.Context, bucket, object string, 
                                    opts ObjectOptions) (*GetObjectReader, error) {
    // Try new location (current generation)
    newPool, newSet := globalHashRing.ComputeSet(bucket, object)
    reader, err := globalServerPools[newPool].sets[newSet].GetObject(ctx, bucket, object, opts)
    if err == nil {
        return reader, nil
    }
    
    // Try old location (previous generation)
    oldRing := getHashRingForGeneration(globalHashRing.generation - 1)
    oldPool, oldSet := oldRing.ComputeSet(bucket, object)
    return globalServerPools[oldPool].sets[oldSet].GetObject(ctx, bucket, object, opts)
}
```

**Performance:**
- Cache hit (99%): ~100ns (in-memory)
- Cache miss: ~1-5ms (registry GET)
- Registry unavailable: Fallback to dual-location check

### 8.3 Add Node

```bash
mc admin cluster add-node ALIAS http://node5:9000/data{1...4} --pool 0
```

```go
func AddNode(ctx context.Context, nodeURL string, poolIdx int, 
             diskPaths []string) error {
    
    // 1. Validate node reachability
    if err := validateNodeConnectivity(nodeURL); err != nil {
        return fmt.Errorf("node unreachable: %w", err)
    }
    
    // 2. Validate disk configuration
    diskCount := len(diskPaths)
    expectedDiskCount := getExpectedDiskCountForPool(poolIdx)
    if diskCount != expectedDiskCount {
        return fmt.Errorf("disk count mismatch: got %d, expected %d", 
                          diskCount, expectedDiskCount)
    }
    
    // 3. Create new set topology
    newSet := SetTopology{
        Idx:   getNextSetIndex(poolIdx),
        State: SetActive,
        Disks: createDiskInfoFromPaths(nodeURL, diskPaths),
    }
    
    // 4. Update topology with new generation
    oldTopology := globalTopologyManager.GetTopology()
    newTopology := oldTopology.Clone()
    newTopology.Pools[poolIdx].Sets = append(newTopology.Pools[poolIdx].Sets, newSet)
    newTopology.Generation++
    
    // 5. Persist new topology (quorum write)
    if err := globalTopologyManager.SaveTopology(ctx, newTopology); err != nil {
        return fmt.Errorf("topology save failed: %w", err)
    }
    
    // 6. Rebuild hash ring
    newHashRing := NewConsistentHashRing(newTopology, vnodeFactor)
    globalHashRing.AtomicSwap(newHashRing)
    
    // 7. Broadcast topology update to all peers
    globalNotificationSys.ReloadTopology(ctx, newTopology)
    
    // 8. Initialize new disks with format.json
    if err := initializeNewSet(ctx, poolIdx, newSet); err != nil {
        // Rollback topology
        globalTopologyManager.SaveTopology(ctx, oldTopology)
        return fmt.Errorf("disk initialization failed: %w", err)
    }
    
    // 9. Start background migration
    migrationID := fmt.Sprintf("migration-gen-%d-to-%d", 
                               oldTopology.Generation, newTopology.Generation)
    go globalMigrationOrchestrator.Start(ctx, migrationID, 
                                         oldTopology.Generation, 
                                         newTopology.Generation)
    
    return nil
}
```

**Duration:** ~30 seconds (excluding migration)

### 8.4 Remove Node

```bash
mc admin cluster remove-node ALIAS http://node3:9000 --drain
```

```go
func RemoveNode(ctx context.Context, nodeURL string, drain bool) error {
    
    // 1. Find set containing this node
    poolIdx, setIdx, err := findSetByNodeURL(nodeURL)
    if err != nil {
        return fmt.Errorf("node not found: %w", err)
    }
    
    // 2. Mark set as draining
    topology := globalTopologyManager.GetTopology()
    topology.Pools[poolIdx].Sets[setIdx].State = SetDraining
    topology.Generation++
    
    // 3. Persist updated topology
    if err := globalTopologyManager.SaveTopology(ctx, topology); err != nil {
        return fmt.Errorf("topology save failed: %w", err)
    }
    
    // 4. Rebuild hash ring (draining sets excluded)
    newHashRing := NewConsistentHashRing(topology, vnodeFactor)
    globalHashRing.AtomicSwap(newHashRing)
    
    // 5. Broadcast topology update
    globalNotificationSys.ReloadTopology(ctx, topology)
    
    // 6. Drain objects from set
    if drain {
        if err := drainSet(ctx, poolIdx, setIdx); err != nil {
            return fmt.Errorf("drain failed: %w", err)
        }
    }
    
    // 7. Mark set as removed
    topology = globalTopologyManager.GetTopology()
    topology.Pools[poolIdx].Sets[setIdx].State = SetRemoved
    topology.Generation++
    globalTopologyManager.SaveTopology(ctx, topology)
    
    // 8. Close connections to node
    globalGrid.RemovePeer(ctx, nodeURL)
    globalNotificationSys.RemovePeer(ctx, nodeURL)
    
    return nil
}

func drainSet(ctx context.Context, poolIdx, setIdx int) error {
    // Scan all objects in the draining set
    scanner := NewSetScanner(poolIdx, setIdx)
    
    for object := range scanner.Scan(ctx) {
        // Compute new location (draining set excluded from ring)
        newPoolIdx, newSetIdx := globalHashRing.ComputeSet(object.Bucket, object.Name)
        
        // Migrate object
        if err := migrateObject(ctx, object, poolIdx, setIdx, newPoolIdx, newSetIdx); err != nil {
            return fmt.Errorf("migration failed for %s: %w", object.Name, err)
        }
    }
    
    return nil
}
```

**Duration:** ~30 seconds + drain time (~1 hour for 2TB)

---

## 9. Migration Strategy

### 9.1 Migration Efficiency

**Consistent Hashing Property:**
Adding set N+1 to N existing sets affects ~1/(N+1) of objects.

**Examples:**
- 4 sets → 5 sets: 20% of objects migrate
- 10 sets → 11 sets: 9% of objects migrate
- 20 sets → 21 sets: 5% of objects migrate

**Formula:**
```
Migration % ≈ 1 / (N + 1) × 100
```

Where N = number of sets before addition.

### 9.2 Migration Timeline

**Assumptions:**
- 10 million objects
- 20% need migration = 2 million objects
- Average object size: 1MB
- Network bandwidth: 10 Gbps
- Migration workers: 16 parallel threads

**Calculation:**
```
Data to migrate: 2M objects × 1MB = 2TB
Network throughput: 10 Gbps = 1.25 GB/s
Theoretical time: 2TB / 1.25 GB/s = 1600 seconds = 27 minutes

With overhead (read, write, registry update, verification): 
Real-world time: ~1 hour
```

### 9.3 Migration Phases

**Phase 1: Planning (1 minute)**
1. Compare old and new hash rings
2. Identify affected objects (scan registry or compute)
3. Create migration queue
4. Estimate completion time

**Phase 2: Migration (50-60 minutes)**
1. Process queue with 16 parallel workers
2. Each worker:
   - Read object from source set
   - Write object to destination set
   - Update registry atomically
   - Verify checksums
   - Delete from source
3. Track progress in `.minio.sys/migration-{id}.json`

**Phase 3: Verification (5 minutes)**
1. Scan destination sets for migrated objects
2. Verify registry entries match actual locations
3. Verify no objects left in source (if draining)
4. Mark migration complete

### 9.4 Migration Interruption Handling

**Scenario: Cluster restarts during migration**

```go
func (m *MigrationOrchestrator) RecoverFromInterruption(ctx context.Context) error {
    // 1. Load migration state from disk
    state, err := loadMigrationState()
    if err != nil {
        return err
    }
    
    // 2. Verify source and target topologies match
    if state.TargetGeneration != globalHashRing.generation {
        return fmt.Errorf("topology changed during migration")
    }
    
    // 3. Resume from last checkpoint
    m.migrationQueue = rebuildQueueFromState(state)
    
    // 4. Continue migration
    return m.processMigrationQueue(ctx)
}
```

**Checkpointing:**
- Every 1000 objects migrated
- Every 5 minutes
- Before shutdown (graceful)

### 9.5 Concurrent User Traffic

**Migration does NOT block user operations:**

**READ Path:**
```go
// Object may be in old location, new location, or both
// Always check registry first
// During migration, fall back to dual-location check
```

**WRITE Path:**
```go
// New writes always go to new location (current generation)
// Migration only touches objects created before topology change
```

**DELETE Path:**
```go
// Delete from both old and new locations (idempotent)
// Update registry to mark deleted
```

---

## 10. Fault Tolerance

### 10.1 Registry Unavailability

**Scenario:** Registry bucket unreachable (disk failures exceed quorum)

**Mitigation:**
```go
func GetObjectWithRegistryFallback(ctx context.Context, bucket, object string) (*GetObjectReader, error) {
    // 1. Try registry lookup
    location, err := globalLocationRegistry.Lookup(ctx, bucket, object, "")
    if err == nil {
        return fetchDirect(location)
    }
    
    // 2. Fall back to computed location
    poolIdx, setIdx := globalHashRing.ComputeSet(bucket, object)
    reader, err := globalServerPools[poolIdx].sets[setIdx].GetObject(ctx, bucket, object, opts)
    if err == nil {
        return reader, nil
    }
    
    // 3. Last resort: fan-out to all pools (degraded mode)
    return fanOutToAllPools(ctx, bucket, object)
}
```

**Impact:**
- Reads: Degraded performance (fall back to fan-out)
- Writes: BLOCKED (cannot record location)

**Recovery:**
- Heal registry bucket disks
- Rebuild registry from object scan (background process)

### 10.2 Registry-Data Inconsistency

**Scenario:** Registry says object at Set A, but actually at Set B

**Detection:**
```go
// Background scrubber (runs weekly)
func RegistryScrubber(ctx context.Context) {
    for entry := range globalLocationRegistry.ScanAll(ctx) {
        // Verify object exists at recorded location
        exists := checkObjectExists(entry.PoolIdx, entry.SetIdx, entry.Bucket, entry.Object)
        
        if !exists {
            // Object not at recorded location - search all sets
            actualLocation := findObjectInCluster(entry.Bucket, entry.Object)
            
            if actualLocation != nil {
                // Fix registry
                globalLocationRegistry.Update(entry.Bucket, entry.Object, entry.VersionID, func(loc *ObjectLocation) {
                    loc.PoolIdx = actualLocation.PoolIdx
                    loc.SetIdx = actualLocation.SetIdx
                })
            } else {
                // Object truly missing - log corruption
                logger.Error("Object referenced in registry but missing from cluster", entry)
            }
        }
    }
}
```

**Prevention:**
- Atomic object write + registry record
- Quorum-based registry writes
- Generation numbers to detect stale entries

### 10.3 Split Brain (Network Partition)

**Scenario:** Cluster partitions into two groups, both continue accepting writes

**Current MinIO Behavior:**
- Erasure coding requires write quorum (N/2 + 1 disks)
- Minority partition cannot write (below quorum)
- **No split brain for data**

**Registry Impact:**
- Registry bucket also uses erasure coding
- Same quorum requirements
- **Minority partition cannot record locations**

**Outcome:**
- Minority partition rejects writes (consistent with current behavior)
- No additional split-brain risk from registry

### 10.4 Migration Failures

**Scenario:** Migration worker crashes mid-object-copy

**Recovery:**
```go
type MigrationTransaction struct {
    ObjectKey      string
    SourceLocation Location
    TargetLocation Location
    State          TransactionState  // Pending, Committed, Aborted
}

// On worker restart
func RecoverMigrationTransaction(tx MigrationTransaction) error {
    switch tx.State {
    case Pending:
        // Object may be partially written to target
        // Delete from target, retry migration
        deleteObject(tx.TargetLocation)
        return retryMigration(tx)
        
    case Committed:
        // Registry updated, but source may not be deleted
        // Idempotent cleanup
        deleteObject(tx.SourceLocation)
        return nil
        
    case Aborted:
        // Cleanup partial writes
        deleteObject(tx.TargetLocation)
        return nil
    }
}
```

**Transaction Log:** `.minio.sys/migration-{id}-txlog.bin`

---

## 11. Performance Analysis

### 11.1 Read Latency Breakdown

**Current (Multi-Pool Fan-Out):**
```
Fan-out to 3 pools in parallel:     10-50ms
├─ Network RTT (3× parallel):       3-10ms
├─ Disk seek (per pool):           5-15ms
└─ Erasure decode:                  2-5ms
Total:                              10-50ms
```

**New (Registry Lookup):**
```
Cache hit path (99%):               ~100ns
├─ LRU cache lookup:                100ns
├─ Hash ring compute:               20ns  (not used with cache)
└─ Direct disk read:                0ms   (not yet started)

Cache miss path (1%):               1-5ms
├─ Registry GET:                    1-5ms
│  ├─ Network RTT:                 1-2ms
│  └─ Erasure decode:              1-3ms
├─ Cache update:                    100ns
└─ Direct disk read:                0ms   (not yet started)

Total:                              ~1-5ms (vs 10-50ms)
```

**Improvement:** **5-50× faster reads**

### 11.2 Write Latency Breakdown

**Current:**
```
Object write:                       10-50ms
├─ Erasure encode:                 2-10ms
├─ Network writes (parallel):      5-20ms
└─ Disk writes:                    3-20ms
Total:                              10-50ms
```

**New:**
```
Object write:                       10-50ms  (same as current)
Registry write:                     +1-5ms
├─ JSON marshal:                   0.1ms
├─ Network RTT:                    1-2ms
└─ Erasure encode + write:         1-3ms

Total:                              11-55ms
```

**Degradation:** **~10% slower writes** (acceptable per NFR-2)

### 11.3 Memory Overhead

**Registry Cache:**
```
1M entries × 200 bytes = 200MB per node
```

**Hash Ring:**
```
100 sets × 150 vnodes × 24 bytes = 360KB
```

**Topology:**
```
~100KB (JSON in memory)
```

**Total:** ~200MB per node (acceptable per NFR-6)

### 11.4 Storage Overhead

**Registry Data:**
```
1 billion objects × 150 bytes (JSON) = 150GB
With gzip compression: ~50GB
With erasure coding (N=12, K=8): 50GB × (12/8) = 75GB raw
```

**Percentage of Data:**
```
1B objects × 1MB avg = 1 PB data
Registry: 75GB / 1PB = 0.0075%
```

**Negligible overhead**

### 11.5 Migration Performance

**Network Utilization:**
```
16 workers × 100 MB/s per worker = 1.6 GB/s
With 10 Gbps network = 1.25 GB/s max
Actual: ~1 GB/s (80% utilization)
```

**CPU Utilization:**
```
Erasure encoding/decoding dominates
Per worker: ~1 CPU core
16 workers: ~16 cores (on 32-core nodes = 50%)
```

**Impact on User Traffic:**
- Bandwidth contention: ~20% reduction in available bandwidth
- CPU contention: Minimal (migration can be throttled)
- **Overall degradation: 10-20% during active migration**

---

## 12. Implementation Plan

### 12.1 Phase 1: Foundation (Weeks 1-3)

**Objectives:**
- Implement location registry
- Self-hosted on MinIO
- Basic CRUD operations
- Unit tests

**Deliverables:**

| File | Lines | Description |
|------|-------|-------------|
| `cmd/location-registry.go` | ~800 | Registry implementation |
| `cmd/location-registry-cache.go` | ~300 | LRU cache layer |
| `cmd/location-registry_test.go` | ~600 | Unit tests |
| `cmd/object-api-registry.go` | ~400 | Integrate registry with PUT/GET |

**Key Functions:**
```go
func (r *LocationRegistry) Record(ctx, loc ObjectLocation) error
func (r *LocationRegistry) Lookup(ctx, bucket, object, versionID string) (*ObjectLocation, error)
func (r *LocationRegistry) Update(ctx, bucket, object, versionID string, fn func(*ObjectLocation)) error
func (r *LocationRegistry) Delete(ctx, bucket, object, versionID string) error
```

**Acceptance Criteria:**
- ✅ Registry bucket created on cluster init
- ✅ PUT records location in registry
- ✅ GET uses registry for lookup
- ✅ Cache hit rate >95% in tests
- ✅ Registry unavailable = graceful degradation

### 12.2 Phase 2: Consistent Hashing (Weeks 4-6)

**Objectives:**
- Implement consistent hash ring
- Virtual node management
- Integration with topology

**Deliverables:**

| File | Lines | Description |
|------|-------|-------------|
| `cmd/consistent-hash.go` | ~700 | Hash ring implementation |
| `cmd/topology-manager.go` | ~600 | Topology CRUD |
| `cmd/consistent-hash_test.go` | ~500 | Unit tests |
| `cmd/topology-manager_test.go` | ~400 | Unit tests |

**Key Functions:**
```go
func NewConsistentHashRing(topology *ClusterTopology, vnodeFactor int) *ConsistentHashRing
func (r *ConsistentHashRing) ComputeSet(bucket, object string) (poolIdx, setIdx int)
func (t *TopologyManager) AddSet(ctx, poolIdx int, disks []DiskInfo) error
func (t *TopologyManager) DrainSet(ctx, poolIdx, setIdx int) error
```

**Acceptance Criteria:**
- ✅ Ring rebuilds in <1ms for 100 sets
- ✅ Adding set affects ~1/(N+1) objects (measured)
- ✅ Ring persists across restarts
- ✅ Topology changes broadcast to all peers

### 12.3 Phase 3: Migration Engine (Weeks 7-10)

**Objectives:**
- Object migration orchestrator
- Progress tracking
- Resume capability

**Deliverables:**

| File | Lines | Description |
|------|-------|-------------|
| `cmd/topology-migration.go` | ~1200 | Migration orchestrator |
| `cmd/migration-scanner.go` | ~600 | Affected objects scanner |
| `cmd/migration-worker.go` | ~500 | Migration worker pool |
| `cmd/migration-tracker.go` | ~400 | Progress tracking |
| `cmd/migration_test.go` | ~800 | Integration tests |

**Key Functions:**
```go
func (m *MigrationOrchestrator) Start(ctx, migrationID string, oldGen, newGen int64) error
func (m *MigrationOrchestrator) Pause(ctx, migrationID string) error
func (m *MigrationOrchestrator) Resume(ctx, migrationID string) error
func (m *MigrationOrchestrator) GetStatus(migrationID string) (*MigrationState, error)
func migrateObject(ctx, obj ObjectInfo, srcPool, srcSet, dstPool, dstSet int) error
```

**Acceptance Criteria:**
- ✅ Migration completes with 0 data loss
- ✅ Resume after cluster restart
- ✅ Progress visible via status command
- ✅ User traffic not blocked during migration
- ✅ Migration throughput >500 MB/s

### 12.4 Phase 4: Admin Commands (Weeks 11-12)

**Objectives:**
- CLI commands for node management
- Status and monitoring

**Deliverables:**

| File | Lines | Description |
|------|-------|-------------|
| `madmin-go/cluster-commands.go` | ~600 | Admin API client |
| `cmd/admin-rpc-cluster.go` | ~800 | Server-side handlers |
| `docs/cluster-management.md` | N/A | Documentation |

**Commands:**
```bash
mc admin cluster add-node ALIAS http://node5:9000/data{1...4} --pool 0
mc admin cluster remove-node ALIAS http://node3:9000 --drain
mc admin cluster list-nodes ALIAS
mc admin cluster migration-status ALIAS
mc admin cluster migration-pause ALIAS
mc admin cluster migration-resume ALIAS
```

**Acceptance Criteria:**
- ✅ Add node completes in <1 minute (excluding migration)
- ✅ Remove node drains data before removal
- ✅ Migration status shows real-time progress
- ✅ Errors clearly communicated to operator

### 12.5 Phase 5: Testing & Hardening (Weeks 13-16)

**Objectives:**
- Chaos testing
- Performance benchmarking
- Production readiness

**Test Scenarios:**

| Test | Description | Success Criteria |
|------|-------------|------------------|
| Node failure during migration | Kill node mid-migration | Migration resumes, 0 data loss |
| Registry corruption | Delete registry entries | Scrubber detects and fixes |
| Network partition | Isolate subset of nodes | Minority rejects writes |
| Scale test | 100 nodes, 100M objects | Add node in <2 min, migration <6 hours |
| Performance regression | Compare vs baseline | Read latency <5ms, write degradation <10% |

**Deliverables:**
- Chaos test suite
- Performance benchmarks
- Operations runbook
- Migration guide for existing deployments (if needed)

---

## 13. Risk Assessment

### 13.1 High Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Registry single point of failure** | Medium | Critical | Self-host on MinIO with erasure coding; fall back to fan-out if unavailable |
| **Migration data loss** | Low | Critical | Transaction log; atomic operations; extensive testing |
| **Performance regression** | Medium | High | Aggressive caching; benchmark-driven development; throttling |
| **Registry-data inconsistency** | Medium | High | Background scrubber; atomic updates; generation tracking |

### 13.2 Medium Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Implementation complexity** | High | Medium | Phased rollout; extensive unit tests; code reviews |
| **Migration time exceeds estimates** | Medium | Medium | Throttling controls; pause/resume; parallel workers |
| **Operator confusion** | Medium | Medium | Clear documentation; validation checks; dry-run mode |

### 13.3 Low Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Storage overhead** | Low | Low | Registry is <0.01% of data; can be compressed |
| **Hash collisions** | Very Low | Medium | SipHash-2-4 has negligible collision rate |
| **Bootstrap circular dependency** | Low | Medium | Well-known registry bucket with hardcoded placement |

---

## 14. Open Questions

### 14.1 Design Questions

1. **Registry bucket placement strategy?**
   - Current proposal: Hardcoded to Pool 0, Set 0
   - Alternative: Replicate registry to all pools for redundancy?
   - **Decision needed before Phase 1**

2. **Migration priority for objects?**
   - FIFO (first scanned, first migrated)
   - Size-based (small objects first for quick wins)
   - Access-based (hot objects first)
   - **Decision needed before Phase 3**

3. **Should we support cross-pool migrations?**
   - Current scope: Only within-pool node changes
   - Future: Move objects between pools for rebalancing?
   - **Defer to future work**

4. **Registry TTL and cache invalidation strategy?**
   - Current proposal: 5-minute TTL + explicit invalidation
   - Alternative: Event-driven invalidation only?
   - **Decision needed before Phase 1**

### 14.2 Implementation Questions

1. **Should migration be opt-in or automatic?**
   - Automatic: Start immediately after topology change
   - Manual: Require explicit `mc admin cluster start-migration`
   - **Decision needed before Phase 3**

2. **How to handle registry during initial cluster setup?**
   - Bootstrap: Create empty registry bucket
   - Backfill: Scan all objects and populate registry
   - **Decision needed before Phase 1**

3. **What happens to in-flight multipart uploads during migration?**
   - Abort all multipart uploads in source set?
   - Allow completion, then migrate?
   - **Decision needed before Phase 3**

### 14.3 Operational Questions

1. **Should we provide a migration from existing MinIO to new architecture?**
   - Current scope: Fresh deployments only
   - Future: In-place upgrade path?
   - **Defer decision until PoC complete**

2. **What metrics should be exposed for monitoring?**
   - Registry cache hit rate
   - Migration progress (objects/sec, bytes/sec)
   - Topology generation number
   - **Define in Phase 4**

3. **How to handle disaster recovery (backup/restore)?**
   - Registry backed up separately or with data?
   - Restore procedure if registry lost?
   - **Define in Phase 5**

---

## 15. Success Criteria

### 15.1 Functional Criteria

- ✅ Add 1-2 nodes to existing cluster without downtime
- ✅ Remove failed node without data loss
- ✅ All objects accessible during migration
- ✅ Migration completes with 0 data corruption
- ✅ Cluster survives node failure during migration

### 15.2 Performance Criteria

- ✅ Read latency <5ms (GET operations)
- ✅ Write throughput degradation <10%
- ✅ Migration affects 20-30% of objects per node change
- ✅ Migration throughput >500 MB/s
- ✅ Cache hit rate >99% for hot objects

### 15.3 Operational Criteria

- ✅ Node addition completes in <1 minute (excluding migration)
- ✅ Migration resumable after restart
- ✅ Clear status reporting (`mc admin cluster migration-status`)
- ✅ Comprehensive documentation
- ✅ Runbook for failure scenarios

---

## 16. Future Enhancements

Beyond the initial implementation, potential enhancements:

1. **Intelligent placement policies**
   - Hot/cold tiering within cluster
   - Geographic awareness (multi-DC deployments)
   - Cost-based placement (SSD vs HDD sets)

2. **Advanced migration strategies**
   - Prioritize hot objects
   - Throttle based on cluster load
   - Predictive migration (anticipate capacity needs)

3. **Registry optimizations**
   - Bloom filters for negative lookups
   - Registry sharding for massive scale (10B+ objects)
   - Compressed registry format

4. **Operational tooling**
   - Dry-run mode for topology changes
   - Migration cost estimator
   - Automated rebalancing recommendations

---

## 17. Conclusion

This architecture provides a pragmatic path to fine-grained scalability in MinIO while maintaining the system's core principles of simplicity and reliability. By combining location registry with consistent hashing, we achieve:

- **Operational Flexibility**: Add/remove individual nodes as needed
- **Performance**: Faster reads via direct lookup, acceptable write overhead
- **Reliability**: Zero data loss, graceful degradation, resumable migrations
- **Simplicity**: Self-contained design with no external dependencies

The phased implementation plan allows for iterative development and validation, reducing risk while delivering incremental value.

**Next Steps:**
1. Review and approve this architecture document
2. Resolve open questions (Section 14)
3. Begin Phase 1 implementation (Location Registry)
4. Establish performance baseline for comparison

---

**Document Version History:**

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-02-25 | Architecture Team | Initial proposal |

**Approvals:**

- [ ] Technical Lead
- [ ] Product Manager
- [ ] Operations Lead
- [ ] Security Review
