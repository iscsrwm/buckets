# Buckets Kubernetes Quick Start

Deploy Buckets distributed storage to Kubernetes in minutes.

## TL;DR

```bash
# 1. Build and deploy
cd k8s
./deploy.sh deploy

# 2. Wait for pods (1-2 minutes)
./deploy.sh status

# 3. Test it works
./deploy.sh test

# Done! 🎉
```

## What Gets Deployed

- ✅ **6 pods** spread across different Kubernetes nodes
- ✅ **24 virtual disks** (4 per pod)
- ✅ **Erasure coding** 8+4 (survives 4 disk failures)
- ✅ **100Gi storage** per pod (600Gi total)
- ✅ **LoadBalancer** for external S3 access
- ✅ **Optimized performance** (512 RPC, 64 threads)

## Prerequisites

- Kubernetes cluster (minikube, kind, GKE, EKS, AKS, etc.)
- kubectl configured
- Docker installed
- At least 6 nodes recommended (for distribution)

## Deployment Options

### Option 1: Quick Deploy (Default)

```bash
./deploy.sh deploy
```

Uses default settings:
- Registry: localhost:5000
- Namespace: buckets
- Replicas: 6
- Storage: 100Gi per pod

### Option 2: Custom Registry

```bash
# Use Docker Hub
./deploy.sh deploy --registry docker.io/yourusername

# Use GCR
./deploy.sh deploy --registry gcr.io/your-project

# Use ECR
./deploy.sh deploy --registry 123456789.dkr.ecr.us-east-1.amazonaws.com
```

### Option 3: Manual Steps

```bash
# 1. Build image
./deploy.sh build

# 2. Push to registry (if using remote cluster)
export DOCKER_REGISTRY=your-registry.com
./deploy.sh push

# 3. Update image in statefulset.yaml
# Edit: image: your-registry.com/buckets:latest

# 4. Deploy
kubectl apply -f namespace.yaml
kubectl apply -f configmap.yaml
kubectl apply -f service.yaml
kubectl apply -f statefulset.yaml

# 5. Wait for ready
kubectl get pods -n buckets -w
```

## Accessing the Cluster

### Get Endpoint

```bash
./deploy.sh status

# Or manually:
kubectl get svc buckets-lb -n buckets
```

### Using mc (MinIO Client)

```bash
# Get endpoint IP
ENDPOINT=$(kubectl get svc buckets-lb -n buckets -o jsonpath='{.status.loadBalancer.ingress[0].ip}')

# Configure
mc alias set k8s http://${ENDPOINT}:9000 minioadmin minioadmin

# Test
mc mb k8s/my-bucket
echo "Hello Kubernetes!" | mc pipe k8s/my-bucket/hello.txt
mc cat k8s/my-bucket/hello.txt
```

### Using AWS CLI

```bash
export AWS_ACCESS_KEY_ID=minioadmin
export AWS_SECRET_ACCESS_KEY=minioadmin
ENDPOINT=$(kubectl get svc buckets-lb -n buckets -o jsonpath='{.status.loadBalancer.ingress[0].ip}')

aws s3 mb s3://my-bucket --endpoint-url http://${ENDPOINT}:9000
aws s3 cp file.txt s3://my-bucket/ --endpoint-url http://${ENDPOINT}:9000
aws s3 ls s3://my-bucket/ --endpoint-url http://${ENDPOINT}:9000
```

### Using Port Forward (Local Testing)

```bash
# Forward port
kubectl port-forward -n buckets svc/buckets-lb 9000:9000

# In another terminal
mc alias set k8s http://localhost:9000 minioadmin minioadmin
mc mb k8s/test
```

## Common Operations

### Check Status

```bash
./deploy.sh status
```

### View Logs

```bash
# All pods
./deploy.sh logs

# Specific pod
kubectl logs -n buckets buckets-0 -f

# Previous instance (after crash)
kubectl logs -n buckets buckets-0 --previous
```

### Restart Pods

```bash
# Rolling restart (no downtime)
kubectl rollout restart statefulset buckets -n buckets

# Delete specific pod (auto-recreated)
kubectl delete pod buckets-0 -n buckets
```

### Scale Up/Down

```bash
# Edit replicas
kubectl scale statefulset buckets -n buckets --replicas=9

# Or edit statefulset
kubectl edit statefulset buckets -n buckets
```

### Upgrade

```bash
# After making config changes
./deploy.sh upgrade

# Or manually
kubectl apply -f configmap.yaml
kubectl apply -f statefulset.yaml
kubectl rollout restart statefulset buckets -n buckets
```

## Testing Performance

### Basic Test

```bash
./deploy.sh test
```

### Upload Performance

```bash
# Create test file
dd if=/dev/urandom of=/tmp/test-1mb.bin bs=1M count=1

# Port forward
kubectl port-forward -n buckets svc/buckets-lb 9000:9000 &

# Configure mc
mc alias set k8s http://localhost:9000 minioadmin minioadmin

# Upload 100 files
mc mb k8s/perf-test
time for i in {1..100}; do
  mc cp /tmp/test-1mb.bin k8s/perf-test/file-$i.bin
done
```

### Expected Performance (on real distributed nodes)

- **Concurrent upload**: 1,500-3,000 ops/sec
- **Concurrent download**: 800-1,600 ops/sec
- **Bandwidth**: 375-750 MB/s aggregate

## Troubleshooting

### Pods Not Starting

```bash
# Check pod status
kubectl describe pod buckets-0 -n buckets

# Common fixes:
# - Not enough nodes: Need 6 nodes for anti-affinity
# - Storage issue: Check PVC status
# - Image not found: Build and push image
```

### PVCs Pending

```bash
# Check PVCs
kubectl get pvc -n buckets

# Check storage class exists
kubectl get storageclass

# Fix: Update storageClassName in statefulset.yaml
```

### Cannot Access LoadBalancer

```bash
# Check service
kubectl get svc buckets-lb -n buckets

# If EXTERNAL-IP is <pending>:
# - Cloud provider may not support LoadBalancer
# - Use NodePort or port-forward instead

# Change to NodePort:
kubectl patch svc buckets-lb -n buckets -p '{"spec":{"type":"NodePort"}}'
kubectl get svc buckets-lb -n buckets  # Note the NodePort
```

### Format Errors

```bash
# Manually format disks
kubectl exec -it -n buckets buckets-0 -- /app/bin/buckets format --config /etc/buckets/config.json

# Check format status
kubectl exec -n buckets buckets-0 -- cat /data/disk1/.buckets/format.json
```

## Cleanup

### Remove Deployment (Keep Data)

```bash
./deploy.sh cleanup
```

### Remove Everything (Delete Data)

```bash
./deploy.sh destroy
```

### Manual Cleanup

```bash
# Remove deployment
kubectl delete statefulset buckets -n buckets
kubectl delete svc buckets-lb buckets-headless -n buckets
kubectl delete configmap buckets-config -n buckets

# Remove data (DESTRUCTIVE!)
kubectl delete pvc -n buckets --all

# Remove namespace
kubectl delete namespace buckets
```

## Configuration

### Storage Size

Edit `k8s/statefulset.yaml`:

```yaml
volumeClaimTemplates:
  - metadata:
      name: data
    spec:
      resources:
        requests:
          storage: 100Gi  # Change this
```

### Storage Class

```yaml
volumeClaimTemplates:
  - metadata:
      name: data
    spec:
      storageClassName: fast  # Options: standard, fast, local-path, etc.
```

### Resources

```yaml
resources:
  requests:
    cpu: 500m      # Minimum
    memory: 1Gi
  limits:
    cpu: 2000m     # Maximum
    memory: 4Gi
```

### Number of Nodes

1. Update replicas in `statefulset.yaml`
2. Update nodes list in `configmap.yaml`
3. Update erasure sets configuration
4. Redeploy

## Security

### Change Default Credentials

```bash
# Create new credentials
kubectl exec -n buckets buckets-0 -- /app/bin/buckets creds create

# It will output:
# Access Key: <random-key>
# Secret Key: <random-secret>

# Disable default minioadmin
kubectl exec -n buckets buckets-0 -- /app/bin/buckets creds disable minioadmin
```

### Enable TLS

Use an Ingress controller with TLS:

```yaml
apiVersion: networking.k8s.io/v1
kind: Ingress
metadata:
  name: buckets-ingress
  namespace: buckets
  annotations:
    cert-manager.io/cluster-issuer: letsencrypt-prod
spec:
  tls:
    - hosts:
        - s3.example.com
      secretName: buckets-tls
  rules:
    - host: s3.example.com
      http:
        paths:
          - path: /
            pathType: Prefix
            backend:
              service:
                name: buckets-lb
                port:
                  number: 9000
```

## Support

- Full guide: `k8s/README.md`
- Performance docs: `docs/BASELINE_METRICS.md`
- Project docs: `docs/`

## What's Next?

1. ✅ Deploy to production cluster
2. ✅ Set up monitoring (Prometheus + Grafana)
3. ✅ Configure backup strategy
4. ✅ Test disaster recovery
5. ✅ Performance tuning for your workload
