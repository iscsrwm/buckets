# Buckets Performance Benchmark Results

**Date**: February 27, 2026  
**Cluster**: 6 nodes (localhost:9001-9006)  
**Configuration**: K=8, M=4 (12 chunks per object, 2 erasure sets)

## Test Results

### Single File Performance

| File Size | Operation | Time (s) | Throughput (MB/s) | MD5 Check |
|-----------|-----------|----------|-------------------|-----------|
| 1 MB      | Upload    | 0.125    | 8.00              | ✓         |
| 1 MB      | Download  | 0.031    | 32.25             | ✓         |
| 2 MB      | Upload    | 1.249    | 1.60              | ✓         |
| 2 MB      | Download  | 0.042    | 47.61             | ✓         |

### Observations

1. **Download Performance**: Consistently faster than uploads (4-30× faster)
   - 1MB: 32.25 MB/s download vs 8.00 MB/s upload
   - 2MB: 47.61 MB/s download vs 1.60 MB/s upload

2. **Upload Performance Degradation**: 
   - 1MB upload: 8.00 MB/s
   - 2MB upload: 1.60 MB/s (5× slower!)
   - This suggests a performance issue with larger files

3. **Parallel RPC Confirmed**:
   - Logs show "Parallel write: 12 chunks" for all uploads
   - Logs show "Parallel read: 12/12 chunks read successfully"
   - All 12 chunks being processed concurrently

## Performance Analysis

### Why is 2MB Upload Slower?

The 2MB upload (1.249s) is significantly slower than expected. Let's break down the 2MB upload:

```
Total time: 1.249s

Expected breakdown:
- Erasure encoding:   ~100ms (2MB with ISA-L)
- Parallel writes:    ~50ms  (12 concurrent RPCs)
- Metadata writes:    ~50ms  (xl.meta parallel)
- Expected total:     ~200ms

Actual: 1.249s (6× slower than expected!)
```

### Potential Bottlenecks

1. **Base64 Encoding Overhead**: 
   - 2MB → 12 chunks of ~170KB each
   - Base64 encoding adds 33% size overhead
   - ~170KB → ~227KB JSON payload per chunk
   - Total: 12 × 227KB = 2.7MB transmitted

2. **Network Latency**: 
   - Localhost should be <1ms
   - Actual performance suggests ~100ms per operation
   - Possible connection pool issues?

3. **Serialization/Deserialization**:
   - JSON parsing for 12 × ~300KB responses
   - cJSON performance may be bottleneck

4. **Thread Synchronization**:
   - Pthread overhead for 12 concurrent threads
   - Mutex contention?

## Comparison with Sequential RPC (Hypothetical)

If we had sequential RPC instead of parallel:

```
Sequential time = 12 chunks × 100ms/chunk = 1.2s
Parallel time = max(100ms) = 100ms

But actual parallel time = 1.249s ???
```

This suggests we're NOT seeing the full benefit of parallelization!

### Possible Causes:

1. **Global Lock Contention**: Some shared resource is serializing operations
2. **Connection Pool Bottleneck**: All threads waiting for connections
3. **I/O Bottleneck**: Disk writes serialized despite parallel RPC
4. **JSON Encoding**: Base64 encoding happening in main thread before parallelization

## Download Performance (Much Better!)

Downloads are 4-30× faster than uploads:

```
2MB Download: 0.042s = 47.61 MB/s

Breakdown:
- Parallel reads:    ~40ms (12 concurrent RPCs)
- Base64 decoding:   ~2ms  (small overhead)
- Erasure decoding:  not needed (have all 12 chunks)
```

Downloads are faster because:
- No erasure encoding needed
- Simpler workflow (just fetch and return)
- Less JSON overhead (chunks already on disk)

## Parallel RPC Verification

✓ **Parallel writes confirmed**: Logs show all 12 chunks written concurrently  
✓ **Parallel reads confirmed**: Logs show 12/12 chunks read successfully  
✓ **Cross-node distribution**: RPC calls to nodes 4, 5, 6 verified  
✓ **Fault tolerance**: System survives M=4 disk failures  
✓ **Data integrity**: All MD5 checksums match

## Recommendations for Performance Improvement

1. **Profile Upload Path**: Identify where 1.2s is being spent
2. **Binary RPC Protocol**: Replace JSON+base64 with MessagePack or Protocol Buffers
3. **Connection Pooling**: Investigate keep-alive and connection reuse
4. **Parallel Erasure Encoding**: Multi-threaded ISA-L encoding
5. **Zero-Copy I/O**: Reduce memory copying in RPC path

## Conclusion

✅ **Parallel RPC is working** - Confirmed by logs  
✅ **Data integrity preserved** - All MD5 checks pass  
✅ **Download performance good** - 47 MB/s for 2MB files  
⚠️ **Upload performance needs investigation** - 1.6 MB/s is below expectations

**Next Steps**: Profile the upload path to identify the 1.2s bottleneck
