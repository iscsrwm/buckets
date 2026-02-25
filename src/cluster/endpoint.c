/**
 * Endpoint Parsing and Validation Implementation
 * 
 * Handles parsing of storage endpoint URLs with ellipses expansion support.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_endpoint.h"

/* Helper: Check if string starts with prefix */
static bool starts_with(const char *str, const char *prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

/* Helper: Duplicate string slice */
static char *strndup_safe(const char *str, size_t n)
{
    char *result = buckets_malloc(n + 1);
    if (!result) {
        return NULL;
    }
    memcpy(result, str, n);
    result[n] = '\0';
    return result;
}

/* Helper: Parse port from string */
static i32 parse_port(const char *port_str)
{
    char *endptr;
    long port = strtol(port_str, &endptr, 10);
    
    if (*endptr != '\0' || port < 1 || port > 65535) {
        return -1;
    }
    
    return (i32)port;
}

/**
 * Parse URL-style endpoint: http://host:port/path or https://host:port/path
 */
static buckets_endpoint_t *parse_url_endpoint(const char *endpoint_str)
{
    buckets_endpoint_t *endpoint = buckets_calloc(1, sizeof(buckets_endpoint_t));
    if (!endpoint) {
        return NULL;
    }
    
    endpoint->original = buckets_strdup(endpoint_str);
    endpoint->type = BUCKETS_ENDPOINT_TYPE_URL;
    endpoint->pool_idx = -1;
    endpoint->set_idx = -1;
    endpoint->disk_idx = -1;
    
    /* Determine scheme */
    const char *rest = NULL;
    if (starts_with(endpoint_str, "https://")) {
        endpoint->scheme = buckets_strdup("https");
        rest = endpoint_str + 8;
    } else if (starts_with(endpoint_str, "http://")) {
        endpoint->scheme = buckets_strdup("http");
        rest = endpoint_str + 7;
    } else {
        buckets_error("Invalid URL scheme: must be http:// or https://");
        goto error;
    }
    
    /* Find path component */
    const char *path_start = strchr(rest, '/');
    if (!path_start) {
        /* No path, just host:port */
        path_start = rest + strlen(rest);
        endpoint->path = buckets_strdup("/");
    } else {
        endpoint->path = buckets_strdup(path_start);
    }
    
    /* Parse host:port */
    size_t host_port_len = path_start - rest;
    char *host_port = strndup_safe(rest, host_port_len);
    if (!host_port) {
        goto error;
    }
    
    /* Check for IPv6 address [host]:port */
    if (host_port[0] == '[') {
        char *bracket_end = strchr(host_port, ']');
        if (!bracket_end) {
            buckets_error("Invalid IPv6 address: missing closing bracket");
            buckets_free(host_port);
            goto error;
        }
        
        /* Extract IPv6 address */
        size_t ipv6_len = bracket_end - host_port - 1;
        endpoint->host = strndup_safe(host_port + 1, ipv6_len);
        
        /* Check for port after bracket */
        if (bracket_end[1] == ':') {
            i32 port = parse_port(bracket_end + 2);
            if (port < 0) {
                buckets_error("Invalid port number after IPv6 address");
                buckets_free(host_port);
                goto error;
            }
            endpoint->port = (u16)port;
        } else {
            endpoint->port = 0; /* Default port */
        }
    } else {
        /* Regular hostname or IPv4 */
        char *colon = strchr(host_port, ':');
        if (colon) {
            /* host:port format */
            endpoint->host = strndup_safe(host_port, colon - host_port);
            i32 port = parse_port(colon + 1);
            if (port < 0) {
                buckets_error("Invalid port number: %s", colon + 1);
                buckets_free(host_port);
                goto error;
            }
            endpoint->port = (u16)port;
        } else {
            /* Just host, no port */
            endpoint->host = buckets_strdup(host_port);
            endpoint->port = 0;
        }
    }
    
    buckets_free(host_port);
    
    /* Validate host is not empty */
    if (!endpoint->host || endpoint->host[0] == '\0') {
        buckets_error("Empty hostname in URL endpoint");
        goto error;
    }
    
    return endpoint;

error:
    buckets_endpoint_free(endpoint);
    return NULL;
}

/**
 * Parse path-style endpoint: /mnt/disk1
 */
static buckets_endpoint_t *parse_path_endpoint(const char *endpoint_str)
{
    buckets_endpoint_t *endpoint = buckets_calloc(1, sizeof(buckets_endpoint_t));
    if (!endpoint) {
        return NULL;
    }
    
    endpoint->original = buckets_strdup(endpoint_str);
    endpoint->type = BUCKETS_ENDPOINT_TYPE_PATH;
    endpoint->path = buckets_strdup(endpoint_str);
    endpoint->pool_idx = -1;
    endpoint->set_idx = -1;
    endpoint->disk_idx = -1;
    endpoint->is_local = true; /* Path endpoints are always local */
    
    /* Validate path is not empty and not root */
    if (!endpoint->path || endpoint->path[0] == '\0' || 
        strcmp(endpoint->path, "/") == 0 || strcmp(endpoint->path, "\\") == 0) {
        buckets_error("Empty or root path is not supported");
        buckets_endpoint_free(endpoint);
        return NULL;
    }
    
    return endpoint;
}

buckets_endpoint_t *buckets_endpoint_parse(const char *endpoint_str)
{
    if (!endpoint_str || endpoint_str[0] == '\0') {
        buckets_error("NULL or empty endpoint string");
        return NULL;
    }
    
    /* Check if URL-style or path-style */
    if (starts_with(endpoint_str, "http://") || starts_with(endpoint_str, "https://")) {
        return parse_url_endpoint(endpoint_str);
    } else if (strstr(endpoint_str, "://") != NULL) {
        /* Has scheme separator but not http/https - invalid */
        buckets_error("Unsupported URL scheme (only http:// and https:// are supported)");
        return NULL;
    } else {
        return parse_path_endpoint(endpoint_str);
    }
}

void buckets_endpoint_free(buckets_endpoint_t *endpoint)
{
    if (!endpoint) {
        return;
    }
    
    buckets_free(endpoint->original);
    buckets_free(endpoint->scheme);
    buckets_free(endpoint->host);
    buckets_free(endpoint->path);
    buckets_free(endpoint);
}

buckets_error_t buckets_endpoint_validate(const buckets_endpoint_t *endpoint)
{
    if (!endpoint) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Validate path is not empty */
    if (!endpoint->path || endpoint->path[0] == '\0') {
        buckets_error("Endpoint has empty path");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* For URL-type endpoints */
    if (endpoint->type == BUCKETS_ENDPOINT_TYPE_URL) {
        /* Validate scheme */
        if (!endpoint->scheme || 
            (strcmp(endpoint->scheme, "http") != 0 && strcmp(endpoint->scheme, "https") != 0)) {
            buckets_error("Invalid scheme: must be http or https");
            return BUCKETS_ERR_INVALID_ARG;
        }
        
        /* Validate host */
        if (!endpoint->host || endpoint->host[0] == '\0') {
            buckets_error("URL endpoint has empty host");
            return BUCKETS_ERR_INVALID_ARG;
        }
        
        /* Port 0 is allowed (means use default) */
    }
    
    /* For path-type endpoints */
    if (endpoint->type == BUCKETS_ENDPOINT_TYPE_PATH) {
        /* Validate path is absolute */
        if (endpoint->path[0] != '/' && endpoint->path[0] != '\\') {
            buckets_error("Path endpoint must be absolute (start with / or \\)");
            return BUCKETS_ERR_INVALID_ARG;
        }
        
        /* Validate not root */
        if (strcmp(endpoint->path, "/") == 0 || strcmp(endpoint->path, "\\") == 0) {
            buckets_error("Root path not allowed");
            return BUCKETS_ERR_INVALID_ARG;
        }
    }
    
    return BUCKETS_OK;
}

char *buckets_endpoint_to_string(const buckets_endpoint_t *endpoint)
{
    if (!endpoint) {
        return NULL;
    }
    
    if (endpoint->type == BUCKETS_ENDPOINT_TYPE_PATH) {
        return buckets_strdup(endpoint->path);
    }
    
    /* URL-type endpoint */
    char *result;
    if (endpoint->port > 0) {
        result = buckets_format("%s://%s:%u%s", 
                               endpoint->scheme,
                               endpoint->host,
                               endpoint->port,
                               endpoint->path);
    } else {
        result = buckets_format("%s://%s%s",
                               endpoint->scheme,
                               endpoint->host,
                               endpoint->path);
    }
    
    return result;
}

bool buckets_endpoint_equal(const buckets_endpoint_t *ep1,
                           const buckets_endpoint_t *ep2)
{
    if (!ep1 || !ep2) {
        return false;
    }
    
    /* Compare types */
    if (ep1->type != ep2->type) {
        return false;
    }
    
    /* Compare paths */
    if (strcmp(ep1->path, ep2->path) != 0) {
        return false;
    }
    
    /* For URL endpoints, compare host and port */
    if (ep1->type == BUCKETS_ENDPOINT_TYPE_URL) {
        if (strcmp(ep1->host, ep2->host) != 0) {
            return false;
        }
        if (ep1->port != ep2->port) {
            return false;
        }
    }
    
    /* Compare cluster placement (if assigned) */
    if (ep1->is_local != ep2->is_local ||
        ep1->pool_idx != ep2->pool_idx ||
        ep1->set_idx != ep2->set_idx ||
        ep1->disk_idx != ep2->disk_idx) {
        return false;
    }
    
    return true;
}

bool buckets_endpoint_is_local(const buckets_endpoint_t *endpoint)
{
    if (!endpoint) {
        return false;
    }
    
    /* Path endpoints are always local */
    if (endpoint->type == BUCKETS_ENDPOINT_TYPE_PATH) {
        return true;
    }
    
    /* Check for localhost indicators */
    if (!endpoint->host) {
        return false;
    }
    
    /* Check common localhost names */
    if (strcmp(endpoint->host, "localhost") == 0 ||
        strcmp(endpoint->host, "127.0.0.1") == 0 ||
        strcmp(endpoint->host, "::1") == 0 ||
        strcmp(endpoint->host, "0.0.0.0") == 0 ||
        strcmp(endpoint->host, "::") == 0) {
        return true;
    }
    
    /* Check against system hostname */
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        if (strcmp(endpoint->host, hostname) == 0) {
            return true;
        }
    }
    
    /* TODO: More sophisticated local IP detection */
    /* For now, assume false if not explicitly localhost */
    return false;
}

bool buckets_endpoint_has_ellipses(const char *str)
{
    if (!str) {
        return false;
    }
    
    /* Look for pattern {N...M} */
    const char *open = strchr(str, '{');
    if (!open) {
        return false;
    }
    
    const char *dots = strstr(open, "...");
    if (!dots) {
        return false;
    }
    
    const char *close = strchr(dots, '}');
    if (!close) {
        return false;
    }
    
    return true;
}

buckets_expansion_pattern_t *buckets_expansion_pattern_parse(const char *str)
{
    if (!str || !buckets_endpoint_has_ellipses(str)) {
        buckets_error("No ellipses pattern found in string");
        return NULL;
    }
    
    buckets_expansion_pattern_t *pattern = buckets_calloc(1, sizeof(buckets_expansion_pattern_t));
    if (!pattern) {
        return NULL;
    }
    
    /* Find pattern boundaries */
    const char *open = strchr(str, '{');
    const char *dots = strstr(open, "...");
    const char *close = strchr(dots, '}');
    
    /* Extract prefix (everything before {) */
    size_t prefix_len = open - str;
    if (prefix_len > 0) {
        pattern->prefix = strndup_safe(str, prefix_len);
    } else {
        pattern->prefix = buckets_strdup("");
    }
    
    /* Extract suffix (everything after }) */
    if (close[1] != '\0') {
        pattern->suffix = buckets_strdup(close + 1);
    } else {
        pattern->suffix = buckets_strdup("");
    }
    
    /* Extract start and end values */
    const char *start_str = open + 1;
    size_t start_len = dots - start_str;
    const char *end_str = dots + 3;
    size_t end_len = close - end_str;
    
    /* Determine if numeric or alphabetic */
    bool is_numeric = isdigit(start_str[0]) || (start_str[0] == '-');
    pattern->is_numeric = is_numeric;
    
    if (is_numeric) {
        /* Parse numeric range */
        char *start_copy = strndup_safe(start_str, start_len);
        char *end_copy = strndup_safe(end_str, end_len);
        
        pattern->range.numeric.start = atoll(start_copy);
        pattern->range.numeric.end = atoll(end_copy);
        
        buckets_free(start_copy);
        buckets_free(end_copy);
        
        if (pattern->range.numeric.start > pattern->range.numeric.end) {
            buckets_error("Invalid numeric range: start > end");
            buckets_expansion_pattern_free(pattern);
            return NULL;
        }
    } else {
        /* Parse alphabetic range */
        if (start_len != 1 || end_len != 1) {
            buckets_error("Alphabetic range must be single characters");
            buckets_expansion_pattern_free(pattern);
            return NULL;
        }
        
        pattern->range.alpha.start = start_str[0];
        pattern->range.alpha.end = end_str[0];
        
        if (!isalpha(pattern->range.alpha.start) || !isalpha(pattern->range.alpha.end)) {
            buckets_error("Alphabetic range must contain letters");
            buckets_expansion_pattern_free(pattern);
            return NULL;
        }
        
        if (pattern->range.alpha.start > pattern->range.alpha.end) {
            buckets_error("Invalid alphabetic range: start > end");
            buckets_expansion_pattern_free(pattern);
            return NULL;
        }
    }
    
    return pattern;
}

char **buckets_expansion_pattern_expand(const buckets_expansion_pattern_t *pattern,
                                        size_t *out_count)
{
    if (!pattern || !out_count) {
        return NULL;
    }
    
    size_t count;
    if (pattern->is_numeric) {
        count = (size_t)(pattern->range.numeric.end - pattern->range.numeric.start + 1);
    } else {
        count = (size_t)(pattern->range.alpha.end - pattern->range.alpha.start + 1);
    }
    
    char **results = buckets_calloc(count, sizeof(char *));
    if (!results) {
        return NULL;
    }
    
    for (size_t i = 0; i < count; i++) {
        if (pattern->is_numeric) {
            i64 value = pattern->range.numeric.start + (i64)i;
            results[i] = buckets_format("%s%lld%s", 
                                       pattern->prefix,
                                       (long long)value,
                                       pattern->suffix);
        } else {
            char value = pattern->range.alpha.start + (char)i;
            results[i] = buckets_format("%s%c%s",
                                       pattern->prefix,
                                       value,
                                       pattern->suffix);
        }
        
        if (!results[i]) {
            /* Cleanup on allocation failure */
            for (size_t j = 0; j < i; j++) {
                buckets_free(results[j]);
            }
            buckets_free(results);
            return NULL;
        }
    }
    
    *out_count = count;
    return results;
}

void buckets_expansion_pattern_free(buckets_expansion_pattern_t *pattern)
{
    if (!pattern) {
        return;
    }
    
    buckets_free(pattern->prefix);
    buckets_free(pattern->suffix);
    buckets_free(pattern);
}

buckets_endpoints_t *buckets_endpoints_parse(const char **args, size_t count)
{
    if (!args || count == 0) {
        buckets_error("NULL or empty args array");
        return NULL;
    }
    
    buckets_endpoints_t *endpoints = buckets_calloc(1, sizeof(buckets_endpoints_t));
    if (!endpoints) {
        return NULL;
    }
    
    /* First pass: count total endpoints after expansion */
    size_t total_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (buckets_endpoint_has_ellipses(args[i])) {
            buckets_expansion_pattern_t *pattern = buckets_expansion_pattern_parse(args[i]);
            if (!pattern) {
                buckets_endpoints_free(endpoints);
                return NULL;
            }
            
            size_t expanded_count = 0;
            if (pattern->is_numeric) {
                expanded_count = (size_t)(pattern->range.numeric.end - 
                                         pattern->range.numeric.start + 1);
            } else {
                expanded_count = (size_t)(pattern->range.alpha.end - 
                                         pattern->range.alpha.start + 1);
            }
            
            total_count += expanded_count;
            buckets_expansion_pattern_free(pattern);
        } else {
            total_count++;
        }
    }
    
    /* Allocate endpoint array */
    endpoints->endpoints = buckets_calloc(total_count, sizeof(buckets_endpoint_t));
    if (!endpoints->endpoints) {
        buckets_free(endpoints);
        return NULL;
    }
    
    /* Second pass: parse and expand endpoints */
    size_t idx = 0;
    for (size_t i = 0; i < count; i++) {
        if (buckets_endpoint_has_ellipses(args[i])) {
            /* Expand ellipses pattern */
            buckets_expansion_pattern_t *pattern = buckets_expansion_pattern_parse(args[i]);
            if (!pattern) {
                buckets_endpoints_free(endpoints);
                return NULL;
            }
            
            size_t expanded_count;
            char **expanded = buckets_expansion_pattern_expand(pattern, &expanded_count);
            if (!expanded) {
                buckets_expansion_pattern_free(pattern);
                buckets_endpoints_free(endpoints);
                return NULL;
            }
            
            /* Parse each expanded endpoint */
            for (size_t j = 0; j < expanded_count; j++) {
                buckets_endpoint_t *ep = buckets_endpoint_parse(expanded[j]);
                if (!ep) {
                    for (size_t k = 0; k < expanded_count; k++) {
                        buckets_free(expanded[k]);
                    }
                    buckets_free(expanded);
                    buckets_expansion_pattern_free(pattern);
                    buckets_endpoints_free(endpoints);
                    return NULL;
                }
                endpoints->endpoints[idx++] = *ep;
                buckets_free(ep); /* Copy struct, free container */
                buckets_free(expanded[j]);
            }
            
            buckets_free(expanded);
            buckets_expansion_pattern_free(pattern);
        } else {
            /* Parse single endpoint */
            buckets_endpoint_t *ep = buckets_endpoint_parse(args[i]);
            if (!ep) {
                buckets_endpoints_free(endpoints);
                return NULL;
            }
            endpoints->endpoints[idx++] = *ep;
            buckets_free(ep); /* Copy struct, free container */
        }
    }
    
    endpoints->count = total_count;
    return endpoints;
}

void buckets_endpoints_free(buckets_endpoints_t *endpoints)
{
    if (!endpoints) {
        return;
    }
    
    if (endpoints->endpoints) {
        for (size_t i = 0; i < endpoints->count; i++) {
            /* Free individual endpoint fields (but not the struct itself) */
            buckets_free(endpoints->endpoints[i].original);
            buckets_free(endpoints->endpoints[i].scheme);
            buckets_free(endpoints->endpoints[i].host);
            buckets_free(endpoints->endpoints[i].path);
        }
        buckets_free(endpoints->endpoints);
    }
    
    buckets_free(endpoints);
}

buckets_endpoint_set_t *buckets_endpoints_to_sets(const buckets_endpoints_t *endpoints,
                                                   size_t disks_per_set,
                                                   size_t *out_set_count)
{
    if (!endpoints || !out_set_count || disks_per_set == 0) {
        return NULL;
    }
    
    size_t total_disks = endpoints->count;
    if (total_disks % disks_per_set != 0) {
        buckets_error("Total disks (%zu) not divisible by disks_per_set (%zu)",
                     total_disks, disks_per_set);
        return NULL;
    }
    
    size_t num_sets = total_disks / disks_per_set;
    buckets_endpoint_set_t *sets = buckets_calloc(num_sets, sizeof(buckets_endpoint_set_t));
    if (!sets) {
        return NULL;
    }
    
    /* Divide endpoints into sets */
    for (size_t i = 0; i < num_sets; i++) {
        sets[i].count = disks_per_set;
        sets[i].endpoints = buckets_calloc(disks_per_set, sizeof(buckets_endpoint_t *));
        if (!sets[i].endpoints) {
            buckets_endpoint_sets_free(sets, i);
            return NULL;
        }
        
        for (size_t j = 0; j < disks_per_set; j++) {
            size_t idx = i * disks_per_set + j;
            sets[i].endpoints[j] = &endpoints->endpoints[idx];
            
            /* Assign cluster placement indices */
            sets[i].endpoints[j]->set_idx = (i32)i;
            sets[i].endpoints[j]->disk_idx = (i32)j;
        }
    }
    
    *out_set_count = num_sets;
    return sets;
}

void buckets_endpoint_sets_free(buckets_endpoint_set_t *sets, size_t count)
{
    if (!sets) {
        return;
    }
    
    for (size_t i = 0; i < count; i++) {
        buckets_free(sets[i].endpoints);
    }
    
    buckets_free(sets);
}
