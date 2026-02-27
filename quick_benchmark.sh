#!/bin/bash
# Quick Performance Benchmark - Focus on key metrics

set -e

ENDPOINT="http://localhost:9001"
BUCKET="perf"
TMP_DIR="/tmp/perf-test"

mkdir -p "$TMP_DIR"

echo "========================================"
echo "Buckets Quick Performance Benchmark"
echo "========================================"
echo "Cluster: 6 nodes, K=8 M=4 (12 chunks)"
echo "========================================"
echo ""

# Create bucket
curl -s -X PUT "$ENDPOINT/$BUCKET" > /dev/null 2>&1 || true

# Function to test file size
test_file() {
    local size_mb=$1
    local filename="perf_${size_mb}mb.bin"
    
    echo "Testing ${size_mb}MB file..."
    
    # Generate file
    dd if=/dev/urandom of="$TMP_DIR/$filename" bs=1M count=$size_mb 2>/dev/null
    local md5_orig=$(md5sum "$TMP_DIR/$filename" | awk '{print $1}')
    
    # Upload
    local up_start=$(date +%s.%N)
    curl -s -X PUT -T "$TMP_DIR/$filename" "$ENDPOINT/$BUCKET/$filename" > /dev/null
    local up_end=$(date +%s.%N)
    local up_time=$(echo "$up_end - $up_start" | bc)
    local up_mbps=$(echo "scale=2; $size_mb / $up_time" | bc)
    
    # Download
    local down_start=$(date +%s.%N)
    curl -s "$ENDPOINT/$BUCKET/$filename" -o "$TMP_DIR/down_$filename"
    local down_end=$(date +%s.%N)
    local down_time=$(echo "$down_end - $down_start" | bc)
    local down_mbps=$(echo "scale=2; $size_mb / $down_time" | bc)
    
    # Verify
    local md5_down=$(md5sum "$TMP_DIR/down_$filename" | awk '{print $1}')
    local md5_status="✗"
    [ "$md5_orig" = "$md5_down" ] && md5_status="✓"
    
    printf "  Upload:   %6.3fs  %8.2f MB/s\n" "$up_time" "$up_mbps"
    printf "  Download: %6.3fs  %8.2f MB/s\n" "$down_time" "$down_mbps"
    printf "  MD5:      %s\n" "$md5_status"
    echo ""
}

# Test various sizes
test_file 1
test_file 2
test_file 5
test_file 10
test_file 20

echo "========================================"
echo "Parallel RPC Activity (last 10 ops)"
echo "========================================"
grep "Parallel write:" /tmp/buckets-6node-node1.log 2>/dev/null | tail -10 | \
    sed 's/.*INFO : //' || echo "No parallel writes logged"
echo ""
grep "Parallel read:" /tmp/buckets-6node-node1.log 2>/dev/null | tail -10 | \
    grep "chunks read" | sed 's/.*INFO : //' || echo "No parallel reads logged"

echo ""
echo "========================================"
echo "RPC Distribution Check"
echo "========================================"
echo "Node 4 RPC calls: $(grep -c "storage.writeChunk" /tmp/buckets-6node-node4.log 2>/dev/null || echo 0)"
echo "Node 5 RPC calls: $(grep -c "storage.writeChunk" /tmp/buckets-6node-node5.log 2>/dev/null || echo 0)"
echo "Node 6 RPC calls: $(grep -c "storage.writeChunk" /tmp/buckets-6node-node6.log 2>/dev/null || echo 0)"

# Cleanup
rm -rf "$TMP_DIR"
echo ""
echo "✓ Benchmark complete"
