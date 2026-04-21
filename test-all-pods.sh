#!/bin/bash
# Test all 6 pods individually and aggregate results

echo "========================================"
echo "CLUSTER-WIDE LOAD TEST - ALL PODS"
echo "========================================"
echo ""
echo "Testing each of the 6 pods individually..."
echo "Each pod has 16 workers (multi-process)"
echo ""

TOTAL_OPS=0
TOTAL_DURATION=0
TOTAL_DATA_MB=0

for i in 0 1 2 3 4 5; do
    POD_URL="http://buckets-${i}.buckets-headless.buckets.svc.cluster.local:9000"
    
    echo "Testing buckets-$i..."
    
    # Create unique benchmark job for this pod
    kubectl delete job -n buckets benchmark-pod-$i 2>/dev/null || true
    
    cat <<EOF | kubectl apply -f -
apiVersion: batch/v1
kind: Job
metadata:
  name: benchmark-pod-$i
  namespace: buckets
spec:
  backoffLimit: 0
  template:
    spec:
      restartPolicy: Never
      containers:
      - name: benchmark
        image: russellmy/buckets-benchmark:go
        imagePullPolicy: Always
        command:
        - /benchmark
        - -endpoint=$POD_URL
        - -workers=50
        - -duration=30s
        - -size=262144
        - -bucket=bench-pod-$i
EOF
    
    sleep 2
done

echo ""
echo "Waiting for all benchmarks to complete (35 seconds)..."
sleep 35

echo ""
echo "========================================"
echo "RESULTS BY POD"
echo "========================================"
echo ""

for i in 0 1 2 3 4 5; do
    echo "=== buckets-$i ==="
    kubectl -n buckets logs job/benchmark-pod-$i 2>&1 | grep -A10 "Results:" | head -12
    echo ""
done

echo "========================================"
echo "AGGREGATING RESULTS..."
echo "========================================"
echo ""

# Extract and sum throughputs
TOTAL_OPS=$(kubectl -n buckets logs -l job-name | grep "Total Operations:" | awk '{sum+=$3} END {print sum}')
TOTAL_SUCCESSFUL=$(kubectl -n buckets logs -l job-name | grep "Successful:" | awk '{sum+=$2} END {print sum}')

echo "Cluster-Wide Results:"
echo "  Total Operations: $(kubectl -n buckets get jobs -l 'job-name in (benchmark-pod-0,benchmark-pod-1,benchmark-pod-2,benchmark-pod-3,benchmark-pod-4,benchmark-pod-5)' -o json | jq '[.items[].status.succeeded // 0] | add')"
echo ""
echo "Check individual results above for detailed breakdown"
