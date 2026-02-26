# S3 API Layer Architecture

**Phase**: Phase 9 (Weeks 35-42)  
**Goal**: S3-compatible REST API for object storage operations  
**Status**: Week 35 - Initial Implementation

---

## Overview

The S3 API Layer provides Amazon S3-compatible REST API endpoints for bucket and object operations. This allows existing S3 clients (AWS SDK, MinIO mc, s3cmd, etc.) to work seamlessly with Buckets.

### Key Requirements

1. **S3 Compatibility**: Support core S3 API operations
2. **Authentication**: AWS Signature Version 4 (SigV4)
3. **XML Responses**: S3-compatible XML format
4. **Integration**: Seamless integration with storage layer, registry, and topology
5. **Performance**: <50ms latency for small object GET/PUT
6. **Standards**: Follow AWS S3 API specification

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                       HTTP Request                           │
│  GET /bucket/object?versionId=xyz HTTP/1.1                 │
│  Authorization: AWS4-HMAC-SHA256 Credential=...            │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                    S3 Request Router                         │
│  - Parse S3 path (bucket/object)                            │
│  - Extract query parameters                                 │
│  - Route to handler                                         │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                 Authentication Layer                         │
│  - Verify AWS Signature V4                                  │
│  - Check access keys                                        │
│  - Validate permissions                                     │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                   S3 Operation Handler                       │
│  - PUT Object: Write to storage layer                       │
│  - GET Object: Read from storage layer                      │
│  - DELETE Object: Remove from storage                       │
│  - HEAD Object: Metadata only                               │
│  - LIST Objects: Registry query                             │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                    Storage Integration                       │
│  - Use buckets_object_* APIs                                │
│  - Update location registry                                 │
│  - Handle versioning                                        │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                      XML Response                            │
│  <?xml version="1.0" encoding="UTF-8"?>                    │
│  <CompleteMultipartUploadResult>...</>                     │
└─────────────────────────────────────────────────────────────┘
```

---

## Week-by-Week Plan

### Week 35: Core PUT/GET Operations ⏳
- S3 request parsing (bucket/object path)
- Basic AWS Signature V4 authentication
- PUT Object operation
- GET Object operation
- XML response generation
- 10 tests minimum

### Week 36: DELETE/HEAD Operations
- DELETE Object with versioning
- HEAD Object (metadata only)
- Error responses (NoSuchBucket, NoSuchKey, etc.)
- 8 tests

### Week 37: Bucket Operations
- PUT Bucket (create)
- DELETE Bucket
- HEAD Bucket
- LIST Buckets
- 10 tests

### Week 38: LIST Objects
- LIST Objects v1
- LIST Objects v2
- Pagination (max-keys, continuation-token)
- Prefix filtering
- 12 tests

### Week 39: Multipart Upload (Part 1)
- Initiate Multipart Upload
- Upload Part
- Part tracking and storage
- 8 tests

### Week 40: Multipart Upload (Part 2)
- Complete Multipart Upload
- Abort Multipart Upload
- List Parts
- 10 tests

### Week 41: Versioning & Metadata
- Object versioning support
- Object tagging
- Custom metadata (x-amz-meta-*)
- 10 tests

### Week 42: Integration & Testing
- End-to-end S3 workflows
- MinIO mc client compatibility
- s3cmd compatibility
- Performance benchmarks
- 15 tests

---

## S3 API Endpoints

### Object Operations

| Method | Path | Operation | Status |
|--------|------|-----------|--------|
| PUT | `/{bucket}/{key}` | Put Object | Week 35 |
| GET | `/{bucket}/{key}` | Get Object | Week 35 |
| DELETE | `/{bucket}/{key}` | Delete Object | Week 36 |
| HEAD | `/{bucket}/{key}` | Head Object | Week 36 |
| POST | `/{bucket}/{key}?uploads` | Initiate Multipart | Week 39 |
| PUT | `/{bucket}/{key}?uploadId={id}&partNumber={n}` | Upload Part | Week 39 |
| POST | `/{bucket}/{key}?uploadId={id}` | Complete Multipart | Week 40 |
| DELETE | `/{bucket}/{key}?uploadId={id}` | Abort Multipart | Week 40 |

### Bucket Operations

| Method | Path | Operation | Status |
|--------|------|-----------|--------|
| PUT | `/{bucket}` | Create Bucket | Week 37 |
| DELETE | `/{bucket}` | Delete Bucket | Week 37 |
| HEAD | `/{bucket}` | Head Bucket | Week 37 |
| GET | `/` | List Buckets | Week 37 |
| GET | `/{bucket}` | List Objects v1 | Week 38 |
| GET | `/{bucket}?list-type=2` | List Objects v2 | Week 38 |

---

## Week 35: Implementation Details

### S3 Request Structure

```c
typedef struct {
    char bucket[256];          /* Bucket name */
    char key[1024];            /* Object key */
    char version_id[64];       /* Version ID (optional) */
    char *body;                /* Request body */
    size_t body_len;           /* Body length */
    
    /* Headers */
    char content_type[128];
    char content_md5[64];
    i64 content_length;
    
    /* Auth */
    char access_key[128];
    char signature[256];
    
    /* Query params */
    char **query_params;
    int query_count;
} buckets_s3_request_t;
```

### S3 Response Structure

```c
typedef struct {
    int status_code;           /* HTTP status (200, 404, etc.) */
    char *xml_body;            /* XML response body */
    size_t body_len;           /* Body length */
    
    /* Headers */
    char etag[64];             /* Object ETag */
    i64 content_length;
    char content_type[128];
    char last_modified[64];    /* RFC 2822 format */
} buckets_s3_response_t;
```

### PUT Object Flow

1. **Parse Request**:
   - Extract bucket and key from path
   - Parse headers (Content-Type, Content-MD5, x-amz-*)
   - Read request body

2. **Authenticate**:
   - Verify AWS Signature V4
   - Check access key exists
   - Validate signature matches

3. **Write Object**:
   - Use `buckets_object_write()` from storage layer
   - Calculate ETag (MD5 or SHA256)
   - Update location registry

4. **Return Response**:
   - HTTP 200 OK
   - ETag header
   - Empty body (or minimal XML)

### GET Object Flow

1. **Parse Request**:
   - Extract bucket and key
   - Parse query params (versionId, range, etc.)

2. **Authenticate**:
   - Verify signature

3. **Read Object**:
   - Query location registry for object location
   - Use `buckets_object_read()` from storage layer
   - Handle range requests if specified

4. **Return Response**:
   - HTTP 200 OK
   - Object body
   - Headers: Content-Type, Content-Length, ETag, Last-Modified

### XML Response Format

**Success Response (PUT Object)**:
```xml
<?xml version="1.0" encoding="UTF-8"?>
<PutObjectResult>
    <ETag>"abc123..."</ETag>
    <VersionId>xyz789</VersionId>
</PutObjectResult>
```

**Error Response**:
```xml
<?xml version="1.0" encoding="UTF-8"?>
<Error>
    <Code>NoSuchBucket</Code>
    <Message>The specified bucket does not exist</Message>
    <Resource>/mybucket/myobject</Resource>
    <RequestId>12345</RequestId>
</Error>
```

---

## Authentication

### AWS Signature Version 4

**Authorization Header Format**:
```
Authorization: AWS4-HMAC-SHA256 
Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request,
SignedHeaders=host;range;x-amz-date,
Signature=fe5f80f77d5fa3beca038a248ff027d0445342fe2855ddc963176630326f1024
```

**Signature Calculation**:
1. Create canonical request (method, URI, query, headers, payload hash)
2. Create string to sign (algorithm, date, credential scope, hashed canonical request)
3. Calculate signing key (HMAC-SHA256 chain)
4. Calculate signature (HMAC-SHA256 of string to sign)

**Implementation Notes**:
- Use OpenSSL for HMAC-SHA256
- Support both header-based and query-based auth
- Cache signing keys per access key (5-minute TTL)

---

## Error Codes

Common S3 error codes to implement:

| Code | HTTP Status | Description |
|------|-------------|-------------|
| NoSuchBucket | 404 | Bucket does not exist |
| NoSuchKey | 404 | Object does not exist |
| BucketAlreadyExists | 409 | Bucket name already taken |
| InvalidBucketName | 400 | Invalid bucket name |
| InvalidObjectName | 400 | Invalid object key |
| AccessDenied | 403 | Authentication failed |
| SignatureDoesNotMatch | 403 | Invalid signature |
| EntityTooLarge | 400 | Object exceeds size limit |
| InternalError | 500 | Server error |

---

## Performance Targets

### Week 35 Targets:
- PUT Object (1KB): <50ms
- GET Object (1KB): <10ms (cache hit), <50ms (cache miss)
- Authentication overhead: <5ms
- XML generation: <1ms

### Future Optimizations:
- Connection pooling (already implemented in Phase 8)
- Request pipelining
- Metadata caching
- Object caching for small objects (<10KB)

---

## Testing Strategy

### Unit Tests (Week 35):
1. S3 request parsing (bucket/key extraction)
2. XML response generation
3. Authentication signature verification
4. PUT Object success
5. GET Object success
6. PUT Object with invalid bucket
7. GET Object not found
8. Authentication failure
9. Content-Type handling
10. ETag generation

### Integration Tests (Week 42):
- MinIO mc client compatibility
- AWS SDK compatibility
- s3cmd compatibility
- Performance benchmarks

---

## Dependencies

### External Libraries:
- **libxml2** or custom XML parser (TBD - may implement lightweight parser)
- **OpenSSL**: Already integrated (for HMAC-SHA256)
- **cJSON**: Already integrated (for internal formats)

### Internal Dependencies:
- Storage layer: `buckets_object_*` APIs
- Location registry: `buckets_registry_*` APIs
- Network layer: HTTP server from Phase 8
- Topology: Bucket placement decisions

---

## File Structure

```
src/s3/
├── s3_handler.c       - Main S3 request handler
├── s3_auth.c          - AWS Signature V4 authentication
├── s3_ops.c           - Object operations (PUT/GET/DELETE)
├── s3_bucket.c        - Bucket operations
├── s3_xml.c           - XML request/response parsing
└── s3_multipart.c     - Multipart upload (Weeks 39-40)

include/
└── buckets_s3.h       - S3 API types and functions

tests/s3/
├── test_s3_parse.c    - Request parsing tests
├── test_s3_auth.c     - Authentication tests
├── test_s3_ops.c      - Object operation tests
└── test_s3_xml.c      - XML generation tests
```

---

## References

- [AWS S3 API Reference](https://docs.aws.amazon.com/AmazonS3/latest/API/)
- [AWS Signature Version 4](https://docs.aws.amazon.com/general/latest/gr/signature-version-4.html)
- [MinIO S3 Compatibility](https://min.io/docs/minio/linux/developers/s3-compatible.html)

---

**Next Update**: End of Week 35 (after PUT/GET implementation)
