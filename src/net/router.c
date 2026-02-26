/**
 * HTTP Router Implementation
 * 
 * Provides request routing with pattern matching for HTTP handlers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>

#include "buckets.h"
#include "buckets_net.h"

/* ===================================================================
 * Router Structure
 * ===================================================================*/

/**
 * Single route entry
 */
typedef struct {
    char *method;                     /* HTTP method (or "*" for all) */
    char *path;                       /* URL path pattern (supports "*" wildcard) */
    buckets_http_handler_t handler;   /* Handler function */
    void *user_data;                  /* User data for handler */
} route_entry_t;

/**
 * Router structure
 */
struct buckets_router {
    route_entry_t *routes;            /* Dynamic array of routes */
    size_t count;                     /* Number of routes */
    size_t capacity;                  /* Allocated capacity */
};

/* ===================================================================
 * Router API
 * ===================================================================*/

buckets_router_t* buckets_router_create(void)
{
    buckets_router_t *router = buckets_calloc(1, sizeof(buckets_router_t));
    if (!router) {
        return NULL;
    }
    
    router->routes = NULL;
    router->count = 0;
    router->capacity = 0;
    
    return router;
}

int buckets_router_add_route(buckets_router_t *router,
                              const char *method,
                              const char *path,
                              buckets_http_handler_t handler,
                              void *user_data)
{
    if (!router || !method || !path || !handler) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Grow array if needed */
    if (router->count >= router->capacity) {
        size_t new_capacity = router->capacity == 0 ? 8 : router->capacity * 2;
        route_entry_t *new_routes = buckets_realloc(router->routes, 
                                                     new_capacity * sizeof(route_entry_t));
        if (!new_routes) {
            return BUCKETS_ERR_NOMEM;
        }
        router->routes = new_routes;
        router->capacity = new_capacity;
    }
    
    /* Add route */
    route_entry_t *entry = &router->routes[router->count];
    entry->method = buckets_strdup(method);
    entry->path = buckets_strdup(path);
    entry->handler = handler;
    entry->user_data = user_data;
    
    if (!entry->method || !entry->path) {
        if (entry->method) buckets_free(entry->method);
        if (entry->path) buckets_free(entry->path);
        return BUCKETS_ERR_NOMEM;
    }
    
    router->count++;
    
    return BUCKETS_OK;
}

int buckets_router_match(buckets_router_t *router,
                         const char *method,
                         const char *path,
                         buckets_route_match_t *match)
{
    if (!router || !method || !path || !match) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Initialize match as not found */
    match->handler = NULL;
    match->user_data = NULL;
    match->matched = false;
    
    /* Try to find matching route */
    for (size_t i = 0; i < router->count; i++) {
        route_entry_t *entry = &router->routes[i];
        
        /* Check method match (exact or wildcard) */
        bool method_matches = (strcmp(entry->method, "*") == 0) ||
                              (strcmp(entry->method, method) == 0);
        
        if (!method_matches) {
            continue;
        }
        
        /* Check path match (exact or wildcard pattern) */
        bool path_matches = false;
        
        if (strcmp(entry->path, path) == 0) {
            /* Exact match */
            path_matches = true;
        } else if (strchr(entry->path, '*') != NULL) {
            /* Pattern match using fnmatch */
            if (fnmatch(entry->path, path, 0) == 0) {
                path_matches = true;
            }
        }
        
        if (path_matches) {
            /* Found a match */
            match->handler = entry->handler;
            match->user_data = entry->user_data;
            match->matched = true;
            return BUCKETS_OK;
        }
    }
    
    /* No match found */
    return BUCKETS_OK;
}

int buckets_router_get_route_count(buckets_router_t *router)
{
    if (!router) {
        return 0;
    }
    return (int)router->count;
}

void buckets_router_free(buckets_router_t *router)
{
    if (!router) {
        return;
    }
    
    /* Free all routes */
    for (size_t i = 0; i < router->count; i++) {
        route_entry_t *entry = &router->routes[i];
        if (entry->method) {
            buckets_free(entry->method);
        }
        if (entry->path) {
            buckets_free(entry->path);
        }
    }
    
    /* Free routes array */
    if (router->routes) {
        buckets_free(router->routes);
    }
    
    buckets_free(router);
}
