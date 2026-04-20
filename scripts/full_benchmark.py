#!/usr/bin/env python3
"""
Comprehensive S3 Performance Benchmark
Tests GET-only, PUT-only, and Mixed workloads
"""

import boto3
from botocore.config import Config
import time
import threading
import random
import ctypes
import sys

def make_client(port):
    session = boto3.session.Session()
    return session.client('s3', endpoint_url=f'http://localhost:{port}',
        aws_access_key_id='minioadmin', aws_secret_access_key='minioadmin',
        config=Config(signature_version='s3v4', max_pool_connections=20),
        region_name='us-east-1')

def run_benchmark(name, op_func, duration=20, num_workers=50):
    """Run a benchmark with the given operation function"""
    counter = ctypes.c_long(0)
    lock = threading.Lock()
    stop = threading.Event()
    
    def worker(wid):
        local = 0
        while not stop.is_set():
            try:
                op_func(wid)
                local += 1
                if local % 20 == 0:
                    with lock:
                        counter.value += 20
                    local = 0
            except:
                pass
        with lock:
            counter.value += local
    
    threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(num_workers)]
    start = time.time()
    for t in threads: t.start()
    
    rates = []
    prev = 0
    for sec in range(duration):
        time.sleep(1)
        with lock:
            current = counter.value
        rate = current - prev
        rates.append(rate)
        elapsed = time.time() - start
        avg = current / elapsed if elapsed > 0 else 0
        print(f"  [{sec+1:2d}s] {name}: {current:6d}  |  {rate:4d}/s  |  avg: {avg:.0f}/s", flush=True)
        prev = current
    
    stop.set()
    for t in threads: t.join(timeout=3)
    
    elapsed = time.time() - start
    total = counter.value
    
    # Calculate stats (skip first 2 seconds for warmup)
    stable_rates = rates[2:] if len(rates) > 2 else rates
    avg_rate = sum(stable_rates) / len(stable_rates) if stable_rates else 0
    peak_rate = max(rates) if rates else 0
    
    return {
        'total': total,
        'duration': elapsed,
        'throughput': total / elapsed,
        'avg_rate': avg_rate,
        'peak_rate': peak_rate
    }

def main():
    print("=" * 70, flush=True)
    print("  BUCKETS S3 PERFORMANCE BENCHMARK", flush=True)
    print("=" * 70, flush=True)
    print(flush=True)
    
    bucket = 'perfbench'
    num_objects = 100
    object_size = 1024
    num_workers = 50
    duration = 20
    
    # Setup
    print("Setting up...", flush=True)
    primary = make_client(9001)
    try:
        primary.create_bucket(Bucket=bucket)
    except:
        pass
    
    # Create test objects and warm up clients
    print(f"Creating {num_objects} test objects...", flush=True)
    test_data = b'x' * object_size
    for i in range(num_objects):
        primary.put_object(Bucket=bucket, Key=f'obj{i}', Body=test_data)
    
    print("Warming up clients...", flush=True)
    clients = []
    for i in range(num_workers):
        c = make_client(9001 + (i % 6))
        c.get_object(Bucket=bucket, Key='obj0')['Body'].read()
        clients.append(c)
    
    print(flush=True)
    print("=" * 70, flush=True)
    print(f"  Test Parameters: {num_workers} workers, {duration}s each, {object_size}B objects", flush=True)
    print("=" * 70, flush=True)
    print(flush=True)
    
    results = {}
    
    # Test 1: GET-only
    print("-" * 70, flush=True)
    print("  TEST 1: GET-only workload", flush=True)
    print("-" * 70, flush=True)
    def get_op(wid):
        key = f'obj{random.randint(0, num_objects-1)}'
        clients[wid].get_object(Bucket=bucket, Key=key)['Body'].read()
    results['GET'] = run_benchmark('GET', get_op, duration, num_workers)
    print(flush=True)
    
    # Test 2: PUT-only
    print("-" * 70, flush=True)
    print("  TEST 2: PUT-only workload", flush=True)
    print("-" * 70, flush=True)
    put_counter = [0]
    def put_op(wid):
        put_counter[0] += 1
        key = f'put_{wid}_{put_counter[0]}'
        clients[wid].put_object(Bucket=bucket, Key=key, Body=test_data)
    results['PUT'] = run_benchmark('PUT', put_op, duration, num_workers)
    print(flush=True)
    
    # Test 3: Mixed (50% GET, 50% PUT)
    print("-" * 70, flush=True)
    print("  TEST 3: Mixed workload (50% GET, 50% PUT)", flush=True)
    print("-" * 70, flush=True)
    mix_counter = [0]
    def mix_op(wid):
        if random.random() < 0.5:
            key = f'obj{random.randint(0, num_objects-1)}'
            clients[wid].get_object(Bucket=bucket, Key=key)['Body'].read()
        else:
            mix_counter[0] += 1
            key = f'mix_{wid}_{mix_counter[0]}'
            clients[wid].put_object(Bucket=bucket, Key=key, Body=test_data)
    results['MIXED'] = run_benchmark('MIX', mix_op, duration, num_workers)
    print(flush=True)
    
    # Summary
    print("=" * 70, flush=True)
    print("  SUMMARY", flush=True)
    print("=" * 70, flush=True)
    print(flush=True)
    print(f"  {'Workload':<12} {'Total Ops':>10} {'Throughput':>12} {'Avg Rate':>10} {'Peak Rate':>10}", flush=True)
    print(f"  {'-'*12} {'-'*10} {'-'*12} {'-'*10} {'-'*10}", flush=True)
    for name, r in results.items():
        print(f"  {name:<12} {r['total']:>10} {r['throughput']:>10.1f}/s {r['avg_rate']:>8.0f}/s {r['peak_rate']:>8.0f}/s", flush=True)
    print(flush=True)
    print("=" * 70, flush=True)
    
    # Cleanup
    print("Cleaning up...", flush=True)
    # List and delete all objects
    paginator = primary.get_paginator('list_objects_v2')
    for page in paginator.paginate(Bucket=bucket):
        for obj in page.get('Contents', []):
            try:
                primary.delete_object(Bucket=bucket, Key=obj['Key'])
            except:
                pass
    try:
        primary.delete_bucket(Bucket=bucket)
    except:
        pass
    print("Done!", flush=True)

if __name__ == '__main__':
    main()
