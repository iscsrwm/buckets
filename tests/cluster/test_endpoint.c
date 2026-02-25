/**
 * Endpoint Parsing Tests
 * 
 * Test suite for endpoint URL parsing and ellipses expansion.
 */

#include <criterion/criterion.h>
#include <string.h>

#include "buckets.h"
#include "buckets_endpoint.h"

/* Test: Parse simple path endpoint */
Test(endpoint, parse_path_endpoint)
{
    buckets_endpoint_t *ep = buckets_endpoint_parse("/mnt/disk1");
    cr_assert_not_null(ep);
    cr_assert_eq(ep->type, BUCKETS_ENDPOINT_TYPE_PATH);
    cr_assert_str_eq(ep->path, "/mnt/disk1");
    cr_assert_eq(ep->is_local, true);
    cr_assert_eq(ep->pool_idx, -1);
    cr_assert_eq(ep->set_idx, -1);
    cr_assert_eq(ep->disk_idx, -1);
    
    buckets_endpoint_free(ep);
}

/* Test: Parse HTTP URL endpoint */
Test(endpoint, parse_http_endpoint)
{
    buckets_endpoint_t *ep = buckets_endpoint_parse("http://node1.example.com:9000/mnt/disk1");
    cr_assert_not_null(ep);
    cr_assert_eq(ep->type, BUCKETS_ENDPOINT_TYPE_URL);
    cr_assert_str_eq(ep->scheme, "http");
    cr_assert_str_eq(ep->host, "node1.example.com");
    cr_assert_eq(ep->port, 9000);
    cr_assert_str_eq(ep->path, "/mnt/disk1");
    
    buckets_endpoint_free(ep);
}

/* Test: Parse HTTPS URL endpoint */
Test(endpoint, parse_https_endpoint)
{
    buckets_endpoint_t *ep = buckets_endpoint_parse("https://storage.example.com:443/data");
    cr_assert_not_null(ep);
    cr_assert_eq(ep->type, BUCKETS_ENDPOINT_TYPE_URL);
    cr_assert_str_eq(ep->scheme, "https");
    cr_assert_str_eq(ep->host, "storage.example.com");
    cr_assert_eq(ep->port, 443);
    cr_assert_str_eq(ep->path, "/data");
    
    buckets_endpoint_free(ep);
}

/* Test: Parse URL without port */
Test(endpoint, parse_url_no_port)
{
    buckets_endpoint_t *ep = buckets_endpoint_parse("http://localhost/mnt/disk");
    cr_assert_not_null(ep);
    cr_assert_str_eq(ep->host, "localhost");
    cr_assert_eq(ep->port, 0); /* Default port */
    cr_assert_str_eq(ep->path, "/mnt/disk");
    
    buckets_endpoint_free(ep);
}

/* Test: Parse URL without path */
Test(endpoint, parse_url_no_path)
{
    buckets_endpoint_t *ep = buckets_endpoint_parse("http://node1:9000");
    cr_assert_not_null(ep);
    cr_assert_str_eq(ep->host, "node1");
    cr_assert_eq(ep->port, 9000);
    cr_assert_str_eq(ep->path, "/"); /* Default path */
    
    buckets_endpoint_free(ep);
}

/* Test: Parse IPv4 address endpoint */
Test(endpoint, parse_ipv4_endpoint)
{
    buckets_endpoint_t *ep = buckets_endpoint_parse("http://192.168.1.100:9000/data");
    cr_assert_not_null(ep);
    cr_assert_str_eq(ep->host, "192.168.1.100");
    cr_assert_eq(ep->port, 9000);
    
    buckets_endpoint_free(ep);
}

/* Test: Parse IPv6 address endpoint */
Test(endpoint, parse_ipv6_endpoint)
{
    buckets_endpoint_t *ep = buckets_endpoint_parse("http://[::1]:9000/data");
    cr_assert_not_null(ep);
    cr_assert_str_eq(ep->host, "::1");
    cr_assert_eq(ep->port, 9000);
    
    buckets_endpoint_free(ep);
}

/* Test: Validate valid endpoint */
Test(endpoint, validate_valid_endpoint)
{
    buckets_endpoint_t *ep = buckets_endpoint_parse("http://node1:9000/data");
    cr_assert_not_null(ep);
    
    buckets_error_t err = buckets_endpoint_validate(ep);
    cr_assert_eq(err, BUCKETS_OK);
    
    buckets_endpoint_free(ep);
}

/* Test: Endpoint to string conversion */
Test(endpoint, endpoint_to_string)
{
    buckets_endpoint_t *ep = buckets_endpoint_parse("http://node1:9000/mnt/disk1");
    cr_assert_not_null(ep);
    
    char *str = buckets_endpoint_to_string(ep);
    cr_assert_str_eq(str, "http://node1:9000/mnt/disk1");
    
    buckets_free(str);
    buckets_endpoint_free(ep);
}

/* Test: Endpoint equality */
Test(endpoint, endpoint_equal)
{
    buckets_endpoint_t *ep1 = buckets_endpoint_parse("http://node1:9000/data");
    buckets_endpoint_t *ep2 = buckets_endpoint_parse("http://node1:9000/data");
    buckets_endpoint_t *ep3 = buckets_endpoint_parse("http://node2:9000/data");
    
    cr_assert(buckets_endpoint_equal(ep1, ep2));
    cr_assert_not(buckets_endpoint_equal(ep1, ep3));
    
    buckets_endpoint_free(ep1);
    buckets_endpoint_free(ep2);
    buckets_endpoint_free(ep3);
}

/* Test: Detect ellipses pattern */
Test(endpoint, has_ellipses)
{
    cr_assert(buckets_endpoint_has_ellipses("node{1...4}"));
    cr_assert(buckets_endpoint_has_ellipses("disk{a...d}"));
    cr_assert(buckets_endpoint_has_ellipses("http://node{1...4}/disk"));
    cr_assert_not(buckets_endpoint_has_ellipses("/mnt/disk1"));
    cr_assert_not(buckets_endpoint_has_ellipses("http://node1:9000"));
}

/* Test: Parse numeric ellipses pattern */
Test(endpoint, parse_numeric_pattern)
{
    buckets_expansion_pattern_t *pattern = buckets_expansion_pattern_parse("node{1...4}");
    cr_assert_not_null(pattern);
    cr_assert_str_eq(pattern->prefix, "node");
    cr_assert_str_eq(pattern->suffix, "");
    cr_assert_eq(pattern->is_numeric, true);
    cr_assert_eq(pattern->range.numeric.start, 1);
    cr_assert_eq(pattern->range.numeric.end, 4);
    
    buckets_expansion_pattern_free(pattern);
}

/* Test: Parse alphabetic ellipses pattern */
Test(endpoint, parse_alpha_pattern)
{
    buckets_expansion_pattern_t *pattern = buckets_expansion_pattern_parse("disk{a...d}");
    cr_assert_not_null(pattern);
    cr_assert_str_eq(pattern->prefix, "disk");
    cr_assert_str_eq(pattern->suffix, "");
    cr_assert_eq(pattern->is_numeric, false);
    cr_assert_eq(pattern->range.alpha.start, 'a');
    cr_assert_eq(pattern->range.alpha.end, 'd');
    
    buckets_expansion_pattern_free(pattern);
}

/* Test: Expand numeric pattern */
Test(endpoint, expand_numeric_pattern)
{
    buckets_expansion_pattern_t *pattern = buckets_expansion_pattern_parse("node{1...4}");
    cr_assert_not_null(pattern);
    
    size_t count;
    char **expanded = buckets_expansion_pattern_expand(pattern, &count);
    cr_assert_not_null(expanded);
    cr_assert_eq(count, 4);
    cr_assert_str_eq(expanded[0], "node1");
    cr_assert_str_eq(expanded[1], "node2");
    cr_assert_str_eq(expanded[2], "node3");
    cr_assert_str_eq(expanded[3], "node4");
    
    for (size_t i = 0; i < count; i++) {
        buckets_free(expanded[i]);
    }
    buckets_free(expanded);
    buckets_expansion_pattern_free(pattern);
}

/* Test: Expand alphabetic pattern */
Test(endpoint, expand_alpha_pattern)
{
    buckets_expansion_pattern_t *pattern = buckets_expansion_pattern_parse("disk{a...c}");
    cr_assert_not_null(pattern);
    
    size_t count;
    char **expanded = buckets_expansion_pattern_expand(pattern, &count);
    cr_assert_not_null(expanded);
    cr_assert_eq(count, 3);
    cr_assert_str_eq(expanded[0], "diska");
    cr_assert_str_eq(expanded[1], "diskb");
    cr_assert_str_eq(expanded[2], "diskc");
    
    for (size_t i = 0; i < count; i++) {
        buckets_free(expanded[i]);
    }
    buckets_free(expanded);
    buckets_expansion_pattern_free(pattern);
}

/* Test: Parse endpoints with ellipses */
Test(endpoint, parse_endpoints_with_ellipses)
{
    const char *args[] = {"http://node{1...3}:9000/mnt/disk1"};
    
    buckets_endpoints_t *endpoints = buckets_endpoints_parse(args, 1);
    cr_assert_not_null(endpoints);
    cr_assert_eq(endpoints->count, 3);
    cr_assert_str_eq(endpoints->endpoints[0].host, "node1");
    cr_assert_str_eq(endpoints->endpoints[1].host, "node2");
    cr_assert_str_eq(endpoints->endpoints[2].host, "node3");
    
    buckets_endpoints_free(endpoints);
}

/* Test: Parse multiple path endpoints */
Test(endpoint, parse_multiple_endpoints)
{
    const char *args[] = {"/mnt/disk1", "/mnt/disk2", "/mnt/disk3", "/mnt/disk4"};
    
    buckets_endpoints_t *endpoints = buckets_endpoints_parse(args, 4);
    cr_assert_not_null(endpoints);
    cr_assert_eq(endpoints->count, 4);
    cr_assert_str_eq(endpoints->endpoints[0].path, "/mnt/disk1");
    cr_assert_str_eq(endpoints->endpoints[3].path, "/mnt/disk4");
    
    buckets_endpoints_free(endpoints);
}

/* Test: Organize endpoints into sets */
Test(endpoint, endpoints_to_sets)
{
    const char *args[] = {
        "/mnt/disk1", "/mnt/disk2", "/mnt/disk3", "/mnt/disk4",
        "/mnt/disk5", "/mnt/disk6", "/mnt/disk7", "/mnt/disk8"
    };
    
    buckets_endpoints_t *endpoints = buckets_endpoints_parse(args, 8);
    cr_assert_not_null(endpoints);
    
    size_t set_count;
    buckets_endpoint_set_t *sets = buckets_endpoints_to_sets(endpoints, 4, &set_count);
    cr_assert_not_null(sets);
    cr_assert_eq(set_count, 2); /* 8 disks / 4 per set = 2 sets */
    cr_assert_eq(sets[0].count, 4);
    cr_assert_eq(sets[1].count, 4);
    
    /* Check set indices assigned correctly */
    cr_assert_eq(sets[0].endpoints[0]->set_idx, 0);
    cr_assert_eq(sets[0].endpoints[0]->disk_idx, 0);
    cr_assert_eq(sets[1].endpoints[0]->set_idx, 1);
    cr_assert_eq(sets[1].endpoints[0]->disk_idx, 0);
    
    buckets_endpoint_sets_free(sets, set_count);
    buckets_endpoints_free(endpoints);
}

/* Test: NULL input handling */
Test(endpoint, null_inputs)
{
    cr_assert_null(buckets_endpoint_parse(NULL));
    cr_assert_null(buckets_endpoint_parse(""));
    cr_assert_null(buckets_expansion_pattern_parse(NULL));
    cr_assert_null(buckets_endpoints_parse(NULL, 0));
}

/* Test: Invalid URL scheme */
Test(endpoint, invalid_scheme)
{
    buckets_endpoint_t *ep = buckets_endpoint_parse("ftp://node1/data");
    cr_assert_null(ep);
}

/* Test: Invalid port number */
Test(endpoint, invalid_port)
{
    buckets_endpoint_t *ep = buckets_endpoint_parse("http://node1:99999/data");
    cr_assert_null(ep);
}

/* Test: Localhost detection */
Test(endpoint, is_local_detection)
{
    buckets_endpoint_t *ep1 = buckets_endpoint_parse("http://localhost/data");
    cr_assert(buckets_endpoint_is_local(ep1));
    
    buckets_endpoint_t *ep2 = buckets_endpoint_parse("http://127.0.0.1/data");
    cr_assert(buckets_endpoint_is_local(ep2));
    
    buckets_endpoint_t *ep3 = buckets_endpoint_parse("/mnt/disk1");
    cr_assert_eq(ep3->is_local, true);
    
    buckets_endpoint_free(ep1);
    buckets_endpoint_free(ep2);
    buckets_endpoint_free(ep3);
}
