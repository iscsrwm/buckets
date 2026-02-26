# Network Layer Architecture

**Version**: 1.0  
**Phase**: 8 (Weeks 31-34)  
**Status**: Planning  
**Last Updated**: February 25, 2026

---

## Overview

The Network Layer provides HTTP/S server capabilities and peer-to-peer communication for the Buckets distributed object storage system. It enables:

1. **HTTP/S Server**: REST API endpoint for S3-compatible operations
2. **Peer Communication**: RPC system for inter-node coordination
3. **Health Checking**: Monitoring and failure detection
4. **Connection Management**: Pooling and reuse for efficiency

---

## Goals

### Primary Goals
- Serve S3 API requests over HTTP/S
- Enable peer-to-peer communication for topology sync
- Provide health checking and monitoring
- Maintain low-latency RPC (<1ms local, <10ms remote)
- Support connection pooling and reuse

### Non-Goals (Deferred)
- Load balancing (handled externally)
- DDoS protection (handled externally)
- Rate limiting (Phase 9+)
- Authentication (Phase 9+)

---

## Architecture

### Components

```
┌─────────────────────────────────────────────────────────────┐
│                    Network Layer                            │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │
│  │ HTTP Server  │  │ Peer Grid    │  │ Health Check │     │
│  │              │  │              │  │              │     │
│  │ - Routing    │  │ - Discovery  │  │ - Heartbeat  │     │
│  │ - TLS        │  │ - RPC        │  │ - Failure    │     │
│  │ - Handlers   │  │ - Broadcast  │  │ - Recovery   │     │
│  └──────────────┘  └──────────────┘  └──────────────┘     │
│         │                  │                  │            │
│         └──────────────────┴──────────────────┘            │
│                            │                               │
│                   ┌────────▼─────────┐                     │
│                   │ Connection Pool  │                     │
│                   │                  │                     │
│                   │ - TCP Sockets    │                     │
│                   │ - Reuse          │                     │
│                   │ - Timeouts       │                     │
│                   └──────────────────┘                     │
└─────────────────────────────────────────────────────────────┘
```

### Data Flow

**HTTP Request Flow**:
```
Client → HTTP Server → Router → Handler → Storage/Registry → Response
```

**Peer RPC Flow**:
```
Node A → RPC Call → Peer Grid → Node B → Handler → Response
```

**Health Check Flow**:
```
Node → Heartbeat → All Peers → Collect Responses → Update Status
```

---

## Week 31: HTTP Server Foundation

### Objectives
1. Evaluate and select HTTP server library
2. Implement basic HTTP/1.1 server
3. Add request routing framework
4. Write comprehensive tests

### Library Evaluation

**Candidates**:
1. **libmicrohttpd** - GNU project, mature, C89
2. **mongoose** - Embedded web server, C99, MIT license
3. **h2o** - High-performance, HTTP/2, complex

**Evaluation Criteria**:
- C11 compatibility
- License (prefer permissive)
- Dependencies (minimal preferred)
- Performance (<1ms overhead)
- TLS support
- Community activity
- Documentation quality

**Recommended**: **mongoose**
- ✅ C99 compatible (works with C11)
- ✅ MIT license (permissive)
- ✅ Single-file library (easy integration)
- ✅ Built-in TLS support
- ✅ Active development
- ✅ Good documentation
- ✅ Used in production systems

### Implementation Plan

**Files to Create**:
```
include/buckets_net.h          - Network layer API
src/net/http_server.c          - HTTP server implementation
src/net/router.c               - Request routing
tests/net/test_http_server.c   - Server tests
tests/net/test_router.c        - Routing tests
third_party/mongoose/          - Library (single file)
```

**API Design**:
```c
// HTTP Server
typedef struct buckets_http_server buckets_http_server_t;
typedef void (*buckets_http_handler_t)(buckets_http_request_t*, 
                                        buckets_http_response_t*);

buckets_http_server_t* buckets_http_server_create(const char *addr, int port);
int buckets_http_server_start(buckets_http_server_t *server);
int buckets_http_server_stop(buckets_http_server_t *server);
void buckets_http_server_free(buckets_http_server_t *server);

// Routing
typedef struct buckets_router buckets_router_t;

buckets_router_t* buckets_router_create(void);
int buckets_router_add_route(buckets_router_t *router, 
                              const char *method, 
                              const char *path, 
                              buckets_http_handler_t handler);
buckets_http_handler_t buckets_router_match(buckets_router_t *router,
                                              const char *method,
                                              const char *path);
void buckets_router_free(buckets_router_t *router);
```

**Request/Response Structures**:
```c
typedef struct {
    const char *method;        // GET, PUT, DELETE, etc.
    const char *path;          // /bucket/object
    const char *query_string;  // key=value&...
    const char *body;          // Request body
    size_t body_len;           // Body length
    // Headers (hash table)
    void *headers;             // Internal
} buckets_http_request_t;

typedef struct {
    int status_code;           // 200, 404, 500, etc.
    const char *body;          // Response body
    size_t body_len;           // Body length
    // Headers (hash table)
    void *headers;             // Internal
} buckets_http_response_t;
```

### Week 31 Deliverables
- HTTP server with mongoose integration (~300 lines)
- Router with path matching (~200 lines)
- Request/Response handling (~150 lines)
- Tests (10-15 tests, ~400 lines)
- **Total**: ~1,050 lines

---

## Week 32: TLS and Connection Pooling

### TLS Integration

**Objectives**:
- Enable HTTPS support
- Certificate loading
- Secure connections

**Implementation**:
```c
typedef struct {
    const char *cert_file;     // Server certificate
    const char *key_file;      // Private key
    const char *ca_file;       // CA bundle (optional)
} buckets_tls_config_t;

int buckets_http_server_enable_tls(buckets_http_server_t *server,
                                     buckets_tls_config_t *config);
```

### Connection Pooling

**Objectives**:
- Reuse TCP connections
- Configurable pool size
- Timeout management

**Implementation**:
```c
typedef struct buckets_conn_pool buckets_conn_pool_t;

buckets_conn_pool_t* buckets_conn_pool_create(int max_conns);
int buckets_conn_pool_get(buckets_conn_pool_t *pool, 
                           const char *host, 
                           int port,
                           buckets_connection_t **conn);
int buckets_conn_pool_release(buckets_conn_pool_t *pool,
                               buckets_connection_t *conn);
void buckets_conn_pool_free(buckets_conn_pool_t *pool);
```

### Week 32 Deliverables
- TLS support (~200 lines)
- Connection pool (~300 lines)
- Tests (10 tests, ~350 lines)
- **Total**: ~850 lines

---

## Week 33: Peer Discovery and Health Checking

### Peer Discovery

**Objectives**:
- Discover peers in cluster
- Maintain peer list
- Handle peer joins/leaves

**Implementation**:
```c
typedef struct {
    char node_id[64];          // Unique node ID
    char endpoint[256];        // http://host:port
    bool online;               // Peer status
    time_t last_seen;          // Last heartbeat
} buckets_peer_t;

typedef struct buckets_peer_grid buckets_peer_grid_t;

buckets_peer_grid_t* buckets_peer_grid_create(void);
int buckets_peer_grid_add_peer(buckets_peer_grid_t *grid, 
                                 const char *endpoint);
int buckets_peer_grid_remove_peer(buckets_peer_grid_t *grid,
                                    const char *node_id);
buckets_peer_t** buckets_peer_grid_get_peers(buckets_peer_grid_t *grid,
                                               int *count);
void buckets_peer_grid_free(buckets_peer_grid_t *grid);
```

### Health Checking

**Objectives**:
- Periodic heartbeats
- Failure detection
- Automatic recovery

**Implementation**:
```c
typedef void (*buckets_health_callback_t)(const char *node_id,
                                            bool online,
                                            void *user_data);

typedef struct buckets_health_checker buckets_health_checker_t;

buckets_health_checker_t* buckets_health_checker_create(
    buckets_peer_grid_t *grid,
    int interval_sec);
    
int buckets_health_checker_start(buckets_health_checker_t *checker);
int buckets_health_checker_stop(buckets_health_checker_t *checker);
int buckets_health_checker_set_callback(buckets_health_checker_t *checker,
                                          buckets_health_callback_t callback,
                                          void *user_data);
void buckets_health_checker_free(buckets_health_checker_t *checker);
```

### Week 33 Deliverables
- Peer grid (~250 lines)
- Health checker (~300 lines)
- Tests (12 tests, ~400 lines)
- **Total**: ~950 lines

---

## Week 34: RPC and Broadcast

### RPC Message Format

**Objectives**:
- Define RPC message structure
- Serialization (JSON)
- Request/response handling

**Message Format** (JSON):
```json
{
  "id": "uuid-v4",
  "method": "topology.update",
  "params": {
    "generation": 42,
    "topology": {...}
  },
  "timestamp": 1708896000
}
```

**Response Format**:
```json
{
  "id": "uuid-v4",
  "result": {...},
  "error": null,
  "timestamp": 1708896001
}
```

**Implementation**:
```c
typedef struct {
    char id[64];               // Request ID
    char method[128];          // RPC method
    cJSON *params;             // Parameters
    time_t timestamp;          // Request time
} buckets_rpc_request_t;

typedef struct {
    char id[64];               // Request ID
    cJSON *result;             // Result data
    buckets_error_t error;     // Error code
    time_t timestamp;          // Response time
} buckets_rpc_response_t;

typedef buckets_rpc_response_t* (*buckets_rpc_handler_t)(
    buckets_rpc_request_t *req);

int buckets_rpc_call(const char *peer_endpoint,
                      const char *method,
                      cJSON *params,
                      buckets_rpc_response_t **response);
                      
int buckets_rpc_register_handler(const char *method,
                                   buckets_rpc_handler_t handler);
```

### Broadcast Primitives

**Objectives**:
- Send to all peers
- Collect responses
- Handle failures

**Implementation**:
```c
typedef struct {
    int total;                 // Total peers
    int success;               // Successful responses
    int failed;                // Failed peers
    buckets_rpc_response_t **responses;
} buckets_broadcast_result_t;

int buckets_rpc_broadcast(buckets_peer_grid_t *grid,
                           const char *method,
                           cJSON *params,
                           buckets_broadcast_result_t **result);
                           
void buckets_broadcast_result_free(buckets_broadcast_result_t *result);
```

### Week 34 Deliverables
- RPC implementation (~400 lines)
- Broadcast system (~200 lines)
- Tests (15 tests, ~500 lines)
- **Total**: ~1,100 lines

---

## Testing Strategy

### Unit Tests
- HTTP server creation/destruction
- Request routing
- TLS configuration
- Connection pool operations
- Peer discovery
- Health checking
- RPC serialization/deserialization
- Broadcast operations

### Integration Tests
- End-to-end HTTP request/response
- TLS handshake
- Multi-peer RPC
- Failure detection and recovery
- Broadcast with partial failures

### Performance Tests
- HTTP request latency (<1ms overhead)
- RPC latency (<1ms local, <10ms remote)
- Connection pool efficiency
- Concurrent request handling
- Broadcast scalability

---

## Performance Targets

| Metric | Target | Rationale |
|--------|--------|-----------|
| HTTP request overhead | <1ms | Minimize latency |
| Local RPC latency | <1ms | Fast inter-thread communication |
| Remote RPC latency | <10ms | Network RTT + processing |
| Connections per node | 100+ | Support multi-peer clusters |
| Concurrent requests | 10,000+ | Handle high load |
| Health check interval | 5s | Balance overhead vs detection |
| Heartbeat timeout | 15s | 3× interval for reliability |

---

## Dependencies

### Required Libraries
- **mongoose**: HTTP server (MIT license)
  - Single-file library
  - ~6,000 lines of C
  - TLS support via OpenSSL

### Existing Dependencies (Already Available)
- OpenSSL: TLS/SSL
- cJSON: JSON serialization
- libuuid: UUID generation
- pthreads: Threading

---

## Error Handling

### HTTP Errors
- 400 Bad Request: Malformed requests
- 404 Not Found: Unknown routes
- 500 Internal Server Error: Server failures
- 503 Service Unavailable: Overload

### RPC Errors
- `BUCKETS_ERR_RPC_TIMEOUT`: No response within timeout
- `BUCKETS_ERR_RPC_PEER_DOWN`: Peer unreachable
- `BUCKETS_ERR_RPC_INVALID`: Malformed message
- `BUCKETS_ERR_RPC_FAILED`: Handler error

### Health Check Errors
- Heartbeat timeout: Mark peer offline
- Consecutive failures: Remove from grid
- Recovery: Automatic re-add on heartbeat

---

## Security Considerations

### Week 31-32 (Foundation + TLS)
- TLS 1.2+ required for production
- Certificate validation
- Secure defaults (no SSLv3, weak ciphers)

### Future Phases (9+)
- Authentication (S3 signatures)
- Authorization (bucket policies)
- Rate limiting
- DDoS protection

---

## Phase 8 Summary

### Total Estimated Code
- Week 31: ~1,050 lines (HTTP server + routing)
- Week 32: ~850 lines (TLS + connection pool)
- Week 33: ~950 lines (peer grid + health check)
- Week 34: ~1,100 lines (RPC + broadcast)
- **Total**: ~3,950 lines (2,000 prod + 1,950 tests)

### Total Tests
- Week 31: 15 tests
- Week 32: 10 tests
- Week 33: 12 tests
- Week 34: 15 tests
- **Total**: ~52 tests

### Milestones
1. ✅ Library selection (mongoose)
2. ⏳ HTTP server working (Week 31)
3. ⏳ TLS enabled (Week 32)
4. ⏳ Peer communication (Week 33-34)
5. ⏳ Production-ready network layer (End of Week 34)

---

## References

- MinIO Grid System: `cmd/grid.go`, `cmd/grid-rpc.go`
- MinIO HTTP Server: `cmd/http/server.go`
- mongoose documentation: https://mongoose.ws/
- HTTP/1.1 RFC: https://tools.ietf.org/html/rfc2616

---

**Status**: Phase 8 planning complete  
**Next**: Week 31 implementation (HTTP server foundation)
