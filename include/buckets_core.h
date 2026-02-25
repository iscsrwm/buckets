/**
 * Buckets Core Data Structures
 * 
 * Fundamental data structures: vectors, hash tables, lists, buffers
 */

#ifndef BUCKETS_CORE_H
#define BUCKETS_CORE_H

#include "buckets.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Dynamic Array (Vector) ===== */

typedef struct buckets_vector {
    void **data;
    size_t size;
    size_t capacity;
} buckets_vector_t;

buckets_vector_t* buckets_vector_new(size_t initial_capacity);
void buckets_vector_free(buckets_vector_t *vec);
int buckets_vector_push(buckets_vector_t *vec, void *item);
void* buckets_vector_pop(buckets_vector_t *vec);
void* buckets_vector_get(buckets_vector_t *vec, size_t index);
int buckets_vector_set(buckets_vector_t *vec, size_t index, void *item);
void buckets_vector_clear(buckets_vector_t *vec);
size_t buckets_vector_size(buckets_vector_t *vec);

/* ===== Hash Table ===== */

typedef u64 (*buckets_hash_fn)(const void *key, size_t len);
typedef int (*buckets_cmp_fn)(const void *a, const void *b);
typedef void (*buckets_free_fn)(void *data);

typedef struct buckets_hash_entry {
    void *key;
    void *value;
    u64 hash;
    struct buckets_hash_entry *next;
} buckets_hash_entry_t;

typedef struct buckets_hash_table {
    buckets_hash_entry_t **buckets;
    size_t num_buckets;
    size_t num_entries;
    buckets_hash_fn hash_fn;
    buckets_cmp_fn cmp_fn;
    buckets_free_fn key_free;
    buckets_free_fn val_free;
} buckets_hash_table_t;

buckets_hash_table_t* buckets_hash_table_new(
    size_t initial_size,
    buckets_hash_fn hash_fn,
    buckets_cmp_fn cmp_fn
);
void buckets_hash_table_free(buckets_hash_table_t *ht);
int buckets_hash_table_insert(buckets_hash_table_t *ht, void *key, void *value);
void* buckets_hash_table_get(buckets_hash_table_t *ht, const void *key);
int buckets_hash_table_remove(buckets_hash_table_t *ht, const void *key);
bool buckets_hash_table_contains(buckets_hash_table_t *ht, const void *key);
size_t buckets_hash_table_size(buckets_hash_table_t *ht);

/* Hash table iteration */
typedef struct buckets_hash_iter {
    buckets_hash_table_t *ht;
    size_t bucket_idx;
    buckets_hash_entry_t *current;
} buckets_hash_iter_t;

buckets_hash_iter_t buckets_hash_iter_new(buckets_hash_table_t *ht);
bool buckets_hash_iter_next(buckets_hash_iter_t *iter, void **key, void **value);

/* ===== Linked List ===== */

typedef struct buckets_list_node {
    void *data;
    struct buckets_list_node *next;
    struct buckets_list_node *prev;
} buckets_list_node_t;

typedef struct buckets_list {
    buckets_list_node_t *head;
    buckets_list_node_t *tail;
    size_t size;
    buckets_free_fn free_fn;
} buckets_list_t;

buckets_list_t* buckets_list_new(buckets_free_fn free_fn);
void buckets_list_free(buckets_list_t *list);
int buckets_list_push_front(buckets_list_t *list, void *data);
int buckets_list_push_back(buckets_list_t *list, void *data);
void* buckets_list_pop_front(buckets_list_t *list);
void* buckets_list_pop_back(buckets_list_t *list);
void* buckets_list_get(buckets_list_t *list, size_t index);
int buckets_list_remove(buckets_list_t *list, void *data);
size_t buckets_list_size(buckets_list_t *list);

/* ===== Ring Buffer ===== */

typedef struct buckets_ring_buffer {
    void **data;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    size_t size;
} buckets_ring_buffer_t;

buckets_ring_buffer_t* buckets_ring_buffer_new(size_t capacity);
void buckets_ring_buffer_free(buckets_ring_buffer_t *rb);
int buckets_ring_buffer_write(buckets_ring_buffer_t *rb, void *item);
void* buckets_ring_buffer_read(buckets_ring_buffer_t *rb);
bool buckets_ring_buffer_is_full(buckets_ring_buffer_t *rb);
bool buckets_ring_buffer_is_empty(buckets_ring_buffer_t *rb);
size_t buckets_ring_buffer_size(buckets_ring_buffer_t *rb);

/* ===== Memory Pool ===== */

typedef struct buckets_pool_chunk {
    void *memory;
    struct buckets_pool_chunk *next;
} buckets_pool_chunk_t;

typedef struct buckets_pool {
    size_t block_size;
    size_t blocks_per_chunk;
    buckets_pool_chunk_t *chunks;
    void **free_list;
    size_t free_count;
} buckets_pool_t;

buckets_pool_t* buckets_pool_new(size_t block_size, size_t blocks_per_chunk);
void buckets_pool_free(buckets_pool_t *pool);
void* buckets_pool_alloc(buckets_pool_t *pool);
void buckets_pool_dealloc(buckets_pool_t *pool, void *ptr);

/* ===== Byte Buffer ===== */

typedef struct buckets_buffer {
    u8 *data;
    size_t size;
    size_t capacity;
    size_t read_pos;
} buckets_buffer_t;

buckets_buffer_t* buckets_buffer_new(size_t initial_capacity);
void buckets_buffer_free(buckets_buffer_t *buf);
int buckets_buffer_write(buckets_buffer_t *buf, const u8 *data, size_t len);
int buckets_buffer_read(buckets_buffer_t *buf, u8 *dest, size_t len);
int buckets_buffer_resize(buckets_buffer_t *buf, size_t new_capacity);
void buckets_buffer_reset(buckets_buffer_t *buf);
size_t buckets_buffer_available(buckets_buffer_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_CORE_H */
