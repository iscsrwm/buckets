#!/bin/bash
# Start 6-node unified cluster with distributed storage

echo "=== Starting 6-Node Unified Cluster ==="
echo ""

# Stop old processes
pkill -f "buckets.*900[1-6]" 2>/dev/null
sleep 2

# Start each node
for i in {1..6}; do
    PORT=$((9000 + i))
    CONFIG="config/6node-unified-node$i.json"
    LOG="/tmp/buckets-6node-node$i.log"
    
    echo "Starting node$i on port $PORT..."
    ./bin/buckets server --config $CONFIG > $LOG 2>&1 &
    PID=$!
    echo "  Node$i PID: $PID (log: $LOG)"
    sleep 0.5
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
        tail -5 /tmp/buckets-6node-node$i.log
    fi
done

echo ""
echo "=== Cluster ready! ==="
echo ""
echo "Test with:"
echo "  dd if=/dev/urandom of=/tmp/test.bin bs=1M count=2"
echo "  time curl -X PUT -T /tmp/test.bin http://localhost:9001/bucket/test.bin"
echo "  curl http://localhost:9001/bucket/test.bin | md5sum"
echo ""
echo "Stop cluster:"
echo "  pkill -f 'buckets.*900[1-6]'"
echo ""
