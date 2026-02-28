#!/bin/bash
# Buckets Operations Per Second Benchmark
# Tests PUT, GET, DELETE, and HEAD operations across different object sizes

set -e

ENDPOINT="${ENDPOINT:-http://localhost:9001}"
BUCKET="ops-benchmark"
TMP_DIR="/tmp/buckets-ops-bench-$$"
RESULTS_FILE="benchmark_ops_results.csv"

# Default test parameters
DURATION=${DURATION:-10}        # Seconds per test
WARMUP=${WARMUP:-3}             # Warmup iterations

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

mkdir -p "$TMP_DIR"

echo "============================================"
echo "Buckets Operations Per Second Benchmark"
echo "============================================"
echo "Endpoint: $ENDPOINT"
echo "Duration per test: ${DURATION}s"
echo "Date: $(date)"
echo "============================================"
echo ""

# Check cluster health
echo "Checking cluster health..."
for port in 9001 9002 9003 9004 9005 9006; do
    status=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:${port}/" 2>/dev/null || echo "DOWN")
    if [ "$status" != "200" ]; then
        echo -e "${RED}Node on port $port is not responding${NC}"
    fi
done
echo -e "${GREEN}Cluster health check complete${NC}"
echo ""

# Create benchmark bucket
echo "Creating benchmark bucket..."
curl -s -X PUT "$ENDPOINT/$BUCKET" > /dev/null 2>&1 || true
echo ""

# Initialize results file
echo "size_bytes,size_label,operation,total_ops,duration_sec,ops_per_sec,avg_latency_ms,throughput_mbps" > "$RESULTS_FILE"

# Function to generate test file of specific size
generate_file() {
    local size=$1
    local file="$TMP_DIR/test_${size}.bin"
    if [ ! -f "$file" ]; then
        dd if=/dev/urandom of="$file" bs="$size" count=1 2>/dev/null
    fi
    echo "$file"
}

# Function to benchmark PUT operations
benchmark_put() {
    local size=$1
    local size_label=$2
    local file=$(generate_file $size)
    local count=0
    local start end duration
    
    echo -n "  PUT:  "
    
    # Warmup
    for i in $(seq 1 $WARMUP); do
        curl -s -X PUT -T "$file" "$ENDPOINT/$BUCKET/warmup_${size}_${i}" > /dev/null
    done
    
    # Timed test
    start=$(date +%s.%N)
    end=$(echo "$start + $DURATION" | bc)
    
    while [ "$(echo "$(date +%s.%N) < $end" | bc)" -eq 1 ]; do
        curl -s -X PUT -T "$file" "$ENDPOINT/$BUCKET/put_${size}_${count}" > /dev/null
        ((count++))
    done
    
    actual_end=$(date +%s.%N)
    duration=$(echo "$actual_end - $start" | bc)
    ops_per_sec=$(echo "scale=2; $count / $duration" | bc)
    avg_latency=$(echo "scale=2; $duration * 1000 / $count" | bc)
    throughput=$(echo "scale=2; $count * $size / $duration / 1048576" | bc)
    
    printf "%6d ops in %5.2fs = %7.2f ops/s (avg: %6.2fms, %6.2f MB/s)\n" \
           "$count" "$duration" "$ops_per_sec" "$avg_latency" "$throughput"
    
    echo "$size,$size_label,PUT,$count,$duration,$ops_per_sec,$avg_latency,$throughput" >> "$RESULTS_FILE"
}

# Function to benchmark GET operations
benchmark_get() {
    local size=$1
    local size_label=$2
    local count=0
    local start end duration
    
    echo -n "  GET:  "
    
    # Ensure we have objects to GET (use the ones from PUT test)
    # Warmup
    for i in $(seq 1 $WARMUP); do
        curl -s "$ENDPOINT/$BUCKET/put_${size}_0" -o /dev/null
    done
    
    # Timed test - cycle through available objects
    start=$(date +%s.%N)
    end=$(echo "$start + $DURATION" | bc)
    local obj_index=0
    
    while [ "$(echo "$(date +%s.%N) < $end" | bc)" -eq 1 ]; do
        curl -s "$ENDPOINT/$BUCKET/put_${size}_${obj_index}" -o /dev/null
        ((count++))
        ((obj_index++))
        # Cycle back if we run out of objects
        if [ $obj_index -ge 100 ]; then
            obj_index=0
        fi
    done
    
    actual_end=$(date +%s.%N)
    duration=$(echo "$actual_end - $start" | bc)
    ops_per_sec=$(echo "scale=2; $count / $duration" | bc)
    avg_latency=$(echo "scale=2; $duration * 1000 / $count" | bc)
    throughput=$(echo "scale=2; $count * $size / $duration / 1048576" | bc)
    
    printf "%6d ops in %5.2fs = %7.2f ops/s (avg: %6.2fms, %6.2f MB/s)\n" \
           "$count" "$duration" "$ops_per_sec" "$avg_latency" "$throughput"
    
    echo "$size,$size_label,GET,$count,$duration,$ops_per_sec,$avg_latency,$throughput" >> "$RESULTS_FILE"
}

# Function to benchmark HEAD operations
benchmark_head() {
    local size=$1
    local size_label=$2
    local count=0
    local start end duration
    
    echo -n "  HEAD: "
    
    # Warmup
    for i in $(seq 1 $WARMUP); do
        curl -s -I "$ENDPOINT/$BUCKET/put_${size}_0" > /dev/null
    done
    
    # Timed test
    start=$(date +%s.%N)
    end=$(echo "$start + $DURATION" | bc)
    local obj_index=0
    
    while [ "$(echo "$(date +%s.%N) < $end" | bc)" -eq 1 ]; do
        curl -s -I "$ENDPOINT/$BUCKET/put_${size}_${obj_index}" > /dev/null
        ((count++))
        ((obj_index++))
        if [ $obj_index -ge 100 ]; then
            obj_index=0
        fi
    done
    
    actual_end=$(date +%s.%N)
    duration=$(echo "$actual_end - $start" | bc)
    ops_per_sec=$(echo "scale=2; $count / $duration" | bc)
    avg_latency=$(echo "scale=2; $duration * 1000 / $count" | bc)
    
    printf "%6d ops in %5.2fs = %7.2f ops/s (avg: %6.2fms)\n" \
           "$count" "$duration" "$ops_per_sec" "$avg_latency"
    
    echo "$size,$size_label,HEAD,$count,$duration,$ops_per_sec,$avg_latency,0" >> "$RESULTS_FILE"
}

# Function to benchmark DELETE operations
benchmark_delete() {
    local size=$1
    local size_label=$2
    local count=0
    local start end duration
    
    echo -n "  DEL:  "
    
    # Create objects to delete
    local file=$(generate_file $size)
    local prep_count=0
    local target_count=$((DURATION * 50))  # Estimate ~50 ops/sec
    
    echo -n "(preparing ${target_count} objects) "
    for i in $(seq 0 $((target_count - 1))); do
        curl -s -X PUT -T "$file" "$ENDPOINT/$BUCKET/del_${size}_${i}" > /dev/null &
        ((prep_count++))
        # Limit parallelism
        if [ $((prep_count % 20)) -eq 0 ]; then
            wait
        fi
    done
    wait
    
    # Warmup
    for i in $(seq 1 $WARMUP); do
        curl -s -X DELETE "$ENDPOINT/$BUCKET/warmup_${size}_${i}" > /dev/null
    done
    
    # Timed test
    start=$(date +%s.%N)
    end=$(echo "$start + $DURATION" | bc)
    
    while [ "$(echo "$(date +%s.%N) < $end" | bc)" -eq 1 ] && [ $count -lt $target_count ]; do
        curl -s -X DELETE "$ENDPOINT/$BUCKET/del_${size}_${count}" > /dev/null
        ((count++))
    done
    
    actual_end=$(date +%s.%N)
    duration=$(echo "$actual_end - $start" | bc)
    ops_per_sec=$(echo "scale=2; $count / $duration" | bc)
    avg_latency=$(echo "scale=2; $duration * 1000 / $count" | bc)
    
    printf "%6d ops in %5.2fs = %7.2f ops/s (avg: %6.2fms)\n" \
           "$count" "$duration" "$ops_per_sec" "$avg_latency"
    
    echo "$size,$size_label,DELETE,$count,$duration,$ops_per_sec,$avg_latency,0" >> "$RESULTS_FILE"
}

# Function to run all benchmarks for a given size
benchmark_size() {
    local size=$1
    local label=$2
    
    echo -e "${BLUE}=== $label Objects ($size bytes) ===${NC}"
    
    benchmark_put $size "$label"
    benchmark_get $size "$label"
    benchmark_head $size "$label"
    benchmark_delete $size "$label"
    
    echo ""
}

# Run benchmarks for various sizes
echo "Starting benchmarks..."
echo ""

# Small objects (metadata-heavy workloads)
benchmark_size 1024 "1KB"
benchmark_size 4096 "4KB"
benchmark_size 16384 "16KB"

# Medium objects (typical web content)
benchmark_size 65536 "64KB"
benchmark_size 262144 "256KB"

# Large objects (file storage)
benchmark_size 1048576 "1MB"
benchmark_size 5242880 "5MB"
benchmark_size 10485760 "10MB"

# Print summary
echo "============================================"
echo "SUMMARY"
echo "============================================"
echo ""

echo "Operations Per Second by Size and Operation:"
echo ""
printf "%-8s | %10s | %10s | %10s | %10s\n" "Size" "PUT" "GET" "HEAD" "DELETE"
printf "%-8s-+-%10s-+-%10s-+-%10s-+-%10s\n" "--------" "----------" "----------" "----------" "----------"

# Parse results and create summary
for size_label in "1KB" "4KB" "16KB" "64KB" "256KB" "1MB" "5MB" "10MB"; do
    put_ops=$(grep ",$size_label,PUT," "$RESULTS_FILE" | cut -d',' -f6)
    get_ops=$(grep ",$size_label,GET," "$RESULTS_FILE" | cut -d',' -f6)
    head_ops=$(grep ",$size_label,HEAD," "$RESULTS_FILE" | cut -d',' -f6)
    del_ops=$(grep ",$size_label,DELETE," "$RESULTS_FILE" | cut -d',' -f6)
    
    printf "%-8s | %10s | %10s | %10s | %10s\n" \
           "$size_label" "${put_ops:-N/A}" "${get_ops:-N/A}" "${head_ops:-N/A}" "${del_ops:-N/A}"
done

echo ""
echo "Throughput (MB/s) for PUT and GET:"
echo ""
printf "%-8s | %12s | %12s\n" "Size" "PUT (MB/s)" "GET (MB/s)"
printf "%-8s-+-%12s-+-%12s\n" "--------" "------------" "------------"

for size_label in "1KB" "4KB" "16KB" "64KB" "256KB" "1MB" "5MB" "10MB"; do
    put_tp=$(grep ",$size_label,PUT," "$RESULTS_FILE" | cut -d',' -f8)
    get_tp=$(grep ",$size_label,GET," "$RESULTS_FILE" | cut -d',' -f8)
    
    printf "%-8s | %12s | %12s\n" "$size_label" "${put_tp:-N/A}" "${get_tp:-N/A}"
done

echo ""
echo "Average Latency (ms):"
echo ""
printf "%-8s | %10s | %10s | %10s | %10s\n" "Size" "PUT" "GET" "HEAD" "DELETE"
printf "%-8s-+-%10s-+-%10s-+-%10s-+-%10s\n" "--------" "----------" "----------" "----------" "----------"

for size_label in "1KB" "4KB" "16KB" "64KB" "256KB" "1MB" "5MB" "10MB"; do
    put_lat=$(grep ",$size_label,PUT," "$RESULTS_FILE" | cut -d',' -f7)
    get_lat=$(grep ",$size_label,GET," "$RESULTS_FILE" | cut -d',' -f7)
    head_lat=$(grep ",$size_label,HEAD," "$RESULTS_FILE" | cut -d',' -f7)
    del_lat=$(grep ",$size_label,DELETE," "$RESULTS_FILE" | cut -d',' -f7)
    
    printf "%-8s | %10s | %10s | %10s | %10s\n" \
           "$size_label" "${put_lat:-N/A}" "${get_lat:-N/A}" "${head_lat:-N/A}" "${del_lat:-N/A}"
done

echo ""
echo "============================================"

# Final cluster health check
echo "Final cluster health check..."
all_healthy=true
for port in 9001 9002 9003 9004 9005 9006; do
    status=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:${port}/" 2>/dev/null || echo "DOWN")
    if [ "$status" != "200" ]; then
        echo -e "${RED}Node on port $port FAILED${NC}"
        all_healthy=false
    fi
done

if [ "$all_healthy" = true ]; then
    echo -e "${GREEN}All nodes healthy after benchmark${NC}"
fi

echo ""
echo "Detailed results saved to: $RESULTS_FILE"
echo ""

# Cleanup
rm -rf "$TMP_DIR"

echo "Benchmark complete!"
