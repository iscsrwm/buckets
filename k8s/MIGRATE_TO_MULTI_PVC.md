# Migration to Multi-PVC Architecture

## Problem
The initial deployment used a single PVC per pod with 4 logical "disk" directories:
- 1 PVC → 1 physical volume → 4 directories (/data/disk1-4)
- No real disk redundancy or failure isolation

## Solution
Use 4 separate PVCs per pod for true multi-disk architecture:
- 4 PVCs per pod → 4 physical volumes → 4 mount points
- Proper failure isolation and performance distribution
- Each disk can fail independently

## Migration Steps

**WARNING: This will destroy all existing data!**

### 1. Delete existing StatefulSet and PVCs
```bash
# Delete StatefulSet (keeps PVCs by default)
kubectl delete statefulset buckets -n buckets

# Delete all existing PVCs (data will be lost!)
kubectl delete pvc -n buckets --all

# Verify deletion
kubectl get pvc -n buckets
```

### 2. Apply updated StatefulSet
```bash
# Apply the new multi-PVC configuration
kubectl apply -f k8s/statefulset.yaml

# Watch pods come up
kubectl get pods -n buckets -w
```

### 3. Verify new PVC layout
```bash
# Should show 24 PVCs total (6 pods × 4 disks)
kubectl get pvc -n buckets

# Expected output:
# disk1-buckets-0, disk2-buckets-0, disk3-buckets-0, disk4-buckets-0
# disk1-buckets-1, disk2-buckets-1, disk3-buckets-1, disk4-buckets-1
# ... (same for buckets-2 through buckets-5)
```

## What Changed

### Before (Single PVC):
```yaml
volumeClaimTemplates:
  - metadata:
      name: data
    spec:
      storage: 20Gi

volumeMounts:
  - name: data
    mountPath: /data
```

### After (4 PVCs):
```yaml
volumeClaimTemplates:
  - metadata:
      name: disk1
    spec:
      storage: 20Gi
  - metadata:
      name: disk2
    spec:
      storage: 20Gi
  - metadata:
      name: disk3
    spec:
      storage: 20Gi
  - metadata:
      name: disk4
    spec:
      storage: 20Gi

volumeMounts:
  - name: disk1
    mountPath: /data/disk1
  - name: disk2
    mountPath: /data/disk2
  - name: disk3
    mountPath: /data/disk3
  - name: disk4
    mountPath: /data/disk4
```

## Benefits

1. **True Multi-Disk Architecture**: Each disk is a separate physical volume
2. **Failure Isolation**: One disk failure doesn't affect others
3. **Performance**: Better I/O distribution across separate volumes
4. **Matches Design**: Aligns with MinIO-style erasure coding architecture
5. **Testing**: Can test disk failure scenarios properly

## Storage Requirements

- **Before**: 6 pods × 1 disk × 20GB = 120GB total
- **After**: 6 pods × 4 disks × 20GB = 480GB total

Adjust `storage:` value in volumeClaimTemplates if needed.
