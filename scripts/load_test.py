#!/usr/bin/env python3
"""
S3 Load Test - Bounded Write/Delete Test
Tests throughput and resilience while keeping disk usage bounded
"""

import argparse
import hashlib
import hmac
import os
import random
import string
import sys
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime
from typing import List, Dict, Optional
import urllib.request
import urllib.error

# Configuration
NODES = [
    "127.0.0.1:9001",
    "127.0.0.1:9002", 
    "127.0.0.1:9003",
    "127.0.0.1:9004",
    "127.0.0.1:9005",
    "127.0.0.1:9006",
]

# AWS Signature V4 credentials
AWS_ACCESS_KEY = "minioadmin"
AWS_SECRET_KEY = "minioadmin"
AWS_REGION = "us-east-1"
AWS_SERVICE = "s3"


def sign(key: bytes, msg: str) -> bytes:
    """HMAC-SHA256 sign"""
    return hmac.new(key, msg.encode('utf-8'), hashlib.sha256).digest()


def get_signature_key(secret_key: str, date_stamp: str, region: str, service: str) -> bytes:
    """Derive the signing key"""
    k_date = sign(('AWS4' + secret_key).encode('utf-8'), date_stamp)
    k_region = sign(k_date, region)
    k_service = sign(k_region, service)
    k_signing = sign(k_service, 'aws4_request')
    return k_signing


def aws_sign_request(method: str, host: str, uri: str, headers: Dict[str, str], 
                     payload: bytes = b'') -> Dict[str, str]:
    """Sign a request with AWS Signature V4"""
    
    # Create timestamp
    t = datetime.utcnow()
    amz_date = t.strftime('%Y%m%dT%H%M%SZ')
    date_stamp = t.strftime('%Y%m%d')
    
    # Calculate payload hash
    payload_hash = hashlib.sha256(payload).hexdigest()
    
    # Add required headers
    headers['host'] = host
    headers['x-amz-date'] = amz_date
    headers['x-amz-content-sha256'] = payload_hash
    
    # Create canonical headers (must be sorted)
    signed_header_names = sorted(headers.keys())
    canonical_headers = ''.join(f"{k}:{headers[k]}\n" for k in signed_header_names)
    signed_headers = ';'.join(signed_header_names)
    
    # Create canonical request
    canonical_request = '\n'.join([
        method,
        uri,
        '',  # query string (empty)
        canonical_headers,
        signed_headers,
        payload_hash
    ])
    
    # Create string to sign
    algorithm = 'AWS4-HMAC-SHA256'
    credential_scope = f"{date_stamp}/{AWS_REGION}/{AWS_SERVICE}/aws4_request"
    string_to_sign = '\n'.join([
        algorithm,
        amz_date,
        credential_scope,
        hashlib.sha256(canonical_request.encode('utf-8')).hexdigest()
    ])
    
    # Calculate signature
    signing_key = get_signature_key(AWS_SECRET_KEY, date_stamp, AWS_REGION, AWS_SERVICE)
    signature = hmac.new(signing_key, string_to_sign.encode('utf-8'), hashlib.sha256).hexdigest()
    
    # Create authorization header
    authorization = (
        f"{algorithm} "
        f"Credential={AWS_ACCESS_KEY}/{credential_scope}, "
        f"SignedHeaders={signed_headers}, "
        f"Signature={signature}"
    )
    
    headers['Authorization'] = authorization
    return headers

@dataclass
class Stats:
    puts_ok: int = 0
    puts_fail: int = 0
    gets_ok: int = 0
    gets_fail: int = 0
    dels_ok: int = 0
    dels_fail: int = 0
    lock: threading.Lock = field(default_factory=threading.Lock)
    
    def add_put(self, ok: bool):
        with self.lock:
            if ok:
                self.puts_ok += 1
            else:
                self.puts_fail += 1
    
    def add_get(self, ok: bool):
        with self.lock:
            if ok:
                self.gets_ok += 1
            else:
                self.gets_fail += 1
                
    def add_del(self, ok: bool):
        with self.lock:
            if ok:
                self.dels_ok += 1
            else:
                self.dels_fail += 1

def get_node() -> str:
    return random.choice(NODES)

def random_key(prefix: str = "obj") -> str:
    suffix = ''.join(random.choices(string.ascii_lowercase + string.digits, k=12))
    return f"{prefix}_{suffix}"

def do_put(bucket: str, key: str, data: bytes, timeout: int = 30) -> bool:
    """PUT an object, returns True on success"""
    node = get_node()
    uri = f"/{bucket}/{key}"
    url = f"http://{node}{uri}"
    try:
        headers = {'content-type': 'application/octet-stream'}
        signed_headers = aws_sign_request('PUT', node, uri, headers, data)
        
        req = urllib.request.Request(url, data=data, method='PUT')
        for k, v in signed_headers.items():
            req.add_header(k, v)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status in (200, 201)
    except Exception as e:
        return False

def do_get(bucket: str, key: str, timeout: int = 30) -> bool:
    """GET an object, returns True on success"""
    node = get_node()
    uri = f"/{bucket}/{key}"
    url = f"http://{node}{uri}"
    try:
        headers = {}
        signed_headers = aws_sign_request('GET', node, uri, headers, b'')
        
        req = urllib.request.Request(url, method='GET')
        for k, v in signed_headers.items():
            req.add_header(k, v)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            _ = resp.read()
            return resp.status == 200
    except Exception:
        return False

def do_delete(bucket: str, key: str, timeout: int = 30) -> bool:
    """DELETE an object, returns True on success"""
    node = get_node()
    uri = f"/{bucket}/{key}"
    url = f"http://{node}{uri}"
    try:
        headers = {}
        signed_headers = aws_sign_request('DELETE', node, uri, headers, b'')
        
        req = urllib.request.Request(url, method='DELETE')
        for k, v in signed_headers.items():
            req.add_header(k, v)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status in (200, 204)
    except urllib.error.HTTPError as e:
        return e.code in (200, 204, 404)  # 404 is ok for delete
    except Exception:
        return False

def create_bucket(bucket: str):
    """Create bucket on all nodes"""
    for node in NODES:
        uri = f"/{bucket}"
        url = f"http://{node}{uri}"
        try:
            headers = {}
            signed_headers = aws_sign_request('PUT', node, uri, headers, b'')
            
            req = urllib.request.Request(url, method='PUT')
            for k, v in signed_headers.items():
                req.add_header(k, v)
            urllib.request.urlopen(req, timeout=5)
        except Exception:
            pass  # Bucket may already exist

def cleanup_bucket(bucket: str):
    """Delete all objects in bucket"""
    node = NODES[0]
    uri = f"/{bucket}"
    url = f"http://{node}{uri}"
    try:
        headers = {}
        signed_headers = aws_sign_request('GET', node, uri, headers, b'')
        
        req = urllib.request.Request(url, method='GET')
        for k, v in signed_headers.items():
            req.add_header(k, v)
        with urllib.request.urlopen(req, timeout=30) as resp:
            content = resp.read().decode('utf-8')
            import re
            keys = re.findall(r'<Key>([^<]+)</Key>', content)
            print(f"  Cleaning up {len(keys)} objects...")
            for key in keys:
                do_delete(bucket, key)
    except Exception as e:
        print(f"  Cleanup error: {e}")

def worker(worker_id: int, bucket: str, duration: int, object_size: int,
           max_objects: int, delete_ratio: int, warmup: int, stats: Stats, 
           stop_event: threading.Event, timeout: int):
    """Worker thread that performs PUT/GET/DELETE operations sequentially"""
    
    objects: List[str] = []
    test_data = os.urandom(object_size)
    per_worker_max = max(20, max_objects // 6)  # Each worker manages ~1/6 of objects
    per_worker_warmup = max(5, warmup // 6)
    warmup_done = False
    
    end_time = time.time() + duration
    
    while time.time() < end_time and not stop_event.is_set():
        # Warmup phase: only PUTs
        if not warmup_done:
            key = random_key(f"w{worker_id}")
            ok = do_put(bucket, key, test_data, timeout)
            stats.add_put(ok)
            if ok:
                objects.append(key)
            if len(objects) >= per_worker_warmup:
                warmup_done = True
            continue
        
        # Main phase: mixed operations
        op = random.randint(0, 99)
        
        if op < delete_ratio and len(objects) > 5:
            # DELETE
            key = random.choice(objects)
            ok = do_delete(bucket, key, timeout)
            stats.add_del(ok)
            if ok and key in objects:
                objects.remove(key)
                
        elif op < delete_ratio + 20 and objects:
            # GET (20% of remaining operations)
            key = random.choice(objects)
            ok = do_get(bucket, key, timeout)
            stats.add_get(ok)
            
        else:
            # PUT
            if len(objects) < per_worker_max:
                key = random_key(f"o{worker_id}")
                ok = do_put(bucket, key, test_data, timeout)
                stats.add_put(ok)
                if ok:
                    objects.append(key)
            else:
                # At capacity, force delete
                if objects:
                    key = random.choice(objects)
                    ok = do_delete(bucket, key, timeout)
                    stats.add_del(ok)
                    if ok and key in objects:
                        objects.remove(key)
    
    # Cleanup: delete our objects
    for key in objects:
        do_delete(bucket, key, timeout)
        stats.add_del(True)

def print_progress(stats: Stats, elapsed: int, object_size: int):
    """Print progress line"""
    total = stats.puts_ok + stats.gets_ok + stats.dels_ok
    ops_sec = total / elapsed if elapsed > 0 else 0
    
    sys.stdout.write(f"\r[{elapsed:3d}s] PUT:{stats.puts_ok:5d}/{stats.puts_fail:<3d} "
                     f"GET:{stats.gets_ok:5d}/{stats.gets_fail:<3d} "
                     f"DEL:{stats.dels_ok:5d}/{stats.dels_fail:<3d} | "
                     f"{ops_sec:6.1f} ops/s   ")
    sys.stdout.flush()

def run_load_test(args):
    bucket = "loadtest"
    stats = Stats()
    stop_event = threading.Event()
    
    print(f"\n{'='*50}")
    print(f"  S3 Load Test - 6 Node Cluster")
    print(f"{'='*50}")
    print(f"  Duration:     {args.duration}s")
    print(f"  Concurrency:  {args.concurrency} workers")
    print(f"  Object size:  {args.size} bytes ({args.size//1024}KB)")
    print(f"  Max objects:  {args.max_objects}")
    print(f"  Delete ratio: {args.delete_ratio}%")
    print(f"  Timeout:      {args.timeout}s per operation")
    print(f"  Nodes:        {len(NODES)} nodes (ports 9001-9006)")
    print(f"{'='*50}\n")
    
    print("Creating test bucket...")
    create_bucket(bucket)
    print("Starting workers...\n")
    
    start_time = time.time()
    
    # Start worker threads
    threads = []
    for i in range(args.concurrency):
        t = threading.Thread(
            target=worker,
            args=(i, bucket, args.duration, args.size, args.max_objects,
                  args.delete_ratio, args.warmup, stats, stop_event, args.timeout)
        )
        t.start()
        threads.append(t)
    
    # Monitor progress
    try:
        while any(t.is_alive() for t in threads):
            elapsed = int(time.time() - start_time)
            print_progress(stats, elapsed, args.size)
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n\nInterrupted! Stopping workers...")
        stop_event.set()
    
    # Wait for all threads
    for t in threads:
        t.join(timeout=args.timeout + 5)
    
    end_time = time.time()
    total_time = max(1, int(end_time - start_time))
    
    # Final results
    print(f"\n\n{'='*50}")
    print(f"         LOAD TEST RESULTS")
    print(f"{'='*50}\n")
    print(f"  Duration:     {total_time} seconds")
    print(f"  Concurrency:  {args.concurrency} workers")
    print(f"  Object Size:  {args.size} bytes\n")
    
    print(f"  \033[32mPUT Success:\033[0m    {stats.puts_ok:6d}")
    print(f"  \033[31mPUT Failed:\033[0m     {stats.puts_fail:6d}")
    print(f"  \033[32mGET Success:\033[0m    {stats.gets_ok:6d}")
    print(f"  \033[31mGET Failed:\033[0m     {stats.gets_fail:6d}")
    print(f"  \033[32mDELETE Success:\033[0m {stats.dels_ok:6d}")
    print(f"  \033[31mDELETE Failed:\033[0m  {stats.dels_fail:6d}\n")
    
    total_ops = stats.puts_ok + stats.gets_ok + stats.dels_ok
    total_failed = stats.puts_fail + stats.gets_fail + stats.dels_fail
    ops_per_sec = total_ops / total_time if total_time > 0 else 0
    success_rate = (total_ops * 100 / (total_ops + total_failed)) if (total_ops + total_failed) > 0 else 100
    
    print(f"  \033[33mTotal Operations:\033[0m {total_ops}")
    print(f"  \033[33mThroughput:\033[0m       {ops_per_sec:.1f} ops/sec")
    print(f"  \033[33mSuccess Rate:\033[0m     {success_rate:.1f}%\n")
    
    mb_written = (stats.puts_ok * args.size) / (1024 * 1024)
    mb_read = (stats.gets_ok * args.size) / (1024 * 1024)
    mb_per_sec = ((stats.puts_ok + stats.gets_ok) * args.size) / (1024 * 1024) / total_time if total_time > 0 else 0
    
    print(f"  \033[34mData Written:\033[0m     {mb_written:.1f} MB")
    print(f"  \033[34mData Read:\033[0m        {mb_read:.1f} MB")
    print(f"  \033[34mData Throughput:\033[0m  {mb_per_sec:.1f} MB/sec")
    print(f"\n{'='*50}")
    
    print("\nCleaning up...")
    cleanup_bucket(bucket)
    print("Done!\n")

def main():
    parser = argparse.ArgumentParser(description='S3 Load Test - Bounded Write/Delete')
    parser.add_argument('-d', '--duration', type=int, default=60,
                        help='Test duration in seconds (default: 60)')
    parser.add_argument('-c', '--concurrency', type=int, default=6,
                        help='Number of concurrent workers (default: 6)')
    parser.add_argument('-s', '--size', type=int, default=16384,
                        help='Object size in bytes (default: 16384)')
    parser.add_argument('-m', '--max-objects', type=int, default=300,
                        help='Max objects to keep (default: 300)')
    parser.add_argument('--delete-ratio', type=int, default=50,
                        help='Percentage of delete operations (default: 50)')
    parser.add_argument('--warmup', type=int, default=60,
                        help='Objects to create before deletes start (default: 60)')
    parser.add_argument('--timeout', type=int, default=30,
                        help='Timeout per operation in seconds (default: 30)')
    parser.add_argument('--cleanup-only', action='store_true',
                        help='Only cleanup existing test data')
    
    args = parser.parse_args()
    
    if args.cleanup_only:
        print("Cleaning up test bucket...")
        cleanup_bucket("loadtest")
        print("Done!")
        return
    
    run_load_test(args)

if __name__ == '__main__':
    main()
