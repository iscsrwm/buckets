# Multi-Node and Multi-Disk Testing Guide

This guide explains how to test Buckets with multiple nodes and disks, both in the current state and once clustering is fully integrated.

## Current State (Phase 9 - S3 API Layer)

Currently, the Buckets server is focused on implementing the S3 API layer. The clustering components (topology, registry, migration) are implemented as libraries but not yet fully integrated into the main server binary.

### What's Available Now

1. **Storage Layer** (Phase 4) - Supports multiple disks
2. **Location Registry** (Phase 5) - Tracks object locations across disks/nodes
3. **Topology Management** (Phase 6) - Manages node membership and disk sets
4. **Network Layer** (Phase 8) - HTTP server, RPC, peer discovery
5. **S3 API Layer** (Phase 9) - S3-compatible REST API (in progress)

### What's Not Yet Integrated

- Multi-node server startup and coordination
- Distributed object placement using topology
- Cross-node data migration
- Cluster-aware S3 operations

## Testing Single Node with Multiple Disks (Available Now)

You can test the storage layer with multiple disks using the unit tests:

### 1. Storage Layer Multi-Disk Tests

```bash
# Run multi-disk storage tests
make test-storage

# The tests create multiple disk paths and test:
# - Writing objects across disks
# - Reading from correct disk
# - Erasure coding across disks
# - Disk failure scenarios
```

### 2. Manual Multi-Disk Testing

Create a test program to use the storage APIs directly:

```c
#include "buckets.h"
#include "buckets_storage.h"

int main() {
    buckets_init();
    
    // Create multi-disk configuration
    const char *disks[] = {
        "/tmp/buckets-disk1",
        "/tmp/buckets-disk2",
        "/tmp/buckets-disk3",
        "/tmp/buckets-disk4"
    };
    
    buckets_multidisk_t *md = buckets_multidisk_create(disks, 4);
    
    // Write an object
    const char *data = "Hello, multi-disk!";
    buckets_object_write_opts_t opts = {
        .bucket = "test-bucket",
        .key = "test-object",
        .data = data,
        .size = strlen(data),
        .erasure_enabled = true,
        .data_shards = 2,
        .parity_shards = 2
    };
    
    buckets_object_write(md, &opts);
    
    // Read it back
    buckets_object_read_opts_t read_opts = {
        .bucket = "test-bucket",
        .key = "test-object"
    };
    
    buckets_object_t *obj = buckets_object_read(md, &read_opts);
    
    printf("Data: %.*s\n", (int)obj->size, obj->data);
    
    buckets_object_free(obj);
    buckets_multidisk_free(md);
    buckets_cleanup();
    
    return 0;
}
```

Compile and run:
```bash
gcc -o test_multidisk test_multidisk.c -Iinclude build/libbuckets.a -lssl -lcrypto -luuid -lz -lisal -lpthread -lm
./test_multidisk
```

### 3. Topology Tests

Test topology management with multiple nodes:

```bash
# Run topology tests
make test-topology
make test-topology-manager

# These tests create virtual "nodes" and test:
# - Node addition/removal
# - Disk set management
# - Set selection for objects
# - Quorum calculations
```

## Testing Multi-Node Setup (Requires Integration Work)

To test true multi-node clustering, we need to integrate the clustering components into the server. Here's how it would work:

### Architecture for Multi-Node Testing

```
Node 1 (localhost:9001)          Node 2 (localhost:9002)          Node 3 (localhost:9003)
├── /tmp/node1/disk1             ├── /tmp/node2/disk1             ├── /tmp/node3/disk1
├── /tmp/node1/disk2             ├── /tmp/node2/disk2             ├── /tmp/node3/disk2
└── /tmp/node1/disk3             └── /tmp/node2/disk3             └── /tmp/node3/disk3
```

### What Needs to Be Added to main.c

To support multi-node testing, the server would need:

1. **Configuration File** - Specify disks, peers, cluster settings
2. **Topology Initialization** - Load/create topology on startup
3. **Registry Initialization** - Track objects across nodes
4. **Peer Discovery** - Connect to other nodes
5. **Distributed Operations** - Route S3 requests to correct nodes

### Example Configuration (cluster.json)

```json
{
  "node": {
    "id": "node1",
    "address": "localhost:9001",
    "disks": [
      "/tmp/node1/disk1",
      "/tmp/node1/disk2",
      "/tmp/node1/disk3"
    ]
  },
  "cluster": {
    "peers": [
      "localhost:9002",
      "localhost:9003"
    ],
    "erasure": {
      "data_shards": 2,
      "parity_shards": 1
    },
    "sets": 4,
    "disks_per_set": 12
  }
}
```

### Future Server Startup

```bash
# Node 1
./bin/buckets server --config node1.json

# Node 2
./bin/buckets server --config node2.json

# Node 3
./bin/buckets server --config node3.json
```

## Docker-Based Multi-Node Testing (Recommended for Future)

Once clustering is integrated, the easiest way to test multi-node setups will be with Docker Compose:

### docker-compose.yml Example

```yaml
version: '3.8'

services:
  node1:
    image: buckets:latest
    ports:
      - "9001:9000"
    volumes:
      - ./data/node1:/data
    environment:
      - NODE_ID=node1
      - PEERS=node2:9000,node3:9000
    command: server --config /etc/buckets/cluster.json

  node2:
    image: buckets:latest
    ports:
      - "9002:9000"
    volumes:
      - ./data/node2:/data
    environment:
      - NODE_ID=node2
      - PEERS=node1:9000,node3:9000
    command: server --config /etc/buckets/cluster.json

  node3:
    image: buckets:latest
    ports:
      - "9003:9000"
    volumes:
      - ./data/node3:/data
    environment:
      - NODE_ID=node3
      - PEERS=node1:9000,node2:9000
    command: server --config /etc/buckets/cluster.json
```

Usage:
```bash
docker-compose up -d
docker-compose logs -f
```

## Current Testing Capabilities Summary

### ✅ Available Now
- **Unit tests** for all clustering components (topology, registry, migration)
- **Storage layer** with multi-disk support (can be tested programmatically)
- **Network layer** with HTTP server and RPC (can run multiple instances)
- **S3 API** single-node operations (fully functional)

### 🔄 In Progress
- **S3 API Layer** completion (Weeks 35-42, currently at Week 39)
- Multipart upload operations

### ⏳ Future Work (Post-Phase 9)
- Integration of clustering into main server binary
- Configuration file support
- Multi-node S3 request routing
- Distributed object placement
- Cross-node replication and erasure coding
- Cluster management commands (join, leave, rebalance)

## Manual Multi-Instance Testing (Workaround)

You can currently test some multi-node scenarios manually:

### Setup

```bash
# Terminal 1: Node 1
mkdir -p /tmp/node1/disk{1,2,3}
./bin/buckets server 9001

# Terminal 2: Node 2
mkdir -p /tmp/node2/disk{1,2,3}
./bin/buckets server 9002

# Terminal 3: Node 3
mkdir -p /tmp/node3/disk{1,2,3}
./bin/buckets server 9003
```

### Testing

```bash
# Create bucket on node 1
curl -X PUT http://localhost:9001/mybucket -H "Content-Length: 0"

# Upload object to node 1
echo "Hello from node 1" | curl -X PUT --data-binary @- http://localhost:9001/mybucket/obj1.txt

# Create bucket on node 2
curl -X PUT http://localhost:9002/mybucket -H "Content-Length: 0"

# Upload object to node 2
echo "Hello from node 2" | curl -X PUT --data-binary @- http://localhost:9002/mybucket/obj2.txt

# Note: Currently these are independent - no coordination between nodes
# Future: Objects would be distributed based on consistent hashing
```

## Simulating Distributed Scenarios

You can test distributed logic using the C APIs directly:

```c
// Test consistent hashing placement
#include "buckets_ring.h"

buckets_ring_t *ring = buckets_ring_create(150); // 150 vnodes per node

// Add nodes
buckets_ring_add_node(ring, "node1:9001");
buckets_ring_add_node(ring, "node2:9002");
buckets_ring_add_node(ring, "node3:9003");

// Find which nodes should store an object
const char *bucket = "mybucket";
const char *key = "myobject";
char hash_input[1024];
snprintf(hash_input, sizeof(hash_input), "%s/%s", bucket, key);

u64 hash = buckets_siphash((const u8*)hash_input, strlen(hash_input), NULL);

// Get 3 replicas (EC 2+1)
const char *nodes[3];
int count = buckets_ring_get_nodes(ring, hash, nodes, 3);

printf("Object %s/%s should be stored on:\n", bucket, key);
for (int i = 0; i < count; i++) {
    printf("  - %s\n", nodes[i]);
}
```

## Next Steps for Full Multi-Node Support

To enable complete multi-node testing, these tasks need to be completed:

1. **Configuration System** (Week 43-44)
   - JSON/YAML config parsing
   - Disk and peer specification
   - Cluster parameters

2. **Server Integration** (Week 45-46)
   - Load topology on startup
   - Initialize registry
   - Connect to peers
   - Health monitoring

3. **Distributed S3 Operations** (Week 47-48)
   - Route requests to correct nodes
   - Implement cross-node reads
   - Implement distributed writes with EC

4. **Testing Infrastructure** (Week 49-50)
   - Docker Compose setup
   - Multi-node test scripts
   - Chaos testing (node failures, network partitions)

## Reference

For more details on the clustering architecture:
- `architecture/SCALE_AND_DATA_PLACEMENT.md` - Overall design
- `architecture/CLUSTER_AND_STATE_MANAGEMENT.md` - Node coordination
- `ROADMAP.md` - Development phases and timeline

## Contributing

If you want to help add multi-node support:
1. Review the architecture documents
2. Look at the existing topology and registry implementations
3. Propose changes to main.c for server integration
4. Test with the unit tests and storage APIs

The clustering foundation is solid - it just needs integration into the server binary!
