# Buckets Kubernetes Deployment

Deploy Buckets distributed object storage across a Kubernetes cluster with true multi-node distribution.

## Architecture

- **6 StatefulSet pods** distributed across different Kubernetes nodes
- **2 erasure sets** (3 nodes each) with 8+4 erasure coding
- **4 virtual disks per pod** (24 total disks)
- **Persistent volumes** for durable storage
- **Headless service** for inter-node RPC communication
- **LoadBalancer service** for external S3 API access

## Prerequisites

1. **Kubernetes cluster** with at least 6 nodes (for true distribution)
2. **kubectl** configured to access your cluster
3. **Storage provisioner** (e.g., local-path, EBS, Ceph, etc.)
4. **Docker** or compatible container runtime

## Quick Start

### 1. Use Pre-built Docker Image

The official Buckets image is available on Docker Hub:

```bash
# Pull the image (optional - Kubernetes will pull automatically)
docker pull russellmy/buckets:latest
```

**Alternatively, build your own image:**

```bash
# From the project root directory
docker build -f k8s/Dockerfile -t buckets:latest .

# Tag for your registry
docker tag buckets:latest your-registry/buckets:latest
docker push your-registry/buckets:latest

# Update k8s/statefulset.yaml to use your image
```

### 2. Deploy to Kubernetes

```bash
# Create namespace
kubectl apply -f k8s/namespace.yaml

# Create ConfigMap with cluster configuration
kubectl apply -f k8s/configmap.yaml

# Create services
kubectl apply -f k8s/service.yaml

# Deploy StatefulSet (this will create 6 pods)
kubectl apply -f k8s/statefulset.yaml
```

### 3. Wait for Pods to be Ready

```bash
# Watch pod status
kubectl get pods -n buckets -w

# Expected output after ~1-2 minutes:
# buckets-0   1/1   Running   0   60s
# buckets-1   1/1   Running   0   60s
# buckets-2   1/1   Running   0   60s
# buckets-3   1/1   Running   0   60s
# buckets-4   1/1   Running   0   60s
# buckets-5   1/1   Running   0   60s
```

### 5. Get LoadBalancer Endpoint

```bash
# Get the external IP/hostname
kubectl get svc buckets-lb -n buckets

# Example output:
# NAME         TYPE           CLUSTER-IP      EXTERNAL-IP      PORT(S)          AGE
# buckets-lb   LoadBalancer   10.96.123.45    203.0.113.10     9000:30123/TCP   2m
```

## Configuration

### Storage Class

Update `storageClassName` in `k8s/statefulset.yaml` based on your cluster:

```yaml
volumeClaimTemplates:
  - metadata:
      name: data
    spec:
      storageClassName: standard  # Options: standard, fast, local-path, etc.
      resources:
        requests:
          storage: 100Gi  # Adjust size as needed
```

### Resource Limits

Adjust CPU/memory based on workload in `k8s/statefulset.yaml`:

```yaml
resources:
  requests:
    cpu: 500m      # Minimum guaranteed
    memory: 1Gi
  limits:
    cpu: 2000m     # Maximum allowed
    memory: 4Gi
```

### Thread Pool Size

Already optimized for high concurrency (64 threads):

```yaml
env:
  - name: UV_THREADPOOL_SIZE
    value: "64"
```

### Scaling

To change the number of nodes, update both:

1. **StatefulSet replicas** in `k8s/statefulset.yaml`:
   ```yaml
   spec:
     replicas: 6  # Change this
   ```

2. **Cluster nodes** in `k8s/configmap.yaml`:
   - Add/remove node entries
   - Adjust erasure sets accordingly

## Usage

### Configure S3 Client (mc)

```bash
# Get LoadBalancer IP
ENDPOINT=$(kubectl get svc buckets-lb -n buckets -o jsonpath='{.status.loadBalancer.ingress[0].ip}')

# Configure mc client
mc alias set k8s-buckets http://${ENDPOINT}:9000 minioadmin minioadmin

# Test
mc mb k8s-buckets/test-bucket
echo "Hello from Kubernetes!" | mc pipe k8s-buckets/test-bucket/hello.txt
mc cat k8s-buckets/test-bucket/hello.txt
```

### AWS CLI

```bash
export AWS_ACCESS_KEY_ID=minioadmin
export AWS_SECRET_ACCESS_KEY=minioadmin
export ENDPOINT=$(kubectl get svc buckets-lb -n buckets -o jsonpath='{.status.loadBalancer.ingress[0].ip}')

aws s3 mb s3://test-bucket --endpoint-url http://${ENDPOINT}:9000
aws s3 cp file.txt s3://test-bucket/ --endpoint-url http://${ENDPOINT}:9000
```

## Monitoring

### Check Pod Health

```bash
# All pods status
kubectl get pods -n buckets

# Logs from specific pod
kubectl logs -n buckets buckets-0

# Follow logs
kubectl logs -n buckets buckets-0 -f

# Check all pods logs
for i in {0..5}; do
  echo "=== buckets-$i ==="
  kubectl logs -n buckets buckets-$i --tail=20
done
```

### Performance Testing

```bash
# Port-forward to access directly
kubectl port-forward -n buckets svc/buckets-lb 9000:9000

# In another terminal, configure mc
mc alias set k8s-local http://localhost:9000 minioadmin minioadmin

# Run performance test
mc mb k8s-local/perf-test
dd if=/dev/urandom of=/tmp/test-256kb.bin bs=256k count=1

# Upload test
time for i in {1..100}; do
  mc cp /tmp/test-256kb.bin k8s-local/perf-test/test-$i.bin
done
```

### Verify Distribution

```bash
# Check that pods are on different nodes
kubectl get pods -n buckets -o wide

# Should see different NODE values:
# NAME        READY   STATUS    NODE
# buckets-0   1/1     Running   worker-1
# buckets-1   1/1     Running   worker-2
# buckets-2   1/1     Running   worker-3
# buckets-3   1/1     Running   worker-4
# buckets-4   1/1     Running   worker-5
# buckets-5   1/1     Running   worker-6
```

## Troubleshooting

### Pods Not Starting

```bash
# Check pod events
kubectl describe pod buckets-0 -n buckets

# Common issues:
# - Insufficient nodes: Need 6 nodes for anti-affinity
# - Storage not available: Check PVC status
# - Image pull errors: Verify image exists
```

### Storage Issues

```bash
# Check PersistentVolumeClaims
kubectl get pvc -n buckets

# Should see 6 PVCs (one per pod):
# NAME              STATUS   VOLUME     CAPACITY   STORAGECLASS
# data-buckets-0    Bound    pvc-xxx    100Gi      standard
# data-buckets-1    Bound    pvc-yyy    100Gi      standard
# ...
```

### Network Connectivity

```bash
# Test inter-pod communication
kubectl exec -n buckets buckets-0 -- curl http://buckets-1.buckets-headless.buckets.svc.cluster.local:9000/

# Should return HTML or "It Works!"
```

### Format Issues

```bash
# If format fails, manually format
kubectl exec -it -n buckets buckets-0 -- /app/bin/buckets format --config /etc/buckets/config.json

# Check format status
kubectl exec -n buckets buckets-0 -- cat /data/disk1/.buckets/format.json
```

## Cleanup

### Remove Everything

```bash
# Delete all resources
kubectl delete -f k8s/statefulset.yaml
kubectl delete -f k8s/service.yaml
kubectl delete -f k8s/configmap.yaml

# Delete PVCs (CAUTION: This deletes data!)
kubectl delete pvc -n buckets --all

# Delete namespace
kubectl delete -f k8s/namespace.yaml
```

### Keep Data, Restart Pods

```bash
# Just restart StatefulSet
kubectl rollout restart statefulset buckets -n buckets
```

## Performance Optimization

### Current Optimizations

✅ **RPC Concurrency**: 512 concurrent RPC calls (optimized for high throughput)  
✅ **Thread Pool**: 64 libuv threads (optimized for I/O parallelism)  
✅ **Pod Anti-Affinity**: Spreads pods across different nodes  
✅ **Parallel Startup**: Pods start simultaneously  

### Expected Performance

On Kubernetes with distributed nodes (each pod on separate physical disk):

- **Concurrent upload**: 1,500-3,000 ops/sec (10-20x localhost)
- **Concurrent download**: 800-1,600 ops/sec
- **Bandwidth**: 375-750 MB/s aggregate

### Scaling Up

For higher performance:

1. **Add more nodes**: Scale replicas beyond 6
2. **Faster storage**: Use NVMe-backed storage classes
3. **Network optimization**: Use CNI with hardware offload
4. **Resource allocation**: Increase CPU/memory limits

## Security Considerations

### Current Setup

- ⚠️  **Default credentials**: minioadmin/minioadmin (change in production)
- ✅  **Non-root container**: Runs as user `buckets` (UID 1000)
- ⚠️  **HTTP only**: No TLS (use ingress/service mesh for encryption)

### Production Recommendations

1. **Change default credentials**:
   ```bash
   kubectl exec -n buckets buckets-0 -- /app/bin/buckets creds create
   ```

2. **Enable TLS**: Use Ingress with cert-manager
3. **Network policies**: Restrict pod-to-pod communication
4. **RBAC**: Limit service account permissions
5. **Pod Security Standards**: Apply restricted policies

## Architecture Details

### Pod Layout

```
Kubernetes Cluster
├── buckets-0 (node-0) → Erasure Set 0
│   ├── disk1, disk2, disk3, disk4
│   └── PersistentVolume: 100Gi
├── buckets-1 (node-1) → Erasure Set 0
├── buckets-2 (node-2) → Erasure Set 0
├── buckets-3 (node-3) → Erasure Set 1
├── buckets-4 (node-4) → Erasure Set 1
└── buckets-5 (node-5) → Erasure Set 1
```

### Network Topology

```
External Client
      ↓
LoadBalancer (buckets-lb)
      ↓
   Any Pod (round-robin)
      ↓
Distributed RPC → All Pods
      ↓
Persistent Storage (PVCs)
```

### Data Flow

1. **Upload**: Client → LB → Any pod → Erasure encoding → Distributed write to 12 disks
2. **Download**: Client → LB → Any pod → Distributed read from quorum → Decode → Client
3. **List**: Client → LB → Any pod → RPC to all nodes → Merge results → Client

## Support

For issues, refer to:
- Project documentation in `docs/`
- Performance metrics in `docs/BASELINE_METRICS.md`
- Optimization guide in `docs/PERFORMANCE_OPTIMIZATION_SUMMARY.txt`
