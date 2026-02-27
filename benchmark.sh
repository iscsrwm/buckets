#!/bin/bash
# Buckets Performance Benchmark Script
# Tests parallel RPC performance across 6-node cluster

set -e

ENDPOINT="http://localhost:9001"
BUCKET="benchmark"
RESULTS_FILE="benchmark_results.txt"
TMP_DIR="/tmp/buckets-benchmark"

mkdir -p "$TMP_DIR"

echo "============================================"
echo "Buckets Performance Benchmark"
echo "============================================"
echo "Cluster: 6 nodes (localhost:9001-9006)"
echo "Erasure: K=8, M=4 (12 chunks per object)"
echo "Date: $(date)"
echo "============================================"
echo ""

# Create benchmark bucket
echo "Creating benchmark bucket..."
curl -s -X PUT "$ENDPOINT/$BUCKET" > /dev/null || true
echo "✓ Bucket created"
echo ""

# Function to benchmark upload
benchmark_upload() {
    local size_mb=$1
    local size_bytes=$((size_mb * 1024 * 1024))
    local filename="test_${size_mb}mb.bin"
    local iterations=3
    
    echo "----------------------------------------"
    echo "Upload Benchmark: ${size_mb}MB file"
    echo "----------------------------------------"
    
    # Generate test file
    echo "Generating ${size_mb}MB test file..."
    dd if=/dev/urandom of="$TMP_DIR/$filename" bs=1M count=$size_mb 2>/dev/null
    local md5_orig=$(md5sum "$TMP_DIR/$filename" | awk '{print $1}')
    echo "✓ File generated (MD5: $md5_orig)"
    
    # Warm-up run
    echo "Warm-up run..."
    curl -s -X PUT -T "$TMP_DIR/$filename" "$ENDPOINT/$BUCKET/warmup_$filename" > /dev/null
    
    # Benchmark runs
    local total_time=0
    echo "Running $iterations benchmark iterations..."
    for i in $(seq 1 $iterations); do
        local start=$(date +%s.%N)
        curl -s -X PUT -T "$TMP_DIR/$filename" "$ENDPOINT/$BUCKET/${i}_$filename" > /dev/null
        local end=$(date +%s.%N)
        local duration=$(echo "$end - $start" | bc)
        echo "  Run $i: ${duration}s"
        total_time=$(echo "$total_time + $duration" | bc)
    done
    
    local avg_time=$(echo "scale=3; $total_time / $iterations" | bc)
    local throughput=$(echo "scale=2; $size_mb / $avg_time" | bc)
    
    echo ""
    echo "Results:"
    echo "  Average time: ${avg_time}s"
    echo "  Throughput: ${throughput} MB/s"
    echo "  Total data written: $((size_mb * iterations))MB"
    echo ""
    
    # Log results
    echo "${size_mb}MB,upload,$avg_time,$throughput" >> "$RESULTS_FILE"
}

# Function to benchmark download
benchmark_download() {
    local size_mb=$1
    local filename="test_${size_mb}mb.bin"
    local iterations=3
    
    echo "----------------------------------------"
    echo "Download Benchmark: ${size_mb}MB file"
    echo "----------------------------------------"
    
    # Use file from upload benchmark
    local md5_orig=$(md5sum "$TMP_DIR/$filename" | awk '{print $1}')
    
    # Warm-up run
    echo "Warm-up run..."
    curl -s "$ENDPOINT/$BUCKET/1_$filename" > /dev/null
    
    # Benchmark runs
    local total_time=0
    local md5_match=true
    echo "Running $iterations benchmark iterations..."
    for i in $(seq 1 $iterations); do
        local start=$(date +%s.%N)
        curl -s "$ENDPOINT/$BUCKET/${i}_$filename" -o "$TMP_DIR/downloaded_$filename"
        local end=$(date +%s.%N)
        local duration=$(echo "$end - $start" | bc)
        echo "  Run $i: ${duration}s"
        total_time=$(echo "$total_time + $duration" | bc)
        
        # Verify MD5
        local md5_down=$(md5sum "$TMP_DIR/downloaded_$filename" | awk '{print $1}')
        if [ "$md5_orig" != "$md5_down" ]; then
            echo "  ✗ MD5 mismatch! Expected: $md5_orig, Got: $md5_down"
            md5_match=false
        fi
    done
    
    local avg_time=$(echo "scale=3; $total_time / $iterations" | bc)
    local throughput=$(echo "scale=2; $size_mb / $avg_time" | bc)
    
    echo ""
    echo "Results:"
    echo "  Average time: ${avg_time}s"
    echo "  Throughput: ${throughput} MB/s"
    echo "  MD5 verification: $([ "$md5_match" = true ] && echo "✓ PASS" || echo "✗ FAIL")"
    echo ""
    
    # Log results
    echo "${size_mb}MB,download,$avg_time,$throughput" >> "$RESULTS_FILE"
}

# Function to benchmark concurrent uploads
benchmark_concurrent() {
    local size_mb=$1
    local concurrency=$2
    local filename="test_${size_mb}mb.bin"
    
    echo "----------------------------------------"
    echo "Concurrent Upload: $concurrency × ${size_mb}MB"
    echo "----------------------------------------"
    
    # Generate test file if not exists
    if [ ! -f "$TMP_DIR/$filename" ]; then
        dd if=/dev/urandom of="$TMP_DIR/$filename" bs=1M count=$size_mb 2>/dev/null
    fi
    
    echo "Starting $concurrency concurrent uploads..."
    local start=$(date +%s.%N)
    
    # Launch concurrent uploads
    for i in $(seq 1 $concurrency); do
        curl -s -X PUT -T "$TMP_DIR/$filename" "$ENDPOINT/$BUCKET/concurrent_${i}_$filename" > /dev/null &
    done
    
    # Wait for all to complete
    wait
    
    local end=$(date +%s.%N)
    local duration=$(echo "$end - $start" | bc)
    local total_mb=$((size_mb * concurrency))
    local throughput=$(echo "scale=2; $total_mb / $duration" | bc)
    
    echo ""
    echo "Results:"
    echo "  Total time: ${duration}s"
    echo "  Total data: ${total_mb}MB"
    echo "  Aggregate throughput: ${throughput} MB/s"
    echo ""
    
    # Log results
    echo "${size_mb}MB,concurrent_${concurrency}x,$duration,$throughput" >> "$RESULTS_FILE"
}

# Initialize results file
echo "size,operation,time_seconds,throughput_mbps" > "$RESULTS_FILE"

# Run benchmarks
echo "Starting benchmarks..."
echo ""

# Single file benchmarks - various sizes
benchmark_upload 1
benchmark_download 1

benchmark_upload 2
benchmark_download 2

benchmark_upload 5
benchmark_download 5

benchmark_upload 10
benchmark_download 10

benchmark_upload 50
benchmark_download 50

# Concurrent upload tests
benchmark_concurrent 2 5
benchmark_concurrent 2 10
benchmark_concurrent 5 5

echo "============================================"
echo "Benchmark Complete!"
echo "============================================"
echo ""
echo "Summary Report:"
echo "============================================"
cat "$RESULTS_FILE" | column -t -s,
echo ""
echo "Detailed results saved to: $RESULTS_FILE"
echo ""

# Analyze parallel RPC logs
echo "============================================"
echo "Parallel RPC Analysis (from logs)"
echo "============================================"
echo ""
echo "Parallel writes detected:"
grep -h "Parallel write:" /tmp/buckets-6node-node1.log 2>/dev/null | tail -20 || echo "No parallel writes logged"
echo ""
echo "Parallel reads detected:"
grep -h "Parallel read:" /tmp/buckets-6node-node1.log 2>/dev/null | tail -20 || echo "No parallel reads logged"
echo ""

# Cleanup
echo "Cleaning up temporary files..."
rm -rf "$TMP_DIR"
echo "✓ Cleanup complete"
