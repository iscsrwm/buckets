/**
 * S3 Authentication (AWS Signature V4)
 * 
 * Implements AWS Signature Version 4 authentication:
 * https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-authenticating-requests.html
 * 
 * Signature calculation:
 * 1. Create canonical request
 * 2. Create string to sign
 * 3. Calculate signing key
 * 4. Calculate signature
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "buckets.h"
#include "buckets_s3.h"

/* ===================================================================
 * Configuration
 * ===================================================================*/

/* AWS Signature V4 constants */
#define AWS4_ALGORITHM "AWS4-HMAC-SHA256"
#define AWS4_REQUEST   "aws4_request"

/* Default region and service for Buckets */
#define DEFAULT_REGION  "us-east-1"
#define DEFAULT_SERVICE "s3"

/* Enable/disable authentication (for testing) */
static bool g_auth_enabled = true;
static bool g_auth_initialized = false;

/* ===================================================================
 * HMAC Helpers
 * ===================================================================*/

/**
 * Calculate HMAC-SHA256
 */
static int hmac_sha256(const unsigned char *key, size_t key_len,
                       const unsigned char *data, size_t data_len,
                       unsigned char *output)
{
    unsigned int out_len = 0;
    
    if (!HMAC(EVP_sha256(), key, (int)key_len, data, data_len, output, &out_len)) {
        return BUCKETS_ERR_CRYPTO;
    }
    
    if (out_len != 32) {
        return BUCKETS_ERR_CRYPTO;
    }
    
    return BUCKETS_OK;
}

/**
 * Calculate SHA256 hash
 */
static int sha256_hash(const unsigned char *data, size_t len, unsigned char *output)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return BUCKETS_ERR_CRYPTO;
    }
    
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        return BUCKETS_ERR_CRYPTO;
    }
    
    if (EVP_DigestUpdate(ctx, data, len) != 1) {
        EVP_MD_CTX_free(ctx);
        return BUCKETS_ERR_CRYPTO;
    }
    
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx, output, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return BUCKETS_ERR_CRYPTO;
    }
    
    EVP_MD_CTX_free(ctx);
    return BUCKETS_OK;
}

/**
 * Convert bytes to lowercase hex string
 */
static void bytes_to_hex(const unsigned char *bytes, size_t len, char *hex)
{
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex[i * 2] = hex_chars[(bytes[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_chars[bytes[i] & 0x0F];
    }
    hex[len * 2] = '\0';
}

/**
 * URL encode a string (for canonical URI)
 */
/* Note: url_encode was removed because:
 * The URI comes to us already percent-encoded from the HTTP layer.
 * Re-encoding it would break signature verification (e.g., %20 -> %2520).
 * If URL encoding is needed in the future, this function can be restored:
 *
 * static void url_encode(const char *src, char *dst, size_t dst_len)
 * {
 *     static const char *safe_chars = "-_.~";
 *     size_t j = 0;
 *     for (size_t i = 0; src[i] != '\0' && j < dst_len - 3; i++) {
 *         char c = src[i];
 *         if (isalnum((unsigned char)c) || strchr(safe_chars, c) || c == '/') {
 *             dst[j++] = c;
 *         } else {
 *             snprintf(dst + j, dst_len - j, "%%%02X", (unsigned char)c);
 *             j += 3;
 *         }
 *     }
 *     dst[j] = '\0';
 * }
 */

/* ===================================================================
 * AWS Signature V4 Components
 * ===================================================================*/

/**
 * Parse Authorization header
 * 
 * Format: AWS4-HMAC-SHA256 Credential=ACCESS_KEY/DATE/REGION/SERVICE/aws4_request,
 *         SignedHeaders=host;x-amz-content-sha256;x-amz-date,Signature=SIGNATURE
 */
int buckets_s3_parse_auth_header(const char *auth_header, buckets_s3_request_t *req,
                                  char *date_out, size_t date_len,
                                  char *region_out, size_t region_len)
{
    if (!auth_header || !req) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Check algorithm */
    if (strncmp(auth_header, AWS4_ALGORITHM, strlen(AWS4_ALGORITHM)) != 0) {
        buckets_debug("Unsupported auth algorithm: %.20s...", auth_header);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Extract Credential */
    const char *cred = strstr(auth_header, "Credential=");
    if (!cred) {
        buckets_debug("Missing Credential in Authorization header");
        return BUCKETS_ERR_INVALID_ARG;
    }
    cred += 11;  /* Skip "Credential=" */
    
    /* Extract access key (before first '/') */
    const char *slash1 = strchr(cred, '/');
    if (!slash1) {
        buckets_debug("Invalid Credential format");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    size_t key_len = (size_t)(slash1 - cred);
    if (key_len >= sizeof(req->access_key)) {
        buckets_debug("Access key too long");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    memcpy(req->access_key, cred, key_len);
    req->access_key[key_len] = '\0';
    
    /* Extract date (YYYYMMDD) */
    const char *date_start = slash1 + 1;
    const char *slash2 = strchr(date_start, '/');
    if (!slash2) {
        buckets_debug("Invalid Credential format - missing date");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (date_out && date_len > 0) {
        size_t date_part_len = (size_t)(slash2 - date_start);
        if (date_part_len < date_len) {
            memcpy(date_out, date_start, date_part_len);
            date_out[date_part_len] = '\0';
        }
    }
    
    /* Extract region */
    const char *region_start = slash2 + 1;
    const char *slash3 = strchr(region_start, '/');
    if (!slash3) {
        buckets_debug("Invalid Credential format - missing region");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (region_out && region_len > 0) {
        size_t region_part_len = (size_t)(slash3 - region_start);
        if (region_part_len < region_len) {
            memcpy(region_out, region_start, region_part_len);
            region_out[region_part_len] = '\0';
        }
    }
    
    /* Extract SignedHeaders */
    const char *signed_hdrs = strstr(auth_header, "SignedHeaders=");
    if (signed_hdrs) {
        signed_hdrs += 14;  /* Skip "SignedHeaders=" */
        const char *comma = strchr(signed_hdrs, ',');
        if (comma) {
            size_t hdr_len = (size_t)(comma - signed_hdrs);
            if (hdr_len < sizeof(req->signed_headers)) {
                memcpy(req->signed_headers, signed_hdrs, hdr_len);
                req->signed_headers[hdr_len] = '\0';
            }
        }
    }
    
    /* Extract Signature */
    const char *sig = strstr(auth_header, "Signature=");
    if (!sig) {
        buckets_debug("Missing Signature in Authorization header");
        return BUCKETS_ERR_INVALID_ARG;
    }
    sig += 10;  /* Skip "Signature=" */
    
    /* Copy signature (until end or comma or space) */
    size_t sig_len = 0;
    while (sig[sig_len] != '\0' && sig[sig_len] != ',' && sig[sig_len] != ' ') {
        sig_len++;
    }
    
    if (sig_len >= sizeof(req->signature)) {
        buckets_debug("Signature too long");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    memcpy(req->signature, sig, sig_len);
    req->signature[sig_len] = '\0';
    
    buckets_debug("Parsed auth: access_key=%s, signed_headers=%s, sig_len=%zu",
                  req->access_key, req->signed_headers, sig_len);
    
    return BUCKETS_OK;
}

/**
 * Build canonical request string
 * 
 * CanonicalRequest =
 *   HTTPRequestMethod + '\n' +
 *   CanonicalURI + '\n' +
 *   CanonicalQueryString + '\n' +
 *   CanonicalHeaders + '\n' +
 *   SignedHeaders + '\n' +
 *   HashedPayload
 */
static int build_canonical_request(const char *method,
                                    const char *uri,
                                    const char *query_string,
                                    const char *canonical_headers,
                                    const char *signed_headers,
                                    const char *payload_hash,
                                    char *canonical_request,
                                    size_t max_len)
{
    /* The URI comes to us already percent-encoded from the HTTP layer.
     * AWS Signature V4 requires the canonical URI to be URI-encoded, but
     * we should NOT re-encode an already-encoded URI (that would turn %20
     * into %2520). The client's signature is computed with the same URI
     * they send in the HTTP request, so we use it as-is. */
    
    int len = snprintf(canonical_request, max_len,
        "%s\n"      /* HTTP method */
        "%s\n"      /* Canonical URI */
        "%s\n"      /* Canonical query string */
        "%s\n"      /* Canonical headers (with trailing newline) */
        "%s\n"      /* Signed headers */
        "%s",       /* Hashed payload */
        method,
        uri,        /* Use URI as-is - already encoded from HTTP layer */
        query_string ? query_string : "",
        canonical_headers,
        signed_headers,
        payload_hash);
    
    if (len < 0 || len >= (int)max_len) {
        return BUCKETS_ERR_NOMEM;
    }
    
    return BUCKETS_OK;
}

/**
 * Build string to sign
 * 
 * StringToSign =
 *   Algorithm + '\n' +
 *   RequestDateTime + '\n' +
 *   CredentialScope + '\n' +
 *   HashedCanonicalRequest
 */
static int build_string_to_sign(const char *datetime,     /* ISO8601 YYYYMMDD'T'HHMMSS'Z' */
                                 const char *date,         /* YYYYMMDD */
                                 const char *region,
                                 const char *service,
                                 const char *canonical_request_hash,
                                 char *string_to_sign,
                                 size_t max_len)
{
    int len = snprintf(string_to_sign, max_len,
        AWS4_ALGORITHM "\n"
        "%s\n"              /* Request datetime */
        "%s/%s/%s/" AWS4_REQUEST "\n"  /* Credential scope */
        "%s",               /* Hashed canonical request */
        datetime,
        date, region, service,
        canonical_request_hash);
    
    if (len < 0 || len >= (int)max_len) {
        return BUCKETS_ERR_NOMEM;
    }
    
    return BUCKETS_OK;
}

/**
 * Calculate signing key
 * 
 * kDate = HMAC("AWS4" + SecretKey, Date)
 * kRegion = HMAC(kDate, Region)
 * kService = HMAC(kRegion, Service)
 * kSigning = HMAC(kService, "aws4_request")
 */
static int calculate_signing_key(const char *secret_key,
                                  const char *date,        /* YYYYMMDD */
                                  const char *region,
                                  const char *service,
                                  unsigned char *signing_key)
{
    unsigned char k_date[32];
    unsigned char k_region[32];
    unsigned char k_service[32];
    
    /* Initial key: "AWS4" + secret_key */
    char aws4_secret[256];
    int len = snprintf(aws4_secret, sizeof(aws4_secret), "AWS4%s", secret_key);
    if (len < 0 || len >= (int)sizeof(aws4_secret)) {
        return BUCKETS_ERR_NOMEM;
    }
    
    /* kDate = HMAC("AWS4" + secret_key, date) */
    if (hmac_sha256((unsigned char *)aws4_secret, (size_t)len,
                    (unsigned char *)date, strlen(date), k_date) != BUCKETS_OK) {
        return BUCKETS_ERR_CRYPTO;
    }
    
    /* kRegion = HMAC(kDate, region) */
    if (hmac_sha256(k_date, 32, (unsigned char *)region, strlen(region),
                    k_region) != BUCKETS_OK) {
        return BUCKETS_ERR_CRYPTO;
    }
    
    /* kService = HMAC(kRegion, service) */
    if (hmac_sha256(k_region, 32, (unsigned char *)service, strlen(service),
                    k_service) != BUCKETS_OK) {
        return BUCKETS_ERR_CRYPTO;
    }
    
    /* kSigning = HMAC(kService, "aws4_request") */
    if (hmac_sha256(k_service, 32, (unsigned char *)AWS4_REQUEST, strlen(AWS4_REQUEST),
                    signing_key) != BUCKETS_OK) {
        return BUCKETS_ERR_CRYPTO;
    }
    
    return BUCKETS_OK;
}

/**
 * Calculate final signature
 */
static int calculate_signature(const unsigned char *signing_key,
                                const char *string_to_sign,
                                char *signature_hex)
{
    unsigned char signature[32];
    
    if (hmac_sha256(signing_key, 32,
                    (unsigned char *)string_to_sign, strlen(string_to_sign),
                    signature) != BUCKETS_OK) {
        return BUCKETS_ERR_CRYPTO;
    }
    
    bytes_to_hex(signature, 32, signature_hex);
    return BUCKETS_OK;
}

/* ===================================================================
 * Public API
 * ===================================================================*/

/**
 * Initialize authentication system
 */
int buckets_s3_auth_init(bool enabled)
{
    g_auth_enabled = enabled;
    g_auth_initialized = true;
    
    buckets_info("S3 authentication %s", enabled ? "enabled" : "disabled");
    return BUCKETS_OK;
}

/**
 * Check if authentication is enabled
 */
bool buckets_s3_auth_enabled(void)
{
    return g_auth_enabled;
}

/**
 * Set authentication enabled/disabled
 */
void buckets_s3_auth_set_enabled(bool enabled)
{
    g_auth_enabled = enabled;
    buckets_info("S3 authentication %s", enabled ? "enabled" : "disabled");
}

/**
 * Get secret key for access key (uses credential store)
 */
int buckets_s3_get_secret_key(const char *access_key, char *secret_key)
{
    return buckets_credentials_get_secret(access_key, secret_key, 128);
}

/**
 * Verify AWS Signature V4
 * 
 * Full implementation that:
 * 1. Parses Authorization header
 * 2. Looks up secret key
 * 3. Builds canonical request
 * 4. Calculates expected signature
 * 5. Compares with provided signature
 */
int buckets_s3_verify_signature(buckets_s3_request_t *req, const char *secret_key)
{
    if (!req) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* If auth is disabled, allow all requests */
    if (!g_auth_enabled) {
        buckets_debug("Authentication disabled - allowing request");
        return BUCKETS_OK;
    }
    
    /* If no access key provided, deny */
    if (req->access_key[0] == '\0') {
        buckets_debug("No access key provided");
        return BUCKETS_ERR_ACCESS_DENIED;
    }
    
    /* If no signature provided, deny */
    if (req->signature[0] == '\0') {
        buckets_debug("No signature provided");
        return BUCKETS_ERR_ACCESS_DENIED;
    }
    
    /* Get secret key if not provided */
    char secret_buf[128];
    const char *secret = secret_key;
    
    if (!secret || secret[0] == '\0') {
        int ret = buckets_credentials_get_secret(req->access_key, secret_buf, sizeof(secret_buf));
        if (ret != BUCKETS_OK) {
            buckets_warn("Unknown access key: %s", req->access_key);
            return BUCKETS_ERR_ACCESS_DENIED;
        }
        secret = secret_buf;
    }
    
    /* Get date from x-amz-date or Date header */
    const char *amz_date = req->date;  /* Should be ISO8601: YYYYMMDD'T'HHMMSS'Z' */
    if (!amz_date || amz_date[0] == '\0') {
        buckets_debug("Missing x-amz-date header");
        return BUCKETS_ERR_ACCESS_DENIED;
    }
    
    /* Extract YYYYMMDD from datetime */
    char date[9] = {0};
    if (strlen(amz_date) >= 8) {
        memcpy(date, amz_date, 8);
        date[8] = '\0';
    }
    
    /* Calculate payload hash */
    char payload_hash[65];
    unsigned char hash[32];
    
    if (req->body && req->body_len > 0) {
        sha256_hash((unsigned char *)req->body, req->body_len, hash);
    } else {
        sha256_hash((unsigned char *)"", 0, hash);
    }
    bytes_to_hex(hash, 32, payload_hash);
    
    /* Build canonical URI - use original URI from HTTP request to preserve trailing slashes */
    char canonical_uri[2048];
    if (req->http_req && req->http_req->uri) {
        /* Extract just the path part (before query string) */
        const char *uri = req->http_req->uri;
        const char *query_start = strchr(uri, '?');
        if (query_start) {
            size_t path_len = (size_t)(query_start - uri);
            if (path_len < sizeof(canonical_uri)) {
                memcpy(canonical_uri, uri, path_len);
                canonical_uri[path_len] = '\0';
            } else {
                strncpy(canonical_uri, uri, sizeof(canonical_uri) - 1);
                canonical_uri[sizeof(canonical_uri) - 1] = '\0';
            }
        } else {
            strncpy(canonical_uri, uri, sizeof(canonical_uri) - 1);
            canonical_uri[sizeof(canonical_uri) - 1] = '\0';
        }
    } else if (req->bucket[0] != '\0' && req->key[0] != '\0') {
        snprintf(canonical_uri, sizeof(canonical_uri), "/%s/%s", req->bucket, req->key);
    } else if (req->bucket[0] != '\0') {
        snprintf(canonical_uri, sizeof(canonical_uri), "/%s", req->bucket);
    } else {
        strcpy(canonical_uri, "/");
    }
    
    /* Get host header from HTTP request */
    const char *host = "localhost";
    const char *content_sha256 = payload_hash;  /* Default to computed hash */
    if (req->http_req && req->http_req->internal) {
        extern const char* uv_http_get_header(void *conn, const char *name);
        const char *host_hdr = uv_http_get_header(req->http_req->internal, "Host");
        if (host_hdr && host_hdr[0] != '\0') {
            host = host_hdr;
        }
        /* Use the client's x-amz-content-sha256 header value for canonical request */
        /* This supports UNSIGNED-PAYLOAD and other special values */
        const char *sha256_hdr = uv_http_get_header(req->http_req->internal, "x-amz-content-sha256");
        if (sha256_hdr && sha256_hdr[0] != '\0') {
            content_sha256 = sha256_hdr;
        }
    }
    
    /* Use signed headers from request or default */
    const char *signed_headers = req->signed_headers;
    if (!signed_headers || signed_headers[0] == '\0') {
        signed_headers = "host;x-amz-content-sha256;x-amz-date";
    }
    
    /* Build canonical headers dynamically from SignedHeaders list */
    /* Parse signed_headers (semicolon-separated) and get each header value */
    char canonical_headers[4096] = "";
    size_t ch_len = 0;
    
    char signed_headers_copy[512];
    strncpy(signed_headers_copy, signed_headers, sizeof(signed_headers_copy) - 1);
    signed_headers_copy[sizeof(signed_headers_copy) - 1] = '\0';
    
    char *saveptr = NULL;
    char *header_name = strtok_r(signed_headers_copy, ";", &saveptr);
    while (header_name && ch_len < sizeof(canonical_headers) - 256) {
        const char *header_value = NULL;
        
        /* Get the header value based on the header name */
        if (strcmp(header_name, "host") == 0) {
            header_value = host;
        } else if (strcmp(header_name, "x-amz-content-sha256") == 0) {
            header_value = content_sha256;
        } else if (strcmp(header_name, "x-amz-date") == 0) {
            header_value = amz_date;
        } else if (req->http_req && req->http_req->internal) {
            /* Get from HTTP request headers */
            extern const char* uv_http_get_header(void *conn, const char *name);
            header_value = uv_http_get_header(req->http_req->internal, header_name);
        }
        
        if (header_value && header_value[0] != '\0') {
            /* Lowercase header name, trim header value */
            ch_len += snprintf(canonical_headers + ch_len, 
                               sizeof(canonical_headers) - ch_len,
                               "%s:%s\n", header_name, header_value);
        } else {
            /* Header not found, use empty value */
            ch_len += snprintf(canonical_headers + ch_len,
                               sizeof(canonical_headers) - ch_len,
                               "%s:\n", header_name);
        }
        
        header_name = strtok_r(NULL, ";", &saveptr);
    }
    
    /* Get HTTP method from request */
    const char *method = "GET";
    if (req->http_req && req->http_req->method) {
        method = req->http_req->method;
    }
    
    /* Get query string from HTTP request (for canonical query string) */
    const char *query_string = NULL;
    char canonical_query_string[2048] = "";
    if (req->http_req && req->http_req->query_string) {
        /* AWS requires query params to be sorted alphabetically and URL-encoded */
        /* For now, use the raw query string (without leading ?) */
        query_string = req->http_req->query_string;
        if (query_string[0] == '?') {
            query_string++;  /* Skip leading ? */
        }
        strncpy(canonical_query_string, query_string, sizeof(canonical_query_string) - 1);
    }
    
    /* Build canonical request */
    char canonical_request[8192];
    if (build_canonical_request(method, canonical_uri, canonical_query_string,
                                 canonical_headers, signed_headers, content_sha256,
                                 canonical_request, sizeof(canonical_request)) != BUCKETS_OK) {
        buckets_error("Failed to build canonical request");
        return BUCKETS_ERR_INTERNAL;
    }
    
    buckets_debug("Canonical request:\n%s", canonical_request);
    
    /* Hash canonical request */
    char canonical_request_hash[65];
    sha256_hash((unsigned char *)canonical_request, strlen(canonical_request), hash);
    bytes_to_hex(hash, 32, canonical_request_hash);
    
    buckets_debug("Canonical request hash: %s", canonical_request_hash);
    
    /* Use region from request credential scope, or default if not provided */
    const char *region = DEFAULT_REGION;
    if (req->region[0] != '\0') {
        region = req->region;
    }
    
    /* Build string to sign */
    char string_to_sign[4096];
    if (build_string_to_sign(amz_date, date, region, DEFAULT_SERVICE,
                              canonical_request_hash, string_to_sign,
                              sizeof(string_to_sign)) != BUCKETS_OK) {
        buckets_error("Failed to build string to sign");
        return BUCKETS_ERR_INTERNAL;
    }
    
    buckets_debug("String to sign:\n%s", string_to_sign);
    
    /* Calculate signing key */
    unsigned char signing_key[32];
    if (calculate_signing_key(secret, date, region, DEFAULT_SERVICE,
                               signing_key) != BUCKETS_OK) {
        buckets_error("Failed to calculate signing key");
        return BUCKETS_ERR_CRYPTO;
    }
    
    /* Calculate expected signature */
    char expected_signature[65];
    if (calculate_signature(signing_key, string_to_sign, expected_signature) != BUCKETS_OK) {
        buckets_error("Failed to calculate signature");
        return BUCKETS_ERR_CRYPTO;
    }
    
    /* Compare signatures (constant-time comparison to prevent timing attacks) */
    size_t sig_len = strlen(expected_signature);
    size_t req_sig_len = strlen(req->signature);
    
    if (sig_len != req_sig_len) {
        buckets_warn("Signature length mismatch for %s", req->access_key);
        buckets_debug("Expected: %s", expected_signature);
        buckets_debug("Got:      %s", req->signature);
        return BUCKETS_ERR_ACCESS_DENIED;
    }
    
    int diff = 0;
    for (size_t i = 0; i < sig_len; i++) {
        diff |= expected_signature[i] ^ req->signature[i];
    }
    
    if (diff != 0) {
        buckets_warn("Signature mismatch for %s", req->access_key);
        buckets_warn("Expected: %s", expected_signature);
        buckets_warn("Got:      %s", req->signature);
        buckets_warn("Client signed_headers: %s", req->signed_headers);
        buckets_warn("We used signed_headers: %s", signed_headers);
        buckets_warn("Canonical request:\n%s", canonical_request);
        buckets_warn("String to sign:\n%s", string_to_sign);
        return BUCKETS_ERR_ACCESS_DENIED;
    }
    
    /* Update last_used timestamp */
    buckets_credentials_touch(req->access_key);
    
    buckets_debug("Signature verified for %s", req->access_key);
    return BUCKETS_OK;
}

/**
 * Quick check if request has valid auth header format
 */
bool buckets_s3_has_auth(buckets_s3_request_t *req)
{
    return req && req->access_key[0] != '\0' && req->signature[0] != '\0';
}
