#!/usr/bin/env python3
"""
GET-only benchmark for Buckets S3 cluster.
Creates test objects once, then measures pure GET throughput.
"""

import boto3
from botocore.config import Config
import time
import threading
import random
import argparse
import sys
from collections import defaultdict

def create_client(endpoint):
    return boto3.client('s3',
        endpoint_url=endpoint,
        aws_access_key_id='minioadmin',
        aws_secret_access_key='minioadmin',
        config=Config(
            signature_version='s3v4',
            max_pool_connections=100,
            connect_timeout=5,
            read_timeout=30,
            retries={'max_attempts': 0}
        ),
        region_name='us-east-1'
    )

def main():
    parser = argparse.ArgumentParser(description='GET-only S3 benchmark')
    parser.add_argument('-d', '--duration', type=int, default=30, help='Test duration in seconds')
    parser.add_argument('-c', '--concurrency', type=int, default=50, help='Number of concurrent workers')
    parser.add_argument('-n', '--num-objects', type=int, default=100, help='Number of test objects')
    parser.add_argument('-s', '--size', type=int, default=1024, help='Object size in bytes')
    parser.add_argument('-p', '--port', type=int, default=9001, help='Starting port')
    args = parser.parse_args()

    # Create clients for load balancing across nodes
    endpoints = [f'http://localhost:{args.port + i}' for i in range(6)]
    clients = [create_client(ep) for ep in endpoints]
    primary = clients[0]

    bucket = 'getbench2'
    
    print("=" * 60)
    print("  GET-Only Benchmark")
    print("=" * 60)
    print(f"  Duration:     {args.duration}s")
    print(f"  Concurrency:  {args.concurrency} workers")
    print(f"  Objects:      {args.num_objects}")
    print(f"  Object size:  {args.size} bytes")
    print("=" * 60)
    print(flush=True)

    # Setup
    print("Setting up test objects...", flush=True)
    try:
        primary.create_bucket(Bucket=bucket)
    except:
        pass

    # Create test objects
    test_data = b'x' * args.size
    for i in range(args.num_objects):
        primary.put_object(Bucket=bucket, Key=f'obj{i}', Body=test_data)
        if (i + 1) % 20 == 0:
            print(f"  Created {i + 1}/{args.num_objects} objects", flush=True)
    
    print(f"  Created {args.num_objects} objects", flush=True)
    print(flush=True)

    # Use atomic counters
    import ctypes
    success_count = ctypes.c_long(0)
    failed_count = ctypes.c_long(0)
    success_lock = threading.Lock()
    stop_event = threading.Event()
    
    def worker(worker_id):
        client = clients[worker_id % 6]
        local_success = 0
        local_failed = 0
        
        while not stop_event.is_set():
            try:
                key = f'obj{random.randint(0, args.num_objects - 1)}'
                response = client.get_object(Bucket=bucket, Key=key)
                response['Body'].read()
                local_success += 1
            except Exception as e:
                local_failed += 1
                # Don't spam on errors
                if local_failed < 3:
                    print(f"Worker {worker_id} error: {e}", flush=True)
        
        with success_lock:
            success_count.value += local_success
            failed_count.value += local_failed

    # Start workers
    print(f"Starting {args.concurrency} workers...", flush=True)
    threads = []
    for i in range(args.concurrency):
        t = threading.Thread(target=worker, args=(i,), daemon=True)
        t.start()
        threads.append(t)

    # Give workers time to start
    time.sleep(0.5)
    
    # Progress reporting
    print(flush=True)
    start_time = time.time()
    prev_count = 0
    
    for sec in range(args.duration):
        time.sleep(1)
        current = success_count.value
        rate = current - prev_count
        elapsed = time.time() - start_time
        avg_rate = current / elapsed if elapsed > 0 else 0
        print(f"[{sec+1:3d}s] GETs: {current:6d}  |  This sec: {rate:4d} ops/s  |  Avg: {avg_rate:.0f} ops/s", flush=True)
        prev_count = current

    # Stop workers
    stop_event.set()
    time.sleep(1)  # Give workers time to finish

    elapsed = time.time() - start_time
    final_success = success_count.value
    final_failed = failed_count.value
    
    print(flush=True)
    print("=" * 60)
    print("  RESULTS")
    print("=" * 60)
    print(f"  Duration:      {elapsed:.1f} seconds")
    print(f"  Total GETs:    {final_success}")
    print(f"  Failed:        {final_failed}")
    print(f"  Throughput:    {final_success/elapsed:.1f} ops/sec")
    if final_success + final_failed > 0:
        print(f"  Success rate:  {100*final_success/(final_success+final_failed):.1f}%")
    print("=" * 60)
    print(flush=True)

    # Cleanup
    print("Cleaning up...", flush=True)
    for i in range(args.num_objects):
        try:
            primary.delete_object(Bucket=bucket, Key=f'obj{i}')
        except:
            pass
    try:
        primary.delete_bucket(Bucket=bucket)
    except:
        pass
    print("Done!")

if __name__ == '__main__':
    main()
