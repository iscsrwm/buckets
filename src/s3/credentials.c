/**
 * S3 Credential Management
 * 
 * Manages access keys and secret keys for S3 authentication.
 * Credentials are stored in a JSON file that can be:
 * - Loaded at server startup
 * - Modified via admin API
 * - Persisted to disk
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>

#include "buckets.h"
#include "buckets_s3.h"
#include "buckets_io.h"
#include "cJSON.h"

/* ===================================================================
 * Configuration
 * ===================================================================*/

#define MAX_CREDENTIALS 1000
#define DEFAULT_CREDENTIALS_FILE ".buckets.sys/credentials.json"

/* ===================================================================
 * Credential Structure
 * ===================================================================*/

typedef struct {
    char access_key[128];
    char secret_key[128];
    char name[256];           /* Human-readable name/description */
    char policy[64];          /* Policy: "readwrite", "readonly", "writeonly" */
    time_t created;
    time_t last_used;
    bool enabled;
} buckets_credential_t;

/* ===================================================================
 * Global State
 * ===================================================================*/

static buckets_credential_t *g_credentials = NULL;
static int g_credential_count = 0;
static int g_credential_capacity = 0;
static pthread_rwlock_t g_credentials_lock = PTHREAD_RWLOCK_INITIALIZER;
static char g_credentials_file[512] = {0};
static bool g_initialized = false;

/* Default credentials for initial setup */
static const buckets_credential_t g_default_credentials[] = {
    {
        .access_key = "minioadmin",
        .secret_key = "minioadmin",
        .name = "Default MinIO-compatible admin",
        .policy = "readwrite",
        .created = 0,
        .last_used = 0,
        .enabled = true
    },
    {
        .access_key = "buckets-admin",
        .secret_key = "buckets-secret-key-change-me",
        .name = "Default Buckets admin",
        .policy = "readwrite",
        .created = 0,
        .last_used = 0,
        .enabled = true
    }
};

/* ===================================================================
 * Internal Helpers
 * ===================================================================*/

/**
 * Generate a random access key (20 characters, uppercase alphanumeric)
 */
static void generate_access_key(char *key, size_t len)
{
    static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (!urandom) {
        /* Fallback to time-based seed */
        srand((unsigned int)time(NULL));
        for (size_t i = 0; i < len - 1; i++) {
            key[i] = charset[rand() % (sizeof(charset) - 1)];
        }
        key[len - 1] = '\0';
        return;
    }
    
    for (size_t i = 0; i < len - 1; i++) {
        unsigned char byte;
        if (fread(&byte, 1, 1, urandom) != 1) {
            byte = (unsigned char)rand();
        }
        key[i] = charset[byte % (sizeof(charset) - 1)];
    }
    key[len - 1] = '\0';
    fclose(urandom);
}

/**
 * Generate a random secret key (40 characters, mixed case alphanumeric + special)
 */
static void generate_secret_key(char *key, size_t len)
{
    static const char charset[] = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (!urandom) {
        srand((unsigned int)time(NULL) ^ (unsigned int)clock());
        for (size_t i = 0; i < len - 1; i++) {
            key[i] = charset[rand() % (sizeof(charset) - 1)];
        }
        key[len - 1] = '\0';
        return;
    }
    
    for (size_t i = 0; i < len - 1; i++) {
        unsigned char byte;
        if (fread(&byte, 1, 1, urandom) != 1) {
            byte = (unsigned char)rand();
        }
        key[i] = charset[byte % (sizeof(charset) - 1)];
    }
    key[len - 1] = '\0';
    fclose(urandom);
}

/**
 * Find credential by access key (internal, must hold lock)
 */
static buckets_credential_t* find_credential_unlocked(const char *access_key)
{
    for (int i = 0; i < g_credential_count; i++) {
        if (strcmp(g_credentials[i].access_key, access_key) == 0) {
            return &g_credentials[i];
        }
    }
    return NULL;
}

/**
 * Serialize credentials to JSON
 */
static char* credentials_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    
    cJSON *creds_array = cJSON_CreateArray();
    if (!creds_array) {
        cJSON_Delete(root);
        return NULL;
    }
    
    for (int i = 0; i < g_credential_count; i++) {
        cJSON *cred = cJSON_CreateObject();
        if (!cred) continue;
        
        cJSON_AddStringToObject(cred, "access_key", g_credentials[i].access_key);
        cJSON_AddStringToObject(cred, "secret_key", g_credentials[i].secret_key);
        cJSON_AddStringToObject(cred, "name", g_credentials[i].name);
        cJSON_AddStringToObject(cred, "policy", g_credentials[i].policy);
        cJSON_AddNumberToObject(cred, "created", (double)g_credentials[i].created);
        cJSON_AddNumberToObject(cred, "last_used", (double)g_credentials[i].last_used);
        cJSON_AddBoolToObject(cred, "enabled", g_credentials[i].enabled);
        
        cJSON_AddItemToArray(creds_array, cred);
    }
    
    cJSON_AddItemToObject(root, "credentials", creds_array);
    cJSON_AddNumberToObject(root, "version", 1);
    
    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    
    return json;
}

/**
 * Load credentials from JSON
 */
static int credentials_from_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        buckets_error("Failed to parse credentials JSON");
        return -1;
    }
    
    cJSON *creds_array = cJSON_GetObjectItem(root, "credentials");
    if (!creds_array || !cJSON_IsArray(creds_array)) {
        buckets_error("Invalid credentials format: missing 'credentials' array");
        cJSON_Delete(root);
        return -1;
    }
    
    int count = cJSON_GetArraySize(creds_array);
    
    /* Resize array if needed */
    if (count > g_credential_capacity) {
        int new_cap = count + 16;
        buckets_credential_t *new_creds = buckets_realloc(g_credentials, 
                                                           new_cap * sizeof(buckets_credential_t));
        if (!new_creds) {
            cJSON_Delete(root);
            return -1;
        }
        g_credentials = new_creds;
        g_credential_capacity = new_cap;
    }
    
    g_credential_count = 0;
    
    for (int i = 0; i < count; i++) {
        cJSON *cred = cJSON_GetArrayItem(creds_array, i);
        if (!cred) continue;
        
        cJSON *access_key = cJSON_GetObjectItem(cred, "access_key");
        cJSON *secret_key = cJSON_GetObjectItem(cred, "secret_key");
        
        if (!access_key || !cJSON_IsString(access_key) ||
            !secret_key || !cJSON_IsString(secret_key)) {
            buckets_warn("Skipping invalid credential entry %d", i);
            continue;
        }
        
        buckets_credential_t *c = &g_credentials[g_credential_count];
        memset(c, 0, sizeof(*c));
        
        strncpy(c->access_key, access_key->valuestring, sizeof(c->access_key) - 1);
        strncpy(c->secret_key, secret_key->valuestring, sizeof(c->secret_key) - 1);
        
        cJSON *name = cJSON_GetObjectItem(cred, "name");
        if (name && cJSON_IsString(name)) {
            strncpy(c->name, name->valuestring, sizeof(c->name) - 1);
        }
        
        cJSON *policy = cJSON_GetObjectItem(cred, "policy");
        if (policy && cJSON_IsString(policy)) {
            strncpy(c->policy, policy->valuestring, sizeof(c->policy) - 1);
        } else {
            strcpy(c->policy, "readwrite");
        }
        
        cJSON *created = cJSON_GetObjectItem(cred, "created");
        if (created && cJSON_IsNumber(created)) {
            c->created = (time_t)created->valuedouble;
        }
        
        cJSON *last_used = cJSON_GetObjectItem(cred, "last_used");
        if (last_used && cJSON_IsNumber(last_used)) {
            c->last_used = (time_t)last_used->valuedouble;
        }
        
        cJSON *enabled = cJSON_GetObjectItem(cred, "enabled");
        c->enabled = !enabled || cJSON_IsTrue(enabled);  /* Default to true */
        
        g_credential_count++;
    }
    
    cJSON_Delete(root);
    buckets_info("Loaded %d credentials", g_credential_count);
    return 0;
}

/* ===================================================================
 * Public API
 * ===================================================================*/

/**
 * Initialize credential system
 */
int buckets_credentials_init(const char *data_dir)
{
    if (g_initialized) {
        return BUCKETS_OK;
    }
    
    pthread_rwlock_wrlock(&g_credentials_lock);
    
    /* Build credentials file path */
    if (data_dir && data_dir[0] != '\0') {
        snprintf(g_credentials_file, sizeof(g_credentials_file), 
                 "%s/%s", data_dir, DEFAULT_CREDENTIALS_FILE);
    } else {
        strncpy(g_credentials_file, DEFAULT_CREDENTIALS_FILE, sizeof(g_credentials_file) - 1);
    }
    
    /* Allocate initial credential array */
    g_credential_capacity = 32;
    g_credentials = buckets_calloc(g_credential_capacity, sizeof(buckets_credential_t));
    if (!g_credentials) {
        pthread_rwlock_unlock(&g_credentials_lock);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Try to load existing credentials */
    void *data = NULL;
    size_t size = 0;
    
    if (buckets_atomic_read(g_credentials_file, &data, &size) == 0 && data) {
        if (credentials_from_json((const char*)data) == 0) {
            buckets_info("Loaded credentials from %s", g_credentials_file);
        }
        buckets_free(data);
    } else {
        /* No existing credentials - add defaults */
        buckets_info("No credentials file found, creating defaults");
        
        for (size_t i = 0; i < sizeof(g_default_credentials) / sizeof(g_default_credentials[0]); i++) {
            g_credentials[g_credential_count] = g_default_credentials[i];
            g_credentials[g_credential_count].created = time(NULL);
            g_credential_count++;
        }
        
        /* Save defaults */
        char *json = credentials_to_json();
        if (json) {
            /* Ensure directory exists */
            char dir[512];
            snprintf(dir, sizeof(dir), "%s/.buckets.sys", data_dir ? data_dir : ".");
            mkdir(dir, 0700);
            
            buckets_atomic_write(g_credentials_file, json, strlen(json));
            buckets_free(json);
        }
    }
    
    g_initialized = true;
    pthread_rwlock_unlock(&g_credentials_lock);
    
    buckets_info("Credential system initialized with %d keys", g_credential_count);
    return BUCKETS_OK;
}

/**
 * Cleanup credential system
 */
void buckets_credentials_cleanup(void)
{
    pthread_rwlock_wrlock(&g_credentials_lock);
    
    if (g_credentials) {
        /* Clear sensitive data */
        memset(g_credentials, 0, g_credential_count * sizeof(buckets_credential_t));
        buckets_free(g_credentials);
        g_credentials = NULL;
    }
    
    g_credential_count = 0;
    g_credential_capacity = 0;
    g_initialized = false;
    
    pthread_rwlock_unlock(&g_credentials_lock);
}

/**
 * Get secret key for access key
 */
int buckets_credentials_get_secret(const char *access_key, char *secret_key, size_t secret_len)
{
    if (!access_key || !secret_key || secret_len == 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock(&g_credentials_lock);
    
    buckets_credential_t *cred = find_credential_unlocked(access_key);
    if (!cred) {
        pthread_rwlock_unlock(&g_credentials_lock);
        return BUCKETS_ERR_NOT_FOUND;
    }
    
    if (!cred->enabled) {
        pthread_rwlock_unlock(&g_credentials_lock);
        buckets_warn("Access key %s is disabled", access_key);
        return BUCKETS_ERR_ACCESS_DENIED;
    }
    
    strncpy(secret_key, cred->secret_key, secret_len - 1);
    secret_key[secret_len - 1] = '\0';
    
    pthread_rwlock_unlock(&g_credentials_lock);
    return BUCKETS_OK;
}

/**
 * Check if access key exists and is enabled
 */
int buckets_credentials_validate(const char *access_key)
{
    if (!access_key) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock(&g_credentials_lock);
    
    buckets_credential_t *cred = find_credential_unlocked(access_key);
    if (!cred) {
        pthread_rwlock_unlock(&g_credentials_lock);
        return BUCKETS_ERR_NOT_FOUND;
    }
    
    bool enabled = cred->enabled;
    pthread_rwlock_unlock(&g_credentials_lock);
    
    return enabled ? BUCKETS_OK : BUCKETS_ERR_ACCESS_DENIED;
}

/**
 * Update last_used timestamp for access key
 */
void buckets_credentials_touch(const char *access_key)
{
    if (!access_key) return;
    
    pthread_rwlock_wrlock(&g_credentials_lock);
    
    buckets_credential_t *cred = find_credential_unlocked(access_key);
    if (cred) {
        cred->last_used = time(NULL);
    }
    
    pthread_rwlock_unlock(&g_credentials_lock);
}

/**
 * Get policy for access key
 */
int buckets_credentials_get_policy(const char *access_key, char *policy, size_t policy_len)
{
    if (!access_key || !policy || policy_len == 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_rdlock(&g_credentials_lock);
    
    buckets_credential_t *cred = find_credential_unlocked(access_key);
    if (!cred) {
        pthread_rwlock_unlock(&g_credentials_lock);
        return BUCKETS_ERR_NOT_FOUND;
    }
    
    strncpy(policy, cred->policy, policy_len - 1);
    policy[policy_len - 1] = '\0';
    
    pthread_rwlock_unlock(&g_credentials_lock);
    return BUCKETS_OK;
}

/**
 * Create new credential
 */
int buckets_credentials_create(const char *name, const char *policy,
                                char *access_key_out, size_t access_key_len,
                                char *secret_key_out, size_t secret_key_len)
{
    if (!access_key_out || access_key_len < 21 ||
        !secret_key_out || secret_key_len < 41) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&g_credentials_lock);
    
    if (g_credential_count >= MAX_CREDENTIALS) {
        pthread_rwlock_unlock(&g_credentials_lock);
        buckets_error("Maximum credential limit reached (%d)", MAX_CREDENTIALS);
        return BUCKETS_ERR_LIMIT;
    }
    
    /* Expand array if needed */
    if (g_credential_count >= g_credential_capacity) {
        int new_cap = g_credential_capacity * 2;
        buckets_credential_t *new_creds = buckets_realloc(g_credentials,
                                                           new_cap * sizeof(buckets_credential_t));
        if (!new_creds) {
            pthread_rwlock_unlock(&g_credentials_lock);
            return BUCKETS_ERR_NOMEM;
        }
        g_credentials = new_creds;
        g_credential_capacity = new_cap;
    }
    
    /* Generate unique access key */
    char new_access_key[21];
    int attempts = 0;
    do {
        generate_access_key(new_access_key, sizeof(new_access_key));
        attempts++;
    } while (find_credential_unlocked(new_access_key) && attempts < 100);
    
    if (attempts >= 100) {
        pthread_rwlock_unlock(&g_credentials_lock);
        buckets_error("Failed to generate unique access key");
        return BUCKETS_ERR_INTERNAL;
    }
    
    /* Generate secret key */
    char new_secret_key[41];
    generate_secret_key(new_secret_key, sizeof(new_secret_key));
    
    /* Add credential */
    buckets_credential_t *cred = &g_credentials[g_credential_count];
    memset(cred, 0, sizeof(*cred));
    
    strncpy(cred->access_key, new_access_key, sizeof(cred->access_key) - 1);
    strncpy(cred->secret_key, new_secret_key, sizeof(cred->secret_key) - 1);
    
    if (name && name[0] != '\0') {
        strncpy(cred->name, name, sizeof(cred->name) - 1);
    } else {
        snprintf(cred->name, sizeof(cred->name), "Key created %ld", (long)time(NULL));
    }
    
    if (policy && policy[0] != '\0') {
        strncpy(cred->policy, policy, sizeof(cred->policy) - 1);
    } else {
        strcpy(cred->policy, "readwrite");
    }
    
    cred->created = time(NULL);
    cred->enabled = true;
    
    g_credential_count++;
    
    /* Copy to output */
    strncpy(access_key_out, new_access_key, access_key_len - 1);
    access_key_out[access_key_len - 1] = '\0';
    strncpy(secret_key_out, new_secret_key, secret_key_len - 1);
    secret_key_out[secret_key_len - 1] = '\0';
    
    /* Persist to disk */
    char *json = credentials_to_json();
    if (json) {
        buckets_atomic_write(g_credentials_file, json, strlen(json));
        buckets_free(json);
    }
    
    pthread_rwlock_unlock(&g_credentials_lock);
    
    buckets_info("Created new credential: %s (%s)", new_access_key, name ? name : "unnamed");
    return BUCKETS_OK;
}

/**
 * Delete credential by access key
 */
int buckets_credentials_delete(const char *access_key)
{
    if (!access_key) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&g_credentials_lock);
    
    int found_idx = -1;
    for (int i = 0; i < g_credential_count; i++) {
        if (strcmp(g_credentials[i].access_key, access_key) == 0) {
            found_idx = i;
            break;
        }
    }
    
    if (found_idx < 0) {
        pthread_rwlock_unlock(&g_credentials_lock);
        return BUCKETS_ERR_NOT_FOUND;
    }
    
    /* Clear sensitive data */
    memset(&g_credentials[found_idx], 0, sizeof(buckets_credential_t));
    
    /* Shift remaining credentials */
    for (int i = found_idx; i < g_credential_count - 1; i++) {
        g_credentials[i] = g_credentials[i + 1];
    }
    g_credential_count--;
    
    /* Persist to disk */
    char *json = credentials_to_json();
    if (json) {
        buckets_atomic_write(g_credentials_file, json, strlen(json));
        buckets_free(json);
    }
    
    pthread_rwlock_unlock(&g_credentials_lock);
    
    buckets_info("Deleted credential: %s", access_key);
    return BUCKETS_OK;
}

/**
 * Enable/disable credential
 */
int buckets_credentials_set_enabled(const char *access_key, bool enabled)
{
    if (!access_key) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&g_credentials_lock);
    
    buckets_credential_t *cred = find_credential_unlocked(access_key);
    if (!cred) {
        pthread_rwlock_unlock(&g_credentials_lock);
        return BUCKETS_ERR_NOT_FOUND;
    }
    
    cred->enabled = enabled;
    
    /* Persist to disk */
    char *json = credentials_to_json();
    if (json) {
        buckets_atomic_write(g_credentials_file, json, strlen(json));
        buckets_free(json);
    }
    
    pthread_rwlock_unlock(&g_credentials_lock);
    
    buckets_info("%s credential: %s", enabled ? "Enabled" : "Disabled", access_key);
    return BUCKETS_OK;
}

/**
 * List all credentials (returns JSON)
 * Caller must free returned string.
 * Note: Secret keys are NOT included in output.
 */
char* buckets_credentials_list(void)
{
    pthread_rwlock_rdlock(&g_credentials_lock);
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        pthread_rwlock_unlock(&g_credentials_lock);
        return NULL;
    }
    
    cJSON *creds_array = cJSON_CreateArray();
    if (!creds_array) {
        cJSON_Delete(root);
        pthread_rwlock_unlock(&g_credentials_lock);
        return NULL;
    }
    
    for (int i = 0; i < g_credential_count; i++) {
        cJSON *cred = cJSON_CreateObject();
        if (!cred) continue;
        
        cJSON_AddStringToObject(cred, "access_key", g_credentials[i].access_key);
        /* Note: secret_key is NOT included for security */
        cJSON_AddStringToObject(cred, "name", g_credentials[i].name);
        cJSON_AddStringToObject(cred, "policy", g_credentials[i].policy);
        cJSON_AddNumberToObject(cred, "created", (double)g_credentials[i].created);
        cJSON_AddNumberToObject(cred, "last_used", (double)g_credentials[i].last_used);
        cJSON_AddBoolToObject(cred, "enabled", g_credentials[i].enabled);
        
        cJSON_AddItemToArray(creds_array, cred);
    }
    
    cJSON_AddItemToObject(root, "credentials", creds_array);
    cJSON_AddNumberToObject(root, "count", g_credential_count);
    
    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    
    pthread_rwlock_unlock(&g_credentials_lock);
    return json;
}

/**
 * Get credential count
 */
int buckets_credentials_count(void)
{
    pthread_rwlock_rdlock(&g_credentials_lock);
    int count = g_credential_count;
    pthread_rwlock_unlock(&g_credentials_lock);
    return count;
}
