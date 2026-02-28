#!/bin/bash
# Start 6-node cluster for testing parallel RPC

echo "=== Starting 6-Node Buckets Cluster ==="
echo ""

# Clean up old processes
echo "Cleaning up old processes..."
pkill -f "buckets.*900[1-6]" 2>/dev/null
sleep 1

# Clean up old data
echo "Cleaning up old data..."
rm -rf /tmp/buckets-6node
sleep 1

# Create directories for each node
for i in {1..6}; do
    mkdir -p /tmp/buckets-6node/node$i/disk{1..4}
    echo "Created directories for node$i"
done

echo ""
echo "=== Starting nodes ==="
echo ""

# Start each node in the background using config files
for i in {1..6}; do
    PORT=$((9000 + i))
    CONFIG="config/node$i.json"
    
    # Check if config exists, if not skip this node
    if [ ! -f "$CONFIG" ]; then
        echo "Skipping node$i - config file $CONFIG not found"
        continue
    fi
    
    echo "Starting node$i on port $PORT (config: $CONFIG)..."
    ./bin/buckets server --config $CONFIG \
        > /tmp/buckets-node$i.log 2>&1 &
    
    echo "  Node$i PID: $!"
    sleep 0.5
done

echo ""
echo "=== Cluster Status ==="
echo ""
echo "All 6 nodes started. Logs:"
for i in {1..6}; do
    echo "  Node$i: /tmp/buckets-node$i.log"
done

echo ""
echo "Waiting for nodes to initialize (5 seconds)..."
sleep 5

echo ""
echo "=== Checking node health ==="
for i in {1..6}; do
    PORT=$((9000 + i))
    if curl -s http://localhost:$PORT/ >/dev/null 2>&1; then
        echo "✓ Node$i (port $PORT) is responding"
    else
        echo "✗ Node$i (port $PORT) is NOT responding"
    fi
done

echo ""
echo "=== Cluster ready for testing! ==="
echo ""
echo "To upload a test file:"
echo "  curl -X PUT -T testfile.bin http://localhost:9001/mybucket/testfile.bin"
echo ""
echo "To stop the cluster:"
echo "  pkill -f 'buckets.*900[1-6]'"
echo ""
