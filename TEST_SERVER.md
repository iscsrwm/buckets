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
curl -v -X PUT http://localhost:9000/my-bucket -H "Content-Length: 0"
```

**Expected Response**: 200 OK

**Note**: The `Content-Length: 0` header is required by the HTTP server (mongoose) for PUT requests, even when there's no body.

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
curl -X PUT http://localhost:9000/test-bucket -H "Content-Length: 0"
echo "data" | curl -X PUT --data-binary @- http://localhost:9000/test-bucket/file.txt
curl -v -X DELETE http://localhost:9000/test-bucket
```

**Expected Response**: 409 Conflict (BucketNotEmpty)

### Try to Create Duplicate Bucket

```bash
curl -X PUT http://localhost:9000/dup-bucket -H "Content-Length: 0"
curl -v -X PUT http://localhost:9000/dup-bucket -H "Content-Length: 0"
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
curl -X PUT http://localhost:9000/mybucket -H "Content-Length: 0"

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

- This is a development server for testing Weeks 35, 37, 38, and 39 functionality
- Authentication is not yet enforced (simplified for development)
- Storage is file-based in `/tmp/buckets-data/` (data is lost on reboot)
- Production deployment would use proper storage backends and authentication

## Multipart Upload (Week 39)

Multipart upload allows uploading large files in parts, which can be uploaded in parallel and resumed if interrupted.

### Initiate Multipart Upload

```bash
# Initiate a multipart upload for a large file
curl -X POST "http://localhost:9000/my-bucket/large-file.bin?uploads" \
  -H "Content-Length: 0"
```

**Expected Response**: XML with `<UploadId>` - save this ID for subsequent operations
```xml
<?xml version="1.0" encoding="UTF-8"?>
<InitiateMultipartUploadResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Bucket>my-bucket</Bucket>
  <Key>large-file.bin</Key>
  <UploadId>d4b805f5ec82421cade120af8595973c</UploadId>
</InitiateMultipartUploadResult>
```

### Upload Parts

```bash
# Set the upload ID from the previous response
UPLOAD_ID="d4b805f5ec82421cade120af8595973c"

# Upload part 1 (must include Content-Length)
echo "Part 1 data here" | curl -X PUT \
  "http://localhost:9000/my-bucket/large-file.bin?uploadId=${UPLOAD_ID}&partNumber=1" \
  -H "Content-Length: 17" \
  --data-binary @- -i

# Upload part 2
echo "Part 2 data here" | curl -X PUT \
  "http://localhost:9000/my-bucket/large-file.bin?uploadId=${UPLOAD_ID}&partNumber=2" \
  -H "Content-Length: 17" \
  --data-binary @- -i
```

**Expected Response**: 200 OK with `ETag` header (MD5 hash of part)
```
HTTP/1.1 200 OK
ETag: "e8130839e04b834104325e178da8d8a9"
Content-Type: application/xml
Content-Length: 0
```

**Note**: Save the ETags from each part - you'll need them to complete the upload.

### Complete Multipart Upload (Coming in Week 39 Part 2)

```bash
# Complete the multipart upload (not yet implemented)
curl -X POST "http://localhost:9000/my-bucket/large-file.bin?uploadId=${UPLOAD_ID}" \
  -H "Content-Type: application/xml" \
  --data '<CompleteMultipartUpload>
    <Part>
      <PartNumber>1</PartNumber>
      <ETag>"e8130839e04b834104325e178da8d8a9"</ETag>
    </Part>
    <Part>
      <PartNumber>2</PartNumber>
      <ETag>"057a106664d27c1295d959d298b6a746"</ETag>
    </Part>
  </CompleteMultipartUpload>'
```

### Abort Multipart Upload (Coming in Week 39 Part 2)

```bash
# Abort/cancel a multipart upload (not yet implemented)
curl -X DELETE "http://localhost:9000/my-bucket/large-file.bin?uploadId=${UPLOAD_ID}"
```

### List Parts (Coming in Week 39 Part 2)

```bash
# List uploaded parts (not yet implemented)
curl "http://localhost:9000/my-bucket/large-file.bin?uploadId=${UPLOAD_ID}"
```

### Verify Uploaded Parts

```bash
# Parts are stored in .multipart directory
ls -la /tmp/buckets-data/my-bucket/.multipart/${UPLOAD_ID}/parts/

# View part contents
cat /tmp/buckets-data/my-bucket/.multipart/${UPLOAD_ID}/parts/part.1
cat /tmp/buckets-data/my-bucket/.multipart/${UPLOAD_ID}/parts/part.2

# View upload metadata
cat /tmp/buckets-data/my-bucket/.multipart/${UPLOAD_ID}/metadata.json
```

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
