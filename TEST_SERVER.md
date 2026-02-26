# Testing the Buckets S3 Server

This guide shows how to test the Buckets S3 API server with curl commands.

## Starting the Server

```bash
# Start on default port (9000)
./bin/buckets server

# Or specify a custom port
./bin/buckets server 8080
```

The server will print helpful information including:
- Storage directory location
- Example curl commands
- Server URL

## Test Commands

### 1. List Buckets (GET /)

```bash
# List all buckets (should be empty initially)
curl -v http://localhost:9000/
```

**Expected Response**: XML with empty bucket list

### 2. Create a Bucket (PUT /bucket-name)

```bash
# Create a bucket called "my-bucket"
curl -v -X PUT http://localhost:9000/my-bucket
```

**Expected Response**: 200 OK

### 3. List Buckets Again

```bash
# Should now show "my-bucket"
curl -v http://localhost:9000/
```

**Expected Response**: XML with `<Name>my-bucket</Name>`

### 4. Check if Bucket Exists (HEAD /bucket-name)

```bash
# Check if bucket exists
curl -v -I http://localhost:9000/my-bucket
```

**Expected Response**: 200 OK (no body)

### 5. Upload an Object (PUT /bucket-name/key)

```bash
# Upload a text file
echo "Hello, Buckets!" | curl -v -X PUT --data-binary @- http://localhost:9000/my-bucket/hello.txt

# Upload a larger file
curl -v -X PUT --data-binary @README.md http://localhost:9000/my-bucket/readme.txt
```

**Expected Response**: 200 OK with ETag

### 6. Download an Object (GET /bucket-name/key)

```bash
# Download the object
curl -v http://localhost:9000/my-bucket/hello.txt

# Save to file
curl -o downloaded.txt http://localhost:9000/my-bucket/hello.txt
```

**Expected Response**: 200 OK with object content

### 7. Get Object Metadata (HEAD /bucket-name/key)

```bash
# Get metadata without downloading
curl -v -I http://localhost:9000/my-bucket/hello.txt
```

**Expected Response**: 200 OK with ETag, Content-Length, Last-Modified headers

### 8. Delete an Object (DELETE /bucket-name/key)

```bash
# Delete the object
curl -v -X DELETE http://localhost:9000/my-bucket/hello.txt
```

**Expected Response**: 204 No Content

### 9. Try to Download Deleted Object

```bash
# Should return 404
curl -v http://localhost:9000/my-bucket/hello.txt
```

**Expected Response**: 404 Not Found with XML error

### 10. Delete the Bucket (DELETE /bucket-name)

```bash
# Delete empty bucket
curl -v -X DELETE http://localhost:9000/my-bucket
```

**Expected Response**: 204 No Content

## Error Cases to Test

### Try to Delete Non-Empty Bucket

```bash
curl -X PUT http://localhost:9000/test-bucket
echo "data" | curl -X PUT --data-binary @- http://localhost:9000/test-bucket/file.txt
curl -v -X DELETE http://localhost:9000/test-bucket
```

**Expected Response**: 409 Conflict (BucketNotEmpty)

### Try to Create Duplicate Bucket

```bash
curl -X PUT http://localhost:9000/dup-bucket
curl -v -X PUT http://localhost:9000/dup-bucket
```

**Expected Response**: 409 Conflict (BucketAlreadyOwnedByYou)

### Try to Access Non-Existent Bucket

```bash
curl -v -I http://localhost:9000/no-such-bucket
```

**Expected Response**: 404 Not Found (NoSuchBucket)

### Try to Get Non-Existent Object

```bash
curl -v http://localhost:9000/my-bucket/no-such-key
```

**Expected Response**: 404 Not Found (NoSuchKey)

## Complete Test Workflow

Here's a complete workflow to test all functionality:

```bash
# 1. Start server (in one terminal)
./bin/buckets server

# 2. Run tests (in another terminal)

# List buckets (empty)
curl http://localhost:9000/

# Create bucket
curl -X PUT http://localhost:9000/mybucket

# List buckets (should show mybucket)
curl http://localhost:9000/

# Upload objects
echo "Hello World" | curl -X PUT --data-binary @- http://localhost:9000/mybucket/hello.txt
echo "Goodbye World" | curl -X PUT --data-binary @- http://localhost:9000/mybucket/goodbye.txt

# Download objects
curl http://localhost:9000/mybucket/hello.txt
curl http://localhost:9000/mybucket/goodbye.txt

# Check metadata
curl -I http://localhost:9000/mybucket/hello.txt

# Try to delete non-empty bucket (should fail)
curl -X DELETE http://localhost:9000/mybucket

# Delete objects
curl -X DELETE http://localhost:9000/mybucket/hello.txt
curl -X DELETE http://localhost:9000/mybucket/goodbye.txt

# Delete bucket (should succeed now)
curl -X DELETE http://localhost:9000/mybucket

# Verify bucket is gone
curl http://localhost:9000/
```

## Storage Location

Objects are stored in the file system at:
```
/tmp/buckets-data/
  bucket-name/
    object-key
```

You can inspect the files directly:
```bash
ls -la /tmp/buckets-data/
ls -la /tmp/buckets-data/my-bucket/
cat /tmp/buckets-data/my-bucket/hello.txt
```

## Stopping the Server

Press `Ctrl+C` in the terminal where the server is running.

## Notes

- This is a development server for testing Week 35 and Week 37 functionality
- Authentication is not yet enforced (simplified for development)
- Storage is file-based in `/tmp/buckets-data/` (data is lost on reboot)
- Production deployment would use proper storage backends and authentication

## Troubleshooting

**Port already in use?**
```bash
# Use a different port
./bin/buckets server 8080
```

**Permission denied?**
```bash
# Make sure binary is executable
chmod +x bin/buckets
```

**Server not responding?**
```bash
# Check if server started successfully
# Look for "Server started successfully!" in the output
# Check for any error messages

# Test basic connectivity
curl -v http://localhost:9000/
```
