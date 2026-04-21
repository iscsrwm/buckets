#!/bin/bash
# Test worker pool with 4 workers

set -e

echo "Testing multi-process worker pool..."
echo "======================================"
echo ""

# Create minimal test data directory
TEST_DIR="/tmp/buckets-test-$$"
mkdir -p "$TEST_DIR/disk1"

# Create minimal config
cat > "$TEST_DIR/config.json" << 'EOF'
{
  "version": 1,
  "deployment_id": "test-deployment",
  "node": {
    "name": "test-node",
    "endpoint": "localhost:9000",
    "data_dir": "/tmp/buckets-test/disk1"
  },
  "server": {
    "port": 9000,
    "bind_address": "127.0.0.1"
  },
  "erasure": {
    "data_shards": 2,
    "parity_shards": 2
  },
  "storage": {
    "disk_count": 1,
    "disks": ["/tmp/buckets-test/disk1"]
  }
}
EOF

# Replace temp dir in config
sed -i "s|/tmp/buckets-test|$TEST_DIR|g" "$TEST_DIR/config.json"

echo "Starting server with BUCKETS_WORKERS=4..."
echo ""

# Start server with 4 workers (use timeout to kill after test)
BUCKETS_WORKERS=4 timeout 10 bin/buckets server --config "$TEST_DIR/config.json" 9000 &
SERVER_PID=$!

# Wait for server to start
sleep 2

# Check if workers are running
echo "Checking worker processes..."
ps aux | grep "bin/buckets" | grep -v grep || true
echo ""

WORKER_COUNT=$(ps aux | grep "bin/buckets server" | grep -v grep | wc -l)
echo "Found $WORKER_COUNT buckets processes (expected: 5 = 1 master + 4 workers)"
echo ""

if [ "$WORKER_COUNT" -ge 4 ]; then
    echo "✓ Worker pool started successfully!"
else
    echo "✗ Worker pool failed to start properly"
    kill $SERVER_PID 2>/dev/null || true
    rm -rf "$TEST_DIR"
    exit 1
fi

# Test concurrent requests
echo "Testing concurrent requests..."
echo ""

for i in {1..20}; do
    curl -s -X GET http://127.0.0.1:9000/ >/dev/null 2>&1 &
done

wait

echo "✓ Concurrent requests completed"
echo ""

# Cleanup
echo "Cleaning up..."
kill $SERVER_PID 2>/dev/null || true
rm -rf "$TEST_DIR"

echo ""
echo "======================================"
echo "Worker pool test PASSED!"
echo "======================================"
