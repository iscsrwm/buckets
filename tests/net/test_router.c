/**
 * Router Tests
 * 
 * Tests for HTTP router with pattern matching using Criterion framework.
 */

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buckets.h"
#include "buckets_net.h"

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

static buckets_router_t *router = NULL;

void setup(void)
{
    buckets_init();
}

void teardown(void)
{
    if (router) {
        buckets_router_free(router);
        router = NULL;
    }
    buckets_cleanup();
}

TestSuite(router, .init = setup, .fini = teardown);

/* ===================================================================
 * Test Handlers
 * ===================================================================*/

static void handler_home(buckets_http_request_t *req,
                         buckets_http_response_t *res,
                         void *user_data)
{
    (void)req;
    (void)res;
    (void)user_data;
}

static void handler_health(buckets_http_request_t *req,
                           buckets_http_response_t *res,
                           void *user_data)
{
    (void)req;
    (void)res;
    (void)user_data;
}

static void handler_buckets(buckets_http_request_t *req,
                            buckets_http_response_t *res,
                            void *user_data)
{
    (void)req;
    (void)res;
    (void)user_data;
}

static void handler_wildcard(buckets_http_request_t *req,
                             buckets_http_response_t *res,
                             void *user_data)
{
    (void)req;
    (void)res;
    (void)user_data;
}

/* ===================================================================
 * Tests
 * ===================================================================*/

Test(router, create_router)
{
    router = buckets_router_create();
    cr_assert_not_null(router, "Router should be created");
    
    int count = buckets_router_get_route_count(router);
    cr_assert_eq(count, 0, "New router should have 0 routes");
}

Test(router, add_route)
{
    router = buckets_router_create();
    cr_assert_not_null(router);
    
    int ret = buckets_router_add_route(router, "GET", "/", handler_home, NULL);
    cr_assert_eq(ret, BUCKETS_OK, "Should add route successfully");
    
    int count = buckets_router_get_route_count(router);
    cr_assert_eq(count, 1, "Router should have 1 route");
}

Test(router, add_multiple_routes)
{
    router = buckets_router_create();
    cr_assert_not_null(router);
    
    buckets_router_add_route(router, "GET", "/", handler_home, NULL);
    buckets_router_add_route(router, "GET", "/health", handler_health, NULL);
    buckets_router_add_route(router, "GET", "/buckets", handler_buckets, NULL);
    
    int count = buckets_router_get_route_count(router);
    cr_assert_eq(count, 3, "Router should have 3 routes");
}

Test(router, match_exact_route)
{
    router = buckets_router_create();
    cr_assert_not_null(router);
    
    buckets_router_add_route(router, "GET", "/health", handler_health, NULL);
    
    buckets_route_match_t match;
    int ret = buckets_router_match(router, "GET", "/health", &match);
    
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert(match.matched, "Should match route");
    cr_assert_eq(match.handler, handler_health, "Should return correct handler");
}

Test(router, match_not_found)
{
    router = buckets_router_create();
    cr_assert_not_null(router);
    
    buckets_router_add_route(router, "GET", "/health", handler_health, NULL);
    
    buckets_route_match_t match;
    int ret = buckets_router_match(router, "GET", "/notfound", &match);
    
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_not(match.matched, "Should not match");
    cr_assert_null(match.handler, "Handler should be NULL");
}

Test(router, match_wildcard_route)
{
    router = buckets_router_create();
    cr_assert_not_null(router);
    
    buckets_router_add_route(router, "GET", "/buckets/*", handler_wildcard, NULL);
    
    buckets_route_match_t match;
    
    /* Test exact wildcard match */
    int ret = buckets_router_match(router, "GET", "/buckets/mybucket", &match);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert(match.matched, "Should match wildcard route");
    cr_assert_eq(match.handler, handler_wildcard);
    
    /* Test longer path */
    ret = buckets_router_match(router, "GET", "/buckets/mybucket/object.txt", &match);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert(match.matched, "Should match wildcard route with longer path");
}

Test(router, match_method_wildcard)
{
    router = buckets_router_create();
    cr_assert_not_null(router);
    
    buckets_router_add_route(router, "*", "/health", handler_health, NULL);
    
    buckets_route_match_t match;
    
    /* Test GET */
    int ret = buckets_router_match(router, "GET", "/health", &match);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert(match.matched, "Should match GET with wildcard method");
    
    /* Test POST */
    ret = buckets_router_match(router, "POST", "/health", &match);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert(match.matched, "Should match POST with wildcard method");
    
    /* Test DELETE */
    ret = buckets_router_match(router, "DELETE", "/health", &match);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert(match.matched, "Should match DELETE with wildcard method");
}

Test(router, match_method_specific)
{
    router = buckets_router_create();
    cr_assert_not_null(router);
    
    buckets_router_add_route(router, "GET", "/health", handler_health, NULL);
    
    buckets_route_match_t match;
    
    /* Test GET - should match */
    int ret = buckets_router_match(router, "GET", "/health", &match);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert(match.matched, "Should match GET");
    
    /* Test POST - should not match */
    ret = buckets_router_match(router, "POST", "/health", &match);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_not(match.matched, "Should not match POST");
}

Test(router, route_priority_first_match)
{
    router = buckets_router_create();
    cr_assert_not_null(router);
    
    /* Add routes in order - first match wins */
    buckets_router_add_route(router, "GET", "/buckets/*", handler_wildcard, NULL);
    buckets_router_add_route(router, "GET", "/buckets/special", handler_buckets, NULL);
    
    buckets_route_match_t match;
    
    /* Should match first route (wildcard) */
    int ret = buckets_router_match(router, "GET", "/buckets/special", &match);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert(match.matched);
    cr_assert_eq(match.handler, handler_wildcard, "First matching route should win");
}

Test(router, invalid_args)
{
    router = buckets_router_create();
    cr_assert_not_null(router);
    
    /* Test add_route with NULL args */
    int ret = buckets_router_add_route(NULL, "GET", "/", handler_home, NULL);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with NULL router");
    
    ret = buckets_router_add_route(router, NULL, "/", handler_home, NULL);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with NULL method");
    
    ret = buckets_router_add_route(router, "GET", NULL, handler_home, NULL);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with NULL path");
    
    ret = buckets_router_add_route(router, "GET", "/", NULL, NULL);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with NULL handler");
    
    /* Test match with NULL args */
    buckets_route_match_t match;
    ret = buckets_router_match(NULL, "GET", "/", &match);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with NULL router");
    
    ret = buckets_router_match(router, NULL, "/", &match);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with NULL method");
    
    ret = buckets_router_match(router, "GET", NULL, &match);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with NULL path");
    
    ret = buckets_router_match(router, "GET", "/", NULL);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with NULL match");
}

Test(router, user_data)
{
    router = buckets_router_create();
    cr_assert_not_null(router);
    
    int test_data = 42;
    buckets_router_add_route(router, "GET", "/test", handler_home, &test_data);
    
    buckets_route_match_t match;
    int ret = buckets_router_match(router, "GET", "/test", &match);
    
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert(match.matched);
    cr_assert_eq(match.user_data, &test_data, "User data should be preserved");
}
