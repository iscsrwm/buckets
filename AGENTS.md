# Buckets Development Guide for AI Coding Agents

**Project**: Buckets - High-Performance S3-Compatible Object Storage in C  
**Language**: C11  
**Architecture**: See `architecture/SCALE_AND_DATA_PLACEMENT.md`

---

## 📍 Current Status (Week 31 - February 25, 2026)

**Phase**: Phase 8 - Network Layer (Weeks 31-34) - IN PROGRESS  
**Progress**: 60% complete (31 of 52 weeks)  
**Completed Phases**: 7 (Foundation, Hashing, Crypto/Erasure, Storage, Registry, Topology, Migration)

**Recent Completion**: Week 31 - HTTP Server Foundation ✅
- HTTP/1.1 server with mongoose library
- Request router with pattern matching
- 21 network tests passing (100%)
- 781 lines of production code, 614 lines of tests

**Test Status**: 275/276 tests passing (99.6%)  
**Code Metrics**:
- Production: 14,640 lines (+781)
- Tests: 8,468 lines (+614)
- Total: 23,718 lines (+1,395)

**Latest Commits** (will be updated after commit):
- Week 31 implementation (HTTP server, router, mongoose integration)
- All Phase 7 weeks complete (migration engine)

**Next Steps**: Week 32 - TLS support and connection pooling

---

## 📝 IMPORTANT: Documentation Updates

**ALWAYS update `docs/PROJECT_STATUS.md` after completing significant work:**

1. **After completing a major component** (e.g., format.c, topology.c):
   - Update the "Completed" section with implementation details
   - Add line counts and file information
   - Document any design decisions made

2. **After completing tests**:
   - Update test metrics (number of tests, passing/failing)
   - Add test coverage information

3. **After fixing bugs**:
   - Document the bug and the fix in the relevant section

4. **At the end of each session**:
   - Update progress metrics (percentage complete, lines of code)
   - Update "In Progress" and "Next Steps" sections
   - Add any new decisions to the "Design Decisions Log"

5. **When completing a week**:
   - Add a comprehensive week summary section
   - Update the roadmap progress

**Quick check before ending a session**: Have you updated PROJECT_STATUS.md?

---

## Build Commands

```bash
# Full build (library + server binary)
make

# Build specific components
make core          # Core data structures only
make hash          # Hashing algorithms
make registry      # Location registry
make storage       # Storage layer

# Build library only
make libbuckets    # Creates build/libbuckets.a and build/libbuckets.so

# Build server binary
make buckets       # Creates bin/buckets

# Clean build
make clean

# Debug build with sanitizers
make debug         # Adds -g -O0 -fsanitize=address -fsanitize=undefined

# Profile build
make profile       # Adds -pg for gprof profiling
```

## Test Commands

```bash
# Run all tests
make test

# Run specific component tests
make test-format    # Format management (20 tests)
make test-topology  # Topology management (18 tests)
make test-topology-operations  # Topology operations (8 tests)
make test-topology-quorum      # Topology quorum (12 tests)
make test-topology-manager     # Topology manager (11 tests)
make test-topology-integration # Topology integration (9 tests)
make test-hash      # Hashing algorithms (49 tests: siphash, xxhash, ring)
make test-crypto    # Cryptography (28 tests: blake2b, sha256)
make test-erasure   # Erasure coding (20 tests)
make test-storage   # Storage layer (18 tests)
make test-scanner   # Migration scanner (10 tests)
make test-worker    # Migration worker pool (12 tests)
make test-orchestrator   # Migration orchestrator (14 tests)
make test-throttle       # Migration throttling (15 tests)
make test-checkpoint     # Migration checkpointing (10 tests)
make test-integration    # Migration integration (10 tests)

# Compile and run individual test suites
mkdir -p build/test/{cluster,hash,crypto,erasure,storage}

# Cluster tests
gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic \
    -Iinclude -Isrc -Ithird_party/cJSON \
    tests/cluster/test_format.c build/libbuckets.a \
    -o build/test/cluster/test_format \
    -lcriterion -lssl -lcrypto -luuid -lz -lpthread -lm
./build/test/cluster/test_format

# Hash tests
gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic \
    -Iinclude -Isrc -Ithird_party/cJSON \
    tests/hash/test_siphash.c build/libbuckets.a \
    -o build/test/hash/test_siphash \
    -lcriterion -lssl -lcrypto -luuid -lz -lpthread -lm
./build/test/hash/test_siphash

# Crypto tests
gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic \
    -Iinclude -Isrc -Ithird_party/cJSON \
    tests/crypto/test_blake2b.c build/libbuckets.a \
    -o build/test/crypto/test_blake2b \
    -lcriterion -lssl -lcrypto -luuid -lz -lpthread -lm
./build/test/crypto/test_blake2b

# Run tests with Valgrind (memory leak detection)
make test-valgrind

# Run tests with verbose output
VERBOSE=1 make test
```

## Lint & Analysis

```bash
# Format code (clang-format)
make format

# Static analysis (clang-tidy)
make analyze

# Check style compliance
clang-format --dry-run --Werror src/**/*.c include/**/*.h
```

---

## Code Style Guidelines

### File Organization

**Headers** (`include/`):
```c
/**
 * Brief description
 * 
 * Detailed description
 */

#ifndef BUCKETS_COMPONENT_H
#define BUCKETS_COMPONENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
// System headers first, alphabetically

#include "buckets.h"
// Project headers second, alphabetically

// Type definitions
// Function declarations
// Macros

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_COMPONENT_H */
```

**Source Files** (`src/`):
```c
/**
 * Component Implementation
 * 
 * Detailed description
 */

#include <stdio.h>      // System headers first
#include <stdlib.h>
#include <string.h>

#include "buckets.h"    // Project headers
#include "component.h"

// Static/private functions first
// Public API implementations last
```

### Naming Conventions

- **Functions**: `buckets_component_action()` - snake_case with `buckets_` prefix
- **Types**: `buckets_type_t` - snake_case with `_t` suffix
- **Structs**: `buckets_struct_name` - snake_case
- **Enums**: `BUCKETS_ENUM_VALUE` - UPPER_CASE
- **Macros**: `BUCKETS_MACRO_NAME` - UPPER_CASE
- **Static functions**: `static_function()` - no prefix
- **Global variables**: `g_variable_name` - `g_` prefix
- **Constants**: `BUCKETS_CONSTANT` - UPPER_CASE

### Types & Declarations

Use project-defined short types from `buckets.h`:
```c
u8, u16, u32, u64        // Unsigned integers
i8, i16, i32, i64        // Signed integers
bool                      // Boolean (true/false)
size_t                    // Standard library size type
```

Pointer declarations - asterisk with type:
```c
char *str;               // Correct
char* str;               // Avoid
```

### Formatting

- **Indentation**: 4 spaces (no tabs)
- **Line length**: 100 characters max
- **Braces**: K&R style (opening brace on same line, except functions)
```c
// Functions - opening brace on new line
void buckets_function(void)
{
    // body
}

// Control structures - opening brace on same line
if (condition) {
    // body
} else {
    // body
}
```

### Error Handling

Always check return values and handle errors:
```c
// Pattern 1: Return error codes
buckets_error_t buckets_do_something(void) {
    if (error_condition) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    return BUCKETS_OK;
}

// Pattern 2: Use buckets_result_t for data + error
buckets_result_t buckets_get_data(void) {
    buckets_result_t result;
    result.data = NULL;
    result.error = BUCKETS_OK;
    
    if (error_condition) {
        result.error = BUCKETS_ERR_NOMEM;
        return result;
    }
    
    result.data = allocated_data;
    return result;
}

// Pattern 3: NULL for pointer returns (set errno if needed)
void* buckets_allocate(size_t size) {
    void *ptr = malloc(size);
    if (!ptr && size > 0) {
        buckets_error("Allocation failed");
        return NULL;
    }
    return ptr;
}
```

### Memory Management

- Always use `buckets_malloc/calloc/realloc/free` wrappers (never raw malloc)
- Check all allocations (wrappers abort on OOM)
- Free in reverse order of allocation
- Set pointers to NULL after freeing
- Document ownership in function comments

### Logging

Use logging macros from `buckets.h`:
```c
buckets_debug("Debug message: %d", value);    // Development only
buckets_info("Informational: %s", str);       // Important events
buckets_warn("Warning: %s", message);         // Recoverable issues
buckets_error("Error: %s", error_msg);        // Error conditions
buckets_fatal("Fatal: %s", fatal_msg);        // Unrecoverable (exits)
```

---

## Architecture Rules

1. **No external dependencies** - Self-contained design (only libc, OpenSSL, zlib, libuuid, ISA-L)
2. **Component isolation** - Each `src/component/` is independent
3. **Public API only in headers** - Use `static` for internal functions
4. **Memory safety first** - Use sanitizers during development
5. **Performance critical** - Profile before optimizing, document trade-offs
6. **Document architecture decisions** - Add to `architecture/*.md` when making significant design choices
7. **Algorithm selection rationale** - Document why specific algorithms/libraries were chosen

## Testing Requirements

- Every public function MUST have unit tests
- Test coverage target: >85%
- Test files mirror source structure: `tests/component/test_feature.c`
- Use descriptive test names: `test_vector_push_increases_size()`
- Test edge cases: NULL inputs, zero sizes, boundary conditions

## Reference Code

MinIO Go code is in `/home/a002687/minio-reference/` for reference:
- `cmd/erasure-sets.go` - Current hash placement
- `cmd/format-erasure.go` - Disk format structures
- `cmd/xl-storage-format-v2.go` - Object metadata

When porting from MinIO, preserve logic but adapt to C idioms.

---

## 📋 End of Session Checklist

Before concluding any work session, ensure you have:

1. ✅ **Updated `docs/PROJECT_STATUS.md`** with:
   - New completed items
   - Progress metrics (lines of code, files created)
   - Test results if applicable
   - Any design decisions made
   - Current status and next steps

2. ✅ **Committed changes** (if requested):
   - Meaningful commit message
   - All files staged properly

3. ✅ **Documented any issues**:
   - Known bugs or limitations
   - TODOs for future work
   - Open questions

**Remember**: Documentation is as important as code. Keep PROJECT_STATUS.md current!

---

**For detailed architecture**: See `architecture/*.md`  
**For development roadmap**: See `ROADMAP.md`  
**For project status**: See `docs/PROJECT_STATUS.md` ⬅️ **UPDATE THIS REGULARLY**
