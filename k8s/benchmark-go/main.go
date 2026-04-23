package main

import (
	"bytes"
	"crypto/rand"
	"flag"
	"fmt"
	"io"
	"net/http"
	"sync"
	"time"
)

type Result struct {
	Success  bool
	Duration time.Duration
}

type Stats struct {
	TotalOps      int64
	SuccessOps    int64
	FailedOps     int64
	TotalDuration time.Duration
	MinLatency    time.Duration
	MaxLatency    time.Duration
}

func generateTestData(size int) []byte {
	data := make([]byte, size)
	rand.Read(data)
	return data
}

func worker(id int, endpoint string, bucket string, data []byte, duration time.Duration, results chan<- Result, wg *sync.WaitGroup) {
	defer wg.Done()

	// Each worker gets its own HTTP client to avoid lock contention
	// But we configure larger connection pool to prevent blocking
	client := &http.Client{
		Timeout: 30 * time.Second,
		Transport: &http.Transport{
			MaxIdleConnsPerHost: 100,  // Large enough for any worker count
			MaxConnsPerHost:     0,     // Unlimited (default behavior)
			IdleConnTimeout:     90 * time.Second,
		},
	}

	endTime := time.Now().Add(duration)
	opCount := 0

	for time.Now().Before(endTime) {
		key := fmt.Sprintf("worker-%d/obj-%06d.bin", id, opCount)
		url := fmt.Sprintf("%s/%s/%s", endpoint, bucket, key)

		start := time.Now()
		req, err := http.NewRequest("PUT", url, bytes.NewReader(data))
		if err != nil {
			results <- Result{Success: false, Duration: 0}
			continue
		}

		req.Header.Set("Content-Type", "application/octet-stream")
		req.ContentLength = int64(len(data))

		resp, err := client.Do(req)
		elapsed := time.Since(start)

		if err != nil {
			results <- Result{Success: false, Duration: elapsed}
			opCount++
			continue
		}

		io.Copy(io.Discard, resp.Body)
		resp.Body.Close()

		success := resp.StatusCode >= 200 && resp.StatusCode < 300
		results <- Result{Success: success, Duration: elapsed}
		opCount++
	}
}

func main() {
	endpoint := flag.String("endpoint", "http://buckets-0.buckets-headless.buckets.svc.cluster.local:9000", "S3 endpoint")
	bucket := flag.String("bucket", "benchmark-go", "Bucket name")
	workers := flag.Int("workers", 50, "Number of concurrent workers")
	duration := flag.Duration("duration", 30*time.Second, "Test duration")
	objectSize := flag.Int("size", 256*1024, "Object size in bytes")
	flag.Parse()

	fmt.Println("========================================================================")
	fmt.Println("Go Concurrent HTTP Benchmark - True Parallel Performance")
	fmt.Println("========================================================================")
	fmt.Printf("Endpoint:     %s\n", *endpoint)
	fmt.Printf("Bucket:       %s\n", *bucket)
	fmt.Printf("Object Size:  %d KB\n", *objectSize/1024)
	fmt.Printf("Workers:      %d\n", *workers)
	fmt.Printf("Duration:     %v\n", *duration)
	fmt.Println("========================================================================")
	fmt.Println()

	// Create bucket
	fmt.Printf("[1/4] Creating bucket '%s'...\n", *bucket)
	client := &http.Client{Timeout: 10 * time.Second}
	req, _ := http.NewRequest("PUT", fmt.Sprintf("%s/%s", *endpoint, *bucket), nil)
	resp, err := client.Do(req)
	if err != nil {
		fmt.Printf("      ✗ Failed to create bucket: %v\n", err)
	} else {
		io.Copy(io.Discard, resp.Body)
		resp.Body.Close()
		if resp.StatusCode == 200 || resp.StatusCode == 409 {
			fmt.Println("      ✓ Bucket ready")
		} else {
			fmt.Printf("      ⚠ Unexpected status: %d\n", resp.StatusCode)
		}
	}
	fmt.Println()

	// Generate test data
	fmt.Printf("[2/4] Generating %d KB test data...\n", *objectSize/1024)
	data := generateTestData(*objectSize)
	fmt.Println("      ✓ Data generated")
	fmt.Println()

	// Run benchmark
	fmt.Printf("[3/4] Running %d-worker benchmark for %v...\n", *workers, *duration)
	
	results := make(chan Result, *workers*100)
	var wg sync.WaitGroup

	startTime := time.Now()

	// Launch workers (each creates its own client to avoid lock contention)
	for i := 0; i < *workers; i++ {
		wg.Add(1)
		go worker(i, *endpoint, *bucket, data, *duration, results, &wg)
	}

	// Wait for completion in background
	go func() {
		wg.Wait()
		close(results)
	}()

	// Collect results
	var stats Stats
	stats.MinLatency = time.Hour
	stats.MaxLatency = 0

	for result := range results {
		stats.TotalOps++
		if result.Success {
			stats.SuccessOps++
		} else {
			stats.FailedOps++
		}
		stats.TotalDuration += result.Duration

		if result.Duration < stats.MinLatency {
			stats.MinLatency = result.Duration
		}
		if result.Duration > stats.MaxLatency {
			stats.MaxLatency = result.Duration
		}
	}

	actualDuration := time.Since(startTime)
	fmt.Println("      ✓ Benchmark complete")
	fmt.Println()

	// Calculate metrics
	throughput := float64(stats.SuccessOps) / actualDuration.Seconds()
	avgLatency := float64(stats.TotalDuration.Milliseconds()) / float64(stats.TotalOps)
	bandwidth := throughput * float64(*objectSize) / (1024 * 1024)
	dataWritten := float64(stats.SuccessOps) * float64(*objectSize) / (1024 * 1024)

	// Print results
	fmt.Println("[4/4] Results:")
	fmt.Println("========================================================================")
	fmt.Printf("Total Operations:    %d\n", stats.TotalOps)
	fmt.Printf("Successful:          %d\n", stats.SuccessOps)
	fmt.Printf("Failed:              %d\n", stats.FailedOps)
	fmt.Printf("Actual Duration:     %.2f seconds\n", actualDuration.Seconds())
	fmt.Printf("Throughput:          %.2f ops/sec\n", throughput)
	fmt.Printf("Average Latency:     %.2f ms\n", avgLatency)
	fmt.Printf("Min Latency:         %.2f ms\n", float64(stats.MinLatency.Microseconds())/1000.0)
	fmt.Printf("Max Latency:         %.2f ms\n", float64(stats.MaxLatency.Microseconds())/1000.0)
	fmt.Printf("Bandwidth:           %.2f MB/s\n", bandwidth)
	fmt.Printf("Data Written:        %.2f MB\n", dataWritten)
	fmt.Println("========================================================================")
	fmt.Println()
	fmt.Println("✓ Benchmark complete!")
}
