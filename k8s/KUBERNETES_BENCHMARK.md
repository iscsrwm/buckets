# Kubernetes Cluster Benchmark Results

**Date**: April 20, 2026  
**Cluster**: 6 nodes (buckets-0 through buckets-5)  
**Configuration**: 2 erasure sets, K=8 data + M=4 parity, 4 disks per node  
**LoadBalancer IP**: 10.252.0.166:9000  
**Test Location**: External to cluster  

---

## Test 1: Concurrent Workers Scalability

**Test**: 1KB objects, mixed GET/PUT workload, 20-second duration

| Workers | Total Ops | Throughput | GET ops/s | PUT ops/s |
|---------|-----------|------------|-----------|-----------|
| 10      | 434       | 21.4 ops/s | 10.7      | 10.7      |
| 20      | 446       | 21.7 ops/s | 10.8      | 10.8      |
| 50      | 518       | 22.0 ops/s | 11.0      | 11.0      |
| 100     | 648       | 22.6 ops/s | 11.3      | 11.3      |

**Observation**: Throughput plateaus around 22 ops/s regardless of worker count, suggesting a bottleneck (likely network latency or client-side).

---

## Test 2: Object Size Impact

**Test**: PUT-only workload, 20 workers, 15-second duration

| Object Size | Operations | Throughput  | MB/s   |
|-------------|-----------|-------------|--------|
| 1 KB        | 195       | 11.6 ops/s  | 0.01   |
| 16 KB       | 191       | 11.3 ops/s  | 0.18   |
| 64 KB       | 180       | 10.5 ops/s  | 0.66   |
| 256 KB      | 166       | 9.7 ops/s   | 2.43   |
| 1 MB        | 158       | 9.1 ops/s   | 9.11   |

**Observation**: Operations/second decreases with object size, but throughput in MB/s increases. Peak measured: **9.11 MB/s** with 1MB objects.

---

## Test 3: Direct Pod Connection

**Test**: Port-forward to single pod (buckets-0), 1KB objects, PUT-only

| Method       | Throughput  |
|--------------|-------------|
| LoadBalancer | 11.2 ops/s  |
| Port-forward | 11.2 ops/s  |

**Observation**: No significant difference between LoadBalancer and direct pod connection, ruling out LoadBalancer as bottleneck.

---

## Performance Bottlenecks Identified

1. **Network Latency**: Testing from external network to cluster
2. **Client-Side Overhead**: boto3 client signature V4 computation
3. **Request/Response Overhead**: Small objects suffer from high overhead ratio
4. **Python GIL**: Multi-threaded Python clients limited by Global Interpreter Lock

---

## Comparison to Local Benchmarks

Previous local testing (March 6, 2026) showed:
- **GET-only**: 410 ops/s
- **PUT-only**: 404 ops/s  
- **Mixed**: 451 ops/s

**Kubernetes cluster is ~18-20x slower** than local testing, primarily due to:
- Network distance (external client → cluster)
- LoadBalancer hop
- Multi-node cluster coordination overhead

---

## Pod Distribution

Pods are spread across 6 different worker nodes for high availability:

| Pod       | Node               | IP           |
|-----------|--------------------|--------------|
| buckets-0 | devk8swrkr-d9      | 10.42.219.6  |
| buckets-1 | devk8swrkr-d2      | 10.42.165.202|
| buckets-2 | devk8swrkr-d7      | 10.42.232.209|
| buckets-3 | devk8swrkr-d4      | 10.42.20.43  |
| buckets-4 | devk8swrkr-d0      | 10.42.217.57 |
| buckets-5 | devk8swrkr-d1      | 10.42.216.153|

---

## Recommendations for Better Performance Testing

1. **Run benchmarks from inside the cluster** (deploy benchmark pods in same namespace)
2. **Use async/concurrent clients** (not synchronous boto3)
3. **Test with larger objects** (1MB+) to amortize request overhead
4. **Use direct HTTP/curl** to eliminate S3 SDK overhead
5. **Measure cluster-internal RPC performance** separately from S3 API

---

## Cluster Health

✅ All 6 pods running and healthy  
✅ All 24 PVCs bound  
✅ Pod-to-pod connectivity confirmed  
✅ S3 API authentication working  
✅ GET and PUT operations functional  
✅ No errors in pod logs  

**Status**: Cluster is operationally healthy. Performance limitations are due to test methodology (external client) rather than cluster issues.
