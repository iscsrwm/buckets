#!/bin/bash
# Test script to verify socket buffer optimization

set -e

echo "=== Socket Buffer Optimization Test ==="
echo ""

# Start server in background
echo "Starting server..."
./bin/buckets server --port 9001 --config config/node1.json > /tmp/buckets-test.log 2>&1 &
SERVER_PID=$!

# Wait for server to start
sleep 2

# Check if server is running
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "❌ Server failed to start"
    cat /tmp/buckets-test.log
    exit 1
fi

echo "✅ Server started (PID: $SERVER_PID)"

# Make a simple request to establish a connection
echo ""
echo "Testing connection..."
curl -X PUT http://localhost:9001/test-bucket -s -o /dev/null -w "HTTP Status: %{http_code}\n" || true

# Check server logs for socket buffer messages
echo ""
echo "Checking socket buffer configuration in logs..."
if grep -q "SO_SNDBUF\|SO_RCVBUF" /tmp/buckets-test.log; then
    echo "⚠️  Found socket buffer warnings (check if buffers were set successfully)"
    grep "SO_SNDBUF\|SO_RCVBUF" /tmp/buckets-test.log
else
    echo "✅ No socket buffer errors (buffers likely set successfully)"
fi

# Get process FD info
echo ""
echo "Server file descriptors:"
ls -la /proc/$SERVER_PID/fd 2>/dev/null | grep socket | head -5 || echo "Could not read FDs"

# Cleanup
echo ""
echo "Stopping server..."
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "=== Test Complete ==="
echo "Check /tmp/buckets-test.log for detailed server output"
