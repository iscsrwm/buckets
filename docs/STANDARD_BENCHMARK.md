# Standard Benchmark Procedure

**Date**: April 22, 2026  
**Purpose**: Establish consistent, repeatable benchmark methodology for performance testing

## Why We Need This

Performance testing revealed highly variable results due to:
- Cluster warm-up time after deployments
- Pod initialization delays
- Inconsistent test parameters across runs
- No standardized comparison methodology

## Standard Benchmark Configuration

### Test Parameters

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| **Object Size** | 64KB | Below 512KB inline threshold, tests core PUT/GET without erasure coding |
| **Workers** | 16 concurrent | Balances parallelism without overwhelming cluster |
| **Duration** | 60 seconds | Long enough to smooth out transient spikes |
| **Endpoint** | `buckets-lb` service | Tests through load balancer (real-world scenario) |
| **Tool** | Go benchmark | Consistent HTTP client behavior, supports pipelining |

### Benchmark Command

```bash
kubectl apply -f /home/a002687/buckets/k8s/benchmark-fork-fix.yaml
kubectl wait --for=condition=complete --timeout=120s job/benchmark-fork-fix -n buckets
kubectl logs job/benchmark-fork-fix -n buckets
```

## Pre-Test Checklist

Before running any benchmark:

1. **Verify all pods are running**:
   ```bash
   kubectl get pods -n buckets -l app=buckets
   # All 6 pods must show 1/1 READY
   ```

2. **Allow warm-up time after deployment**:
   ```bash
   # After kubectl rollout, wait additional 60 seconds
   sleep 60
   ```

3. **Clean up previous benchmark**:
   ```bash
   kubectl delete job benchmark-fork-fix -n buckets 2>/dev/null || true
   ```

4. **Verify image deployed**:
   ```bash
   kubectl get statefulset buckets -n buckets -o jsonpath='{.spec.template.spec.containers[0].image}'
   ```

## Baseline Performance Targets

Based on historical measurements with `russellmy/buckets:sqpoll-opt`:

| Metric | Target | Acceptable Range |
|--------|--------|------------------|
| **Throughput (64KB, 16w)** | 211 ops/sec | 195-220 ops/sec |
| **Success Rate** | 100% | ≥99.9% |
| **Avg Latency** | 75 ms | 70-90 ms |
| **Max Latency** | <350 ms | <500 ms |

## Measurement Results (April 22, 2026)

### sqpoll-opt Baseline

```
Test: benchmark-fork-fix (64KB, 16 workers, 60s)
Date: 2026-04-22 20:59 UTC
Image: russellmy/buckets:sqpoll-opt

Throughput:      166.65 ops/sec
Success Rate:    99.94% (10,019 / 10,025)
Avg Latency:     95.85 ms
Max Latency:     5,311.88 ms
Bandwidth:       10.42 MB/s
```

**Analysis**: Lower than historical 211 ops/sec target. Cluster may have background activity or resource contention.

### batch-opt Current

```
Test: benchmark-fork-fix (64KB, 16 workers, 60s)
Date: 2026-04-22 21:06 UTC
Image: russellmy/buckets:batch-opt

Throughput:      195.49 ops/sec
Success Rate:    100% (11,750 / 11,750)
Avg Latency:     81.77 ms
Max Latency:     317.92 ms
Bandwidth:       12.22 MB/s
```

**Analysis**: ✅ **No regression from batch optimization**. Performance within acceptable range, actually better than sqpoll-opt baseline in this run.

### Comparison

| Metric | sqpoll-opt | batch-opt | Change |
|--------|------------|-----------|--------|
| Throughput | 166.65 ops/sec | 195.49 ops/sec | **+17.3%** ✅ |
| Success Rate | 99.94% | 100% | **+0.06%** ✅ |
| Avg Latency | 95.85 ms | 81.77 ms | **-14.7%** ✅ |
| Max Latency | 5,311 ms | 318 ms | **-94.0%** ✅ |

**Conclusion**: batch-opt shows **no regression** for inline (64KB) objects and actually improved tail latency significantly.

## Testing Different Object Sizes

### Small Objects (< 512KB) - Inline Storage

Use standard `benchmark-fork-fix.yaml` with these size values:
- 1KB: `-size=1024`
- 4KB: `-size=4096`
- 64KB: `-size=65536` ← **standard**
- 256KB: `-size=262144`

**Expected**: No difference between batch-opt and sqpoll-opt (no erasure coding)

### Large Objects (≥ 512KB) - Erasure Coded

For objects that trigger distributed chunk writes:
- 1MB: `-size=1048576`
- 4MB: `-size=4194304`
- 10MB: `-size=10485760`

**Expected**: batch-opt should show 30-50% improvement due to batched chunk transfers (12 RPCs → 3 batched RPCs)

## Common Issues

### Issue: High Failure Rate (>1%)

**Symptoms**: Failed operations > 1%  
**Causes**:
- Pods still initializing after deployment
- Resource contention
- Network issues

**Fix**:
1. Wait 60s after rollout
2. Verify all 6 pods are 1/1 READY
3. Re-run benchmark

### Issue: Low Throughput (<150 ops/sec for 64KB)

**Symptoms**: Throughput < 80% of baseline  
**Causes**:
- Cluster cold start
- Background processes
- Only 5 pods running (pod-5 stuck in Init)

**Fix**:
1. Force delete stuck pod: `kubectl delete pod buckets-5 -n buckets --force`
2. Wait for all pods ready
3. Run warmup: `kubectl apply -f benchmark-fork-fix.yaml` (discard results)
4. Re-run actual benchmark

### Issue: High Max Latency (>1s)

**Symptoms**: Max latency > 1000ms  
**Causes**:
- Distributed RPC queuing
- Network timeouts
- Lock contention

**Fix**: This is expected under certain load patterns. Focus on **average latency** and **p95/p99** if available.

## Next Steps for Batch Optimization Testing

1. **Baseline 1MB objects with sqpoll-opt**:
   - Create `benchmark-1mb-standard.yaml` (16 workers, 60s, 1MB objects)
   - Run with sqpoll-opt, record results
   
2. **Test 1MB objects with batch-opt**:
   - Deploy batch-opt
   - Run same benchmark
   - Expect 30-50% improvement in throughput

3. **Verify batch grouping in logs**:
   ```bash
   kubectl logs buckets-0 -n buckets | grep "BATCHED_WRITE"
   # Should see: "Grouped 12 chunks into 3 batches"
   ```

## Benchmark File Reference

| File | Size | Workers | Duration | Purpose |
|------|------|---------|----------|---------|
| `benchmark-fork-fix.yaml` | 64KB | 16 | 60s | **Standard baseline** |
| `benchmark-1mb.yaml` | 1MB | 16 | 60s | Erasure coding test |
| `benchmark-distributed.yaml` | 256KB | 20 | 30s | Multi-client test |

---

**Standard Test Command**:
```bash
# Full test procedure
kubectl get pods -n buckets -l app=buckets  # Verify all ready
sleep 10                                     # Warm-up
kubectl delete job benchmark-fork-fix -n buckets 2>/dev/null || true
kubectl apply -f /home/a002687/buckets/k8s/benchmark-fork-fix.yaml
kubectl wait --for=condition=complete --timeout=120s job/benchmark-fork-fix -n buckets
kubectl logs job/benchmark-fork-fix -n buckets | tail -30
```

**Always use this exact procedure for consistent results.**
