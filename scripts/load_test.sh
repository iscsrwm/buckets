#!/bin/bash
#
# S3 Load Test Script - Bounded Write/Delete Test
# Tests throughput and resilience while keeping disk usage bounded
#

# Configuration
NODES=("127.0.0.1:9001" "127.0.0.1:9002" "127.0.0.1:9003" "127.0.0.1:9004" "127.0.0.1:9005" "127.0.0.1:9006")
BUCKET="loadtest"
DURATION=${DURATION:-60}
CONCURRENCY=${CONCURRENCY:-10}
OBJECT_SIZE=${OBJECT_SIZE:-65536}
MAX_OBJECTS=${MAX_OBJECTS:-500}
DELETE_RATIO=${DELETE_RATIO:-50}
WARMUP_OBJECTS=${WARMUP_OBJECTS:-100}

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[OK]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Get a random node
get_node() {
    local idx=$((RANDOM % ${#NODES[@]}))
    echo "${NODES[$idx]}"
}

# Create test bucket
setup_bucket() {
    log_info "Creating test bucket '$BUCKET'..."
    for node in "${NODES[@]}"; do
        curl -s -X PUT "http://$node/$BUCKET" -o /dev/null 2>/dev/null || true
    done
    log_success "Bucket ready"
}

# Cleanup bucket
cleanup_bucket() {
    log_info "Cleaning up test bucket..."
    local node="${NODES[0]}"
    local objects=$(curl -s "http://$node/$BUCKET" 2>/dev/null | grep -oP '(?<=<Key>)[^<]+' || true)
    local count=0
    for key in $objects; do
        curl -s -X DELETE "http://$node/$BUCKET/$key" -o /dev/null 2>/dev/null &
        ((count++)) || true
        if ((count % 50 == 0)); then
            wait
        fi
    done
    wait
    log_success "Cleaned up $count objects"
}

# Worker function - runs in subshell
run_worker() {
    local worker_id=$1
    local duration=$2
    local output_file=$3
    local obj_size=$4
    local max_objs=$5
    local del_ratio=$6
    local warmup=$7
    
    local end_time=$(($(date +%s) + duration))
    local objects=()
    local obj_count=0
    local warmup_done=0
    local per_worker_warmup=$((warmup / CONCURRENCY + 1))
    local per_worker_max=$((max_objs / CONCURRENCY + 1))
    
    # Generate test data once
    local test_data=$(dd if=/dev/urandom bs=$obj_size count=1 2>/dev/null | base64 | head -c $obj_size)
    
    while true; do
        local now=$(date +%s)
        if ((now >= end_time)); then
            break
        fi
        
        local node="${NODES[$((RANDOM % ${#NODES[@]}))]}"
        
        # Warmup phase
        if ((warmup_done == 0)); then
            local key="w${worker_id}_${RANDOM}_${now}"
            local code=$(curl -s -o /dev/null -w "%{http_code}" \
                -X PUT "http://$node/$BUCKET/$key" \
                -H "Content-Type: application/octet-stream" \
                --data-binary "$test_data" 2>/dev/null)
            
            if [[ "$code" == "200" ]] || [[ "$code" == "201" ]]; then
                echo "PUT_OK" >> "$output_file"
                objects+=("$key")
                ((obj_count++)) || true
            else
                echo "PUT_FAIL" >> "$output_file"
            fi
            
            if ((obj_count >= per_worker_warmup)); then
                warmup_done=1
            fi
            continue
        fi
        
        # Main phase
        local op=$((RANDOM % 100))
        
        if ((op < del_ratio)) && ((obj_count > 5)); then
            # DELETE
            local idx=$((RANDOM % obj_count))
            local key="${objects[$idx]}"
            local code=$(curl -s -o /dev/null -w "%{http_code}" \
                -X DELETE "http://$node/$BUCKET/$key" 2>/dev/null)
            
            if [[ "$code" == "200" ]] || [[ "$code" == "204" ]]; then
                echo "DEL_OK" >> "$output_file"
                # Remove from array
                objects=("${objects[@]:0:$idx}" "${objects[@]:$((idx+1))}")
                ((obj_count--)) || true
            else
                echo "DEL_FAIL" >> "$output_file"
            fi
        elif ((op < del_ratio + 20)) && ((obj_count > 0)); then
            # GET
            local idx=$((RANDOM % obj_count))
            local key="${objects[$idx]}"
            local code=$(curl -s -o /dev/null -w "%{http_code}" \
                "http://$node/$BUCKET/$key" 2>/dev/null)
            
            if [[ "$code" == "200" ]]; then
                echo "GET_OK" >> "$output_file"
            else
                echo "GET_FAIL" >> "$output_file"
            fi
        else
            # PUT
            if ((obj_count < per_worker_max)); then
                local key="o${worker_id}_${RANDOM}_${now}"
                local code=$(curl -s -o /dev/null -w "%{http_code}" \
                    -X PUT "http://$node/$BUCKET/$key" \
                    -H "Content-Type: application/octet-stream" \
                    --data-binary "$test_data" 2>/dev/null)
                
                if [[ "$code" == "200" ]] || [[ "$code" == "201" ]]; then
                    echo "PUT_OK" >> "$output_file"
                    objects+=("$key")
                    ((obj_count++)) || true
                else
                    echo "PUT_FAIL" >> "$output_file"
                fi
            else
                # At capacity, delete instead
                local idx=$((RANDOM % obj_count))
                local key="${objects[$idx]}"
                local code=$(curl -s -o /dev/null -w "%{http_code}" \
                    -X DELETE "http://$node/$BUCKET/$key" 2>/dev/null)
                
                if [[ "$code" == "200" ]] || [[ "$code" == "204" ]]; then
                    echo "DEL_OK" >> "$output_file"
                    objects=("${objects[@]:0:$idx}" "${objects[@]:$((idx+1))}")
                    ((obj_count--)) || true
                else
                    echo "DEL_FAIL" >> "$output_file"
                fi
            fi
        fi
    done
    
    # Cleanup worker's objects
    for key in "${objects[@]}"; do
        local node="${NODES[$((RANDOM % ${#NODES[@]}))]}"
        curl -s -X DELETE "http://$node/$BUCKET/$key" -o /dev/null 2>/dev/null
        echo "DEL_OK" >> "$output_file"
    done
}

# Main
run_load_test() {
    log_info "Starting load test..."
    log_info "  Duration: ${DURATION}s"
    log_info "  Concurrency: $CONCURRENCY workers"  
    log_info "  Object size: $OBJECT_SIZE bytes"
    log_info "  Max objects: $MAX_OBJECTS"
    log_info "  Delete ratio: ${DELETE_RATIO}%"
    log_info "  Nodes: ${NODES[*]}"
    echo ""
    
    setup_bucket
    
    local tmpdir=$(mktemp -d)
    trap "rm -rf $tmpdir" EXIT
    
    local start_time=$(date +%s)
    
    # Start workers
    for ((i=0; i<CONCURRENCY; i++)); do
        touch "$tmpdir/worker_$i.log"
        run_worker $i $DURATION "$tmpdir/worker_$i.log" $OBJECT_SIZE $MAX_OBJECTS $DELETE_RATIO $WARMUP_OBJECTS &
    done
    
    log_info "Workers started, monitoring progress..."
    echo ""
    
    # Monitor
    while [[ $(jobs -r -p | wc -l) -gt 0 ]]; do
        sleep 2
        local elapsed=$(($(date +%s) - start_time))
        
        local puts_ok=$(cat "$tmpdir"/worker_*.log 2>/dev/null | grep -c "PUT_OK" || echo 0)
        local puts_fail=$(cat "$tmpdir"/worker_*.log 2>/dev/null | grep -c "PUT_FAIL" || echo 0)
        local gets_ok=$(cat "$tmpdir"/worker_*.log 2>/dev/null | grep -c "GET_OK" || echo 0)
        local gets_fail=$(cat "$tmpdir"/worker_*.log 2>/dev/null | grep -c "GET_FAIL" || echo 0)
        local dels_ok=$(cat "$tmpdir"/worker_*.log 2>/dev/null | grep -c "DEL_OK" || echo 0)
        local dels_fail=$(cat "$tmpdir"/worker_*.log 2>/dev/null | grep -c "DEL_FAIL" || echo 0)
        
        local total=$((puts_ok + gets_ok + dels_ok))
        local ops_sec=0
        if ((elapsed > 0)); then
            ops_sec=$((total / elapsed))
        fi
        
        printf "\r${BLUE}[%3ds]${NC} PUT:${GREEN}%5d${NC}/${RED}%d${NC} GET:${GREEN}%5d${NC}/${RED}%d${NC} DEL:${GREEN}%5d${NC}/${RED}%d${NC} | ${YELLOW}%d ops/s${NC}   " \
            "$elapsed" "$puts_ok" "$puts_fail" "$gets_ok" "$gets_fail" "$dels_ok" "$dels_fail" "$ops_sec"
    done
    
    wait
    
    local end_time=$(date +%s)
    local total_time=$((end_time - start_time))
    
    # Final stats
    local puts_ok=$(cat "$tmpdir"/worker_*.log 2>/dev/null | grep -c "PUT_OK" || echo 0)
    local puts_fail=$(cat "$tmpdir"/worker_*.log 2>/dev/null | grep -c "PUT_FAIL" || echo 0)
    local gets_ok=$(cat "$tmpdir"/worker_*.log 2>/dev/null | grep -c "GET_OK" || echo 0)
    local gets_fail=$(cat "$tmpdir"/worker_*.log 2>/dev/null | grep -c "GET_FAIL" || echo 0)
    local dels_ok=$(cat "$tmpdir"/worker_*.log 2>/dev/null | grep -c "DEL_OK" || echo 0)
    local dels_fail=$(cat "$tmpdir"/worker_*.log 2>/dev/null | grep -c "DEL_FAIL" || echo 0)
    
    echo ""
    echo ""
    log_info "=========================================="
    log_info "         LOAD TEST RESULTS"
    log_info "=========================================="
    echo ""
    printf "  Duration:     %d seconds\n" "$total_time"
    printf "  Concurrency:  %d workers\n" "$CONCURRENCY"
    printf "  Object Size:  %d bytes\n" "$OBJECT_SIZE"
    echo ""
    printf "  ${GREEN}PUT Success:${NC}    %6d\n" "$puts_ok"
    printf "  ${RED}PUT Failed:${NC}     %6d\n" "$puts_fail"
    printf "  ${GREEN}GET Success:${NC}    %6d\n" "$gets_ok"
    printf "  ${RED}GET Failed:${NC}     %6d\n" "$gets_fail"
    printf "  ${GREEN}DELETE Success:${NC} %6d\n" "$dels_ok"
    printf "  ${RED}DELETE Failed:${NC}  %6d\n" "$dels_fail"
    echo ""
    
    local total_ops=$((puts_ok + gets_ok + dels_ok))
    local total_failed=$((puts_fail + gets_fail + dels_fail))
    local ops_per_sec=0
    if ((total_time > 0)); then
        ops_per_sec=$((total_ops / total_time))
    fi
    local success_rate=100
    if ((total_ops + total_failed > 0)); then
        success_rate=$((total_ops * 100 / (total_ops + total_failed)))
    fi
    
    printf "  ${YELLOW}Total Operations:${NC} %d\n" "$total_ops"
    printf "  ${YELLOW}Throughput:${NC}       %d ops/sec\n" "$ops_per_sec"
    printf "  ${YELLOW}Success Rate:${NC}     %d%%\n" "$success_rate"
    
    local mb_written=$((puts_ok * OBJECT_SIZE / 1024 / 1024))
    local mb_read=$((gets_ok * OBJECT_SIZE / 1024 / 1024))
    local mb_per_sec=0
    if ((total_time > 0)); then
        mb_per_sec=$(((puts_ok + gets_ok) * OBJECT_SIZE / 1024 / 1024 / total_time))
    fi
    
    echo ""
    printf "  ${BLUE}Data Written:${NC}     %d MB\n" "$mb_written"
    printf "  ${BLUE}Data Read:${NC}        %d MB\n" "$mb_read"
    printf "  ${BLUE}Data Throughput:${NC}  %d MB/sec\n" "$mb_per_sec"
    echo ""
    log_info "=========================================="
    
    cleanup_bucket
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--duration) DURATION="$2"; shift 2 ;;
        -c|--concurrency) CONCURRENCY="$2"; shift 2 ;;
        -s|--size) OBJECT_SIZE="$2"; shift 2 ;;
        -m|--max-objects) MAX_OBJECTS="$2"; shift 2 ;;
        --delete-ratio) DELETE_RATIO="$2"; shift 2 ;;
        --cleanup-only) setup_bucket; cleanup_bucket; exit 0 ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "  -d, --duration SEC     Test duration (default: 60)"
            echo "  -c, --concurrency N    Concurrent workers (default: 10)"
            echo "  -s, --size BYTES       Object size (default: 65536)"
            echo "  -m, --max-objects N    Max objects (default: 500)"
            echo "  --delete-ratio PCT     Delete percentage (default: 50)"
            echo "  --cleanup-only         Just cleanup"
            exit 0 ;;
        *) log_error "Unknown: $1"; exit 1 ;;
    esac
done

run_load_test
