#!/bin/bash

# Buckets Kubernetes Deployment Script
set -e

NAMESPACE="buckets"
IMAGE_NAME="buckets"
IMAGE_TAG="latest"
REGISTRY="${DOCKER_REGISTRY:-localhost:5000}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if kubectl is available
if ! command -v kubectl &> /dev/null; then
    log_error "kubectl not found. Please install kubectl first."
    exit 1
fi

# Check if docker is available
if ! command -v docker &> /dev/null; then
    log_error "docker not found. Please install docker first."
    exit 1
fi

show_usage() {
    cat << EOF
Buckets Kubernetes Deployment Script

Usage: $0 [COMMAND] [OPTIONS]

Commands:
    build       Build Docker image
    push        Push image to registry
    deploy      Deploy to Kubernetes
    upgrade     Upgrade existing deployment
    status      Check deployment status
    logs        Show logs from all pods
    test        Run basic functionality test
    cleanup     Remove all resources (keeps PVCs)
    destroy     Remove everything including data (DESTRUCTIVE!)
    help        Show this help message

Options:
    --registry  Docker registry (default: localhost:5000)
    --namespace Kubernetes namespace (default: buckets)
    --replicas  Number of replicas (default: 6)
    --storage   Storage size per pod (default: 100Gi)

Examples:
    $0 build
    $0 deploy --registry docker.io/myuser
    $0 status
    $0 logs
    $0 cleanup

EOF
}

build_image() {
    log_info "Building Docker image: ${IMAGE_NAME}:${IMAGE_TAG}"
    
    cd "$(dirname "$0")/.."
    docker build -f k8s/Dockerfile -t ${IMAGE_NAME}:${IMAGE_TAG} .
    
    log_success "Image built successfully"
}

push_image() {
    log_info "Tagging and pushing to registry: ${REGISTRY}"
    
    docker tag ${IMAGE_NAME}:${IMAGE_TAG} ${REGISTRY}/${IMAGE_NAME}:${IMAGE_TAG}
    docker push ${REGISTRY}/${IMAGE_NAME}:${IMAGE_TAG}
    
    log_success "Image pushed successfully"
}

deploy_cluster() {
    log_info "Deploying Buckets to Kubernetes"
    
    # Create namespace
    log_info "Creating namespace: ${NAMESPACE}"
    kubectl apply -f k8s/namespace.yaml
    
    # Create ConfigMap
    log_info "Creating ConfigMap"
    kubectl apply -f k8s/configmap.yaml
    
    # Create Services
    log_info "Creating Services"
    kubectl apply -f k8s/service.yaml
    
    # Deploy StatefulSet
    log_info "Deploying StatefulSet"
    kubectl apply -f k8s/statefulset.yaml
    
    log_success "Deployment initiated"
    log_info "Waiting for pods to be ready (this may take 1-2 minutes)..."
    
    # Wait for rollout
    kubectl rollout status statefulset/buckets -n ${NAMESPACE} --timeout=5m || {
        log_warn "Rollout timeout. Check status with: $0 status"
        return 1
    }
    
    log_success "All pods are ready!"
    
    # Show endpoint
    show_endpoint
}

upgrade_deployment() {
    log_info "Upgrading Buckets deployment"
    
    kubectl apply -f k8s/configmap.yaml
    kubectl apply -f k8s/service.yaml
    kubectl apply -f k8s/statefulset.yaml
    
    log_info "Rolling out changes..."
    kubectl rollout restart statefulset/buckets -n ${NAMESPACE}
    kubectl rollout status statefulset/buckets -n ${NAMESPACE} --timeout=5m
    
    log_success "Upgrade complete"
}

show_status() {
    log_info "Deployment Status:"
    echo ""
    
    kubectl get pods -n ${NAMESPACE} -o wide
    echo ""
    
    kubectl get pvc -n ${NAMESPACE}
    echo ""
    
    kubectl get svc -n ${NAMESPACE}
    echo ""
    
    log_info "Pod distribution across nodes:"
    kubectl get pods -n ${NAMESPACE} -o custom-columns=NAME:.metadata.name,NODE:.spec.nodeName --no-headers | sort -k2
}

show_logs() {
    log_info "Fetching logs from all pods..."
    
    for i in {0..5}; do
        POD="buckets-$i"
        echo ""
        echo "=========================================="
        echo "Logs from $POD"
        echo "=========================================="
        kubectl logs -n ${NAMESPACE} ${POD} --tail=50 || log_warn "Pod ${POD} not found"
    done
}

show_endpoint() {
    log_info "Buckets S3 Endpoint:"
    
    EXTERNAL_IP=$(kubectl get svc buckets-lb -n ${NAMESPACE} -o jsonpath='{.status.loadBalancer.ingress[0].ip}' 2>/dev/null)
    EXTERNAL_HOST=$(kubectl get svc buckets-lb -n ${NAMESPACE} -o jsonpath='{.status.loadBalancer.ingress[0].hostname}' 2>/dev/null)
    
    if [ -n "$EXTERNAL_IP" ]; then
        echo ""
        echo "  Endpoint: http://${EXTERNAL_IP}:9000"
        echo ""
        echo "  Configure mc client:"
        echo "    mc alias set k8s-buckets http://${EXTERNAL_IP}:9000 minioadmin minioadmin"
    elif [ -n "$EXTERNAL_HOST" ]; then
        echo ""
        echo "  Endpoint: http://${EXTERNAL_HOST}:9000"
        echo ""
        echo "  Configure mc client:"
        echo "    mc alias set k8s-buckets http://${EXTERNAL_HOST}:9000 minioadmin minioadmin"
    else
        log_warn "LoadBalancer external IP not yet assigned. Run this again in a moment."
        echo ""
        echo "  Or use port-forward:"
        echo "    kubectl port-forward -n ${NAMESPACE} svc/buckets-lb 9000:9000"
        echo "    mc alias set k8s-buckets http://localhost:9000 minioadmin minioadmin"
    fi
}

run_test() {
    log_info "Running basic functionality test"
    
    # Check if mc is installed
    if ! command -v mc &> /dev/null; then
        log_error "mc (MinIO client) not found. Please install it first."
        exit 1
    fi
    
    # Port forward in background
    log_info "Setting up port-forward..."
    kubectl port-forward -n ${NAMESPACE} svc/buckets-lb 9000:9000 &
    PF_PID=$!
    sleep 3
    
    # Configure mc
    log_info "Configuring mc client..."
    mc alias set k8s-test http://localhost:9000 minioadmin minioadmin 2>/dev/null || true
    
    # Test operations
    log_info "Creating test bucket..."
    mc mb k8s-test/test-bucket 2>/dev/null || log_warn "Bucket may already exist"
    
    log_info "Uploading test file..."
    echo "Hello from Kubernetes at $(date)" > /tmp/k8s-test.txt
    mc cp /tmp/k8s-test.txt k8s-test/test-bucket/hello.txt
    
    log_info "Downloading and verifying..."
    mc cat k8s-test/test-bucket/hello.txt
    
    log_info "Listing objects..."
    mc ls k8s-test/test-bucket/
    
    # Cleanup
    kill $PF_PID 2>/dev/null || true
    rm /tmp/k8s-test.txt
    
    log_success "Basic functionality test passed!"
}

cleanup() {
    log_warn "Removing Buckets deployment (PVCs will be kept)"
    read -p "Are you sure? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        log_info "Cleanup cancelled"
        return
    fi
    
    kubectl delete -f k8s/statefulset.yaml --ignore-not-found=true
    kubectl delete -f k8s/service.yaml --ignore-not-found=true
    kubectl delete -f k8s/configmap.yaml --ignore-not-found=true
    
    log_success "Deployment removed (data preserved in PVCs)"
}

destroy() {
    log_error "DESTRUCTIVE: This will delete ALL data including PVCs!"
    read -p "Type 'DELETE ALL DATA' to confirm: " -r
    echo
    if [[ ! $REPLY == "DELETE ALL DATA" ]]; then
        log_info "Destroy cancelled"
        return
    fi
    
    log_warn "Deleting everything..."
    kubectl delete -f k8s/statefulset.yaml --ignore-not-found=true
    kubectl delete -f k8s/service.yaml --ignore-not-found=true
    kubectl delete -f k8s/configmap.yaml --ignore-not-found=true
    kubectl delete pvc -n ${NAMESPACE} --all
    kubectl delete namespace ${NAMESPACE} --ignore-not-found=true
    
    log_success "All resources and data destroyed"
}

# Parse arguments
COMMAND=$1
shift || true

while [[ $# -gt 0 ]]; do
    case $1 in
        --registry)
            REGISTRY="$2"
            shift 2
            ;;
        --namespace)
            NAMESPACE="$2"
            shift 2
            ;;
        *)
            log_error "Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
done

# Execute command
case $COMMAND in
    build)
        build_image
        ;;
    push)
        push_image
        ;;
    deploy)
        build_image
        deploy_cluster
        ;;
    upgrade)
        upgrade_deployment
        ;;
    status)
        show_status
        show_endpoint
        ;;
    logs)
        show_logs
        ;;
    test)
        run_test
        ;;
    cleanup)
        cleanup
        ;;
    destroy)
        destroy
        ;;
    help|"")
        show_usage
        ;;
    *)
        log_error "Unknown command: $COMMAND"
        show_usage
        exit 1
        ;;
esac
