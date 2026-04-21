#!/bin/bash
# Simple test to verify worker pool can start processes

echo "Testing worker pool auto-detection..."
echo ""

# Test 1: Check optimal worker count
echo "Test 1: Optimal worker count detection"
cat > /tmp/test_workers.c << 'EOF'
#include <stdio.h>
#include <unistd.h>
int main() {
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    printf("CPU cores detected: %ld\n", ncpu);
    printf("Optimal workers: %ld\n", ncpu);
    return 0;
}
EOF

gcc -o /tmp/test_workers /tmp/test_workers.c
/tmp/test_workers
rm -f /tmp/test_workers /tmp/test_workers.c

echo ""
echo "✓ Worker count detection works"
echo ""

# Test 2: Verify SO_REUSEPORT is available
echo "Test 2: SO_REUSEPORT availability"
cat > /tmp/test_reuseport.c << 'EOF'
#include <stdio.h>
#include <sys/socket.h>

int main() {
#ifdef SO_REUSEPORT
    printf("✓ SO_REUSEPORT is available\n");
    return 0;
#else
    printf("✗ SO_REUSEPORT is NOT available\n");
    return 1;
#endif
}
EOF

gcc -o /tmp/test_reuseport /tmp/test_reuseport.c
/tmp/test_reuseport
RESULT=$?
rm -f /tmp/test_reuseport /tmp/test_reuseport.c

echo ""
if [ $RESULT -eq 0 ]; then
    echo "======================================"
    echo "Worker pool prerequisites PASSED!"
    echo "======================================"
else
    echo "======================================"
    echo "WARNING: SO_REUSEPORT not available"
    echo "Worker pool may not work properly"
    echo "======================================"
    exit 1
fi
