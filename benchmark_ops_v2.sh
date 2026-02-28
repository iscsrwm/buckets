#!/bin/bash
# Buckets Operations Per Second Benchmark v2
# Measures throughput and latency for various object sizes

set -e

ENDPOINT="${ENDPOINT:-http://localhost:9001}"
BUCKET="bench-ops-v2"
TMP="/tmp/buckets-bench-ops-$$"

mkdir -p "$TMP"

echo "============================================================"
echo "        Buckets Operations Per Second Benchmark"
echo "============================================================"
echo "Endpoint:  $ENDPOINT"
echo "Date:      $(date)"
echo "============================================================"
echo ""

# Check cluster health
echo "Cluster Health Check:"
all_up=true
for port in 9001 9002 9003 9004 9005 9006; do
    status=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:${port}/" 2>/dev/null || echo "DOWN")
    if [ "$status" = "200" ]; then
        echo "  Node $port: OK"
    else
        echo "  Node $port: DOWN"
        all_up=false
    fi
done
echo ""

if [ "$all_up" != "true" ]; then
    echo "WARNING: Not all nodes are healthy!"
    echo ""
fi

# Create bucket
curl -s -X PUT "$ENDPOINT/$BUCKET" > /dev/null 2>&1 || true

# Generate test files
echo "Generating test files..."
for size in 1K 4K 16K 64K 256K 1M 4M; do
    dd if=/dev/urandom of="$TMP/$size.bin" bs=$size count=1 2>/dev/null
done
echo "  Files ready: 1K, 4K, 16K, 64K, 256K, 1M, 4M"
echo ""

# Results arrays
declare -A PUT_OPS GET_OPS HEAD_OPS DEL_OPS
declare -A PUT_LAT GET_LAT HEAD_LAT DEL_LAT
declare -A PUT_MBPS GET_MBPS

# Run PUT benchmark for a size
bench_put() {
    local size_label=$1
    local file="$TMP/${size_label}.bin"
    local file_size=$(stat -c%s "$file")
    local ops=10
    
    # Increase ops for small files
    if [ $file_size -lt 65536 ]; then
        ops=20
    fi
    
    local start=$(date +%s.%N)
    for i in $(seq 1 $ops); do
        curl -s -X PUT -T "$file" "$ENDPOINT/$BUCKET/put_${size_label}_${i}" > /dev/null
    done
    local end=$(date +%s.%N)
    
    local duration=$(echo "$end - $start" | bc)
    local ops_sec=$(echo "scale=2; $ops / $duration" | bc)
    local lat_ms=$(echo "scale=1; $duration * 1000 / $ops" | bc)
    local mbps=$(echo "scale=2; $ops * $file_size / $duration / 1048576" | bc)
    
    PUT_OPS[$size_label]=$ops_sec
    PUT_LAT[$size_label]=$lat_ms
    PUT_MBPS[$size_label]=$mbps
    
    printf "  PUT  %-5s: %5.2f ops/s  %6.1f ms  %6.2f MB/s\n" "$size_label" "$ops_sec" "$lat_ms" "$mbps"
}

# Run GET benchmark for a size
bench_get() {
    local size_label=$1
    local file="$TMP/${size_label}.bin"
    local file_size=$(stat -c%s "$file")
    local ops=20
    
    # Increase ops for small files
    if [ $file_size -lt 65536 ]; then
        ops=50
    fi
    
    local start=$(date +%s.%N)
    for i in $(seq 1 $ops); do
        idx=$(( (i % 10) + 1 ))
        curl -s "$ENDPOINT/$BUCKET/put_${size_label}_${idx}" -o /dev/null
    done
    local end=$(date +%s.%N)
    
    local duration=$(echo "$end - $start" | bc)
    local ops_sec=$(echo "scale=2; $ops / $duration" | bc)
    local lat_ms=$(echo "scale=1; $duration * 1000 / $ops" | bc)
    local mbps=$(echo "scale=2; $ops * $file_size / $duration / 1048576" | bc)
    
    GET_OPS[$size_label]=$ops_sec
    GET_LAT[$size_label]=$lat_ms
    GET_MBPS[$size_label]=$mbps
    
    printf "  GET  %-5s: %5.2f ops/s  %6.1f ms  %6.2f MB/s\n" "$size_label" "$ops_sec" "$lat_ms" "$mbps"
}

# Run HEAD benchmark
bench_head() {
    local size_label=$1
    local ops=30
    
    local start=$(date +%s.%N)
    for i in $(seq 1 $ops); do
        idx=$(( (i % 10) + 1 ))
        curl -s -I "$ENDPOINT/$BUCKET/put_${size_label}_${idx}" > /dev/null
    done
    local end=$(date +%s.%N)
    
    local duration=$(echo "$end - $start" | bc)
    local ops_sec=$(echo "scale=2; $ops / $duration" | bc)
    local lat_ms=$(echo "scale=1; $duration * 1000 / $ops" | bc)
    
    HEAD_OPS[$size_label]=$ops_sec
    HEAD_LAT[$size_label]=$lat_ms
    
    printf "  HEAD %-5s: %5.2f ops/s  %6.1f ms\n" "$size_label" "$ops_sec" "$lat_ms"
}

# Run DELETE benchmark
bench_delete() {
    local size_label=$1
    local file="$TMP/${size_label}.bin"
    local ops=10
    
    # First create objects to delete
    for i in $(seq 1 $ops); do
        curl -s -X PUT -T "$file" "$ENDPOINT/$BUCKET/del_${size_label}_${i}" > /dev/null &
    done
    wait
    
    local start=$(date +%s.%N)
    for i in $(seq 1 $ops); do
        curl -s -X DELETE "$ENDPOINT/$BUCKET/del_${size_label}_${i}" > /dev/null
    done
    local end=$(date +%s.%N)
    
    local duration=$(echo "$end - $start" | bc)
    local ops_sec=$(echo "scale=2; $ops / $duration" | bc)
    local lat_ms=$(echo "scale=1; $duration * 1000 / $ops" | bc)
    
    DEL_OPS[$size_label]=$ops_sec
    DEL_LAT[$size_label]=$lat_ms
    
    printf "  DEL  %-5s: %5.2f ops/s  %6.1f ms\n" "$size_label" "$ops_sec" "$lat_ms"
}

# Run benchmarks
SIZES="1K 4K 16K 64K 256K 1M 4M"

echo "=== PUT Operations ==="
for size in $SIZES; do
    bench_put $size
done
echo ""

echo "=== GET Operations ==="
for size in $SIZES; do
    bench_get $size
done
echo ""

echo "=== HEAD Operations ==="
for size in $SIZES; do
    bench_head $size
done
echo ""

echo "=== DELETE Operations ==="
for size in $SIZES; do
    bench_delete $size
done
echo ""

# Summary tables
echo "============================================================"
echo "                      SUMMARY"
echo "============================================================"
echo ""

echo "Operations Per Second:"
echo "------------------------------------------------------------"
printf "%-8s | %10s | %10s | %10s | %10s\n" "Size" "PUT" "GET" "HEAD" "DELETE"
printf "%-8s-+-%10s-+-%10s-+-%10s-+-%10s\n" "--------" "----------" "----------" "----------" "----------"
for size in $SIZES; do
    printf "%-8s | %10s | %10s | %10s | %10s\n" \
        "$size" "${PUT_OPS[$size]}" "${GET_OPS[$size]}" "${HEAD_OPS[$size]}" "${DEL_OPS[$size]}"
done
echo ""

echo "Average Latency (ms):"
echo "------------------------------------------------------------"
printf "%-8s | %10s | %10s | %10s | %10s\n" "Size" "PUT" "GET" "HEAD" "DELETE"
printf "%-8s-+-%10s-+-%10s-+-%10s-+-%10s\n" "--------" "----------" "----------" "----------" "----------"
for size in $SIZES; do
    printf "%-8s | %10s | %10s | %10s | %10s\n" \
        "$size" "${PUT_LAT[$size]}" "${GET_LAT[$size]}" "${HEAD_LAT[$size]}" "${DEL_LAT[$size]}"
done
echo ""

echo "Throughput (MB/s):"
echo "------------------------------------------------------------"
printf "%-8s | %12s | %12s\n" "Size" "PUT (MB/s)" "GET (MB/s)"
printf "%-8s-+-%12s-+-%12s\n" "--------" "------------" "------------"
for size in $SIZES; do
    printf "%-8s | %12s | %12s\n" "$size" "${PUT_MBPS[$size]}" "${GET_MBPS[$size]}"
done
echo ""

# Save CSV results
echo "size,put_ops,put_lat_ms,put_mbps,get_ops,get_lat_ms,get_mbps,head_ops,head_lat_ms,del_ops,del_lat_ms" > benchmark_ops_results.csv
for size in $SIZES; do
    echo "$size,${PUT_OPS[$size]},${PUT_LAT[$size]},${PUT_MBPS[$size]},${GET_OPS[$size]},${GET_LAT[$size]},${GET_MBPS[$size]},${HEAD_OPS[$size]},${HEAD_LAT[$size]},${DEL_OPS[$size]},${DEL_LAT[$size]}" >> benchmark_ops_results.csv
done
echo "Results saved to: benchmark_ops_results.csv"
echo ""

# Final health check
echo "Final Cluster Health Check:"
all_up=true
for port in 9001 9002 9003 9004 9005 9006; do
    status=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:${port}/" 2>/dev/null || echo "DOWN")
    if [ "$status" = "200" ]; then
        echo "  Node $port: OK"
    else
        echo "  Node $port: DOWN"
        all_up=false
    fi
done
echo ""

if [ "$all_up" = "true" ]; then
    echo "BENCHMARK COMPLETE: All nodes healthy"
else
    echo "WARNING: Some nodes failed during benchmark"
fi

# Cleanup
rm -rf "$TMP"
