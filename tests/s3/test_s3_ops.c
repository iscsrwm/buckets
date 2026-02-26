/**
 * S3 Operations Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <criterion/criterion.h>

#include "buckets.h"
#include "buckets_s3.h"

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

void setup_s3_ops(void)
{
    buckets_init();
    /* Create test data directory */
    int ret = system("mkdir -p /tmp/buckets-data");
    (void)ret; /* Ignore return value */
}

void teardown_s3_ops(void)
{
    /* Cleanup test data */
    int ret = system("rm -rf /tmp/buckets-data");
    (void)ret; /* Ignore return value */
    buckets_cleanup();
}

TestSuite(s3_ops, .init = setup_s3_ops, .fini = teardown_s3_ops);

/* ===================================================================
 * Utility Tests
 * ===================================================================*/

Test(s3_ops, calculate_etag)
{
    char etag[65];
    const char *data = "Hello, World!";
    
    int ret = buckets_s3_calculate_etag(data, strlen(data), etag);
    cr_assert_eq(ret, BUCKETS_OK, "ETag calculation should succeed");
    cr_assert_eq(strlen(etag), 32, "ETag should be 32 hex characters");
}

Test(s3_ops, validate_bucket_name)
{
    cr_assert(buckets_s3_validate_bucket_name("mybucket"), "Simple name should be valid");
    cr_assert(buckets_s3_validate_bucket_name("my-bucket"), "Hyphens should be valid");
    cr_assert(buckets_s3_validate_bucket_name("my.bucket"), "Dots should be valid");
    cr_assert(buckets_s3_validate_bucket_name("mybucket123"), "Numbers should be valid");
    
    cr_assert(!buckets_s3_validate_bucket_name("ab"), "Too short should be invalid");
    cr_assert(!buckets_s3_validate_bucket_name("MyBucket"), "Uppercase should be invalid");
    cr_assert(!buckets_s3_validate_bucket_name("my_bucket"), "Underscores should be invalid");
    cr_assert(!buckets_s3_validate_bucket_name("mybucket-"), "Ending with dash should be invalid");
}

Test(s3_ops, validate_object_key)
{
    cr_assert(buckets_s3_validate_object_key("myobject"), "Simple key should be valid");
    cr_assert(buckets_s3_validate_object_key("path/to/object"), "Path should be valid");
    cr_assert(buckets_s3_validate_object_key("object-123_456.txt"), "Mixed chars should be valid");
    
    cr_assert(!buckets_s3_validate_object_key(""), "Empty key should be invalid");
    cr_assert(!buckets_s3_validate_object_key("/object"), "Starting with slash should be invalid");
}

/* ===================================================================
 * PUT/GET Tests
 * ===================================================================*/

Test(s3_ops, put_get_object)
{
    /* Create mock request for PUT */
    buckets_s3_request_t req;
    memset(&req, 0, sizeof(req));
    strcpy(req.bucket, "testbucket");
    strcpy(req.key, "testkey");
    req.body = "Test data content";
    req.body_len = strlen(req.body);
    
    /* PUT object */
    buckets_s3_response_t put_res;
    memset(&put_res, 0, sizeof(put_res));
    
    int ret = buckets_s3_put_object(&req, &put_res);
    cr_assert_eq(ret, BUCKETS_OK, "PUT should succeed");
    cr_assert_eq(put_res.status_code, 200, "PUT should return 200");
    cr_assert(put_res.etag[0] != '\0', "PUT should generate ETag");
    
    char etag_from_put[65];
    strcpy(etag_from_put, put_res.etag);
    
    if (put_res.body) buckets_free(put_res.body);
    
    /* GET object */
    buckets_s3_response_t get_res;
    memset(&get_res, 0, sizeof(get_res));
    
    ret = buckets_s3_get_object(&req, &get_res);
    cr_assert_eq(ret, BUCKETS_OK, "GET should succeed");
    cr_assert_eq(get_res.status_code, 200, "GET should return 200");
    cr_assert_not_null(get_res.body, "GET should return body");
    cr_assert_eq(get_res.body_len, strlen("Test data content"),
                 "GET should return correct length");
    cr_assert_str_eq(get_res.body, "Test data content",
                     "GET should return correct content");
    cr_assert_str_eq(get_res.etag, etag_from_put,
                     "GET ETag should match PUT ETag");
    
    if (get_res.body) buckets_free(get_res.body);
}

Test(s3_ops, get_nonexistent_object)
{
    buckets_s3_request_t req;
    memset(&req, 0, sizeof(req));
    strcpy(req.bucket, "testbucket");
    strcpy(req.key, "nonexistent");
    
    buckets_s3_response_t res;
    memset(&res, 0, sizeof(res));
    
    int ret = buckets_s3_get_object(&req, &res);
    cr_assert_eq(ret, BUCKETS_OK, "Should succeed (error response)");
    cr_assert_eq(res.status_code, 404, "Should return 404");
    cr_assert_str_eq(res.error_code, "NoSuchKey", "Error code should be NoSuchKey");
    
    if (res.body) buckets_free(res.body);
}

Test(s3_ops, delete_object)
{
    /* First create an object */
    buckets_s3_request_t req;
    memset(&req, 0, sizeof(req));
    strcpy(req.bucket, "testbucket");
    strcpy(req.key, "deleteme");
    req.body = "delete this";
    req.body_len = strlen(req.body);
    
    buckets_s3_response_t put_res;
    memset(&put_res, 0, sizeof(put_res));
    buckets_s3_put_object(&req, &put_res);
    if (put_res.body) buckets_free(put_res.body);
    
    /* DELETE it */
    buckets_s3_response_t del_res;
    memset(&del_res, 0, sizeof(del_res));
    
    int ret = buckets_s3_delete_object(&req, &del_res);
    cr_assert_eq(ret, BUCKETS_OK, "DELETE should succeed");
    cr_assert_eq(del_res.status_code, 204, "DELETE should return 204");
    
    /* Verify it's gone */
    buckets_s3_response_t get_res;
    memset(&get_res, 0, sizeof(get_res));
    buckets_s3_get_object(&req, &get_res);
    cr_assert_eq(get_res.status_code, 404, "GET after DELETE should return 404");
    
    if (get_res.body) buckets_free(get_res.body);
}

Test(s3_ops, head_object)
{
    /* Create object */
    buckets_s3_request_t req;
    memset(&req, 0, sizeof(req));
    strcpy(req.bucket, "testbucket");
    strcpy(req.key, "headtest");
    req.body = "head test data";
    req.body_len = strlen(req.body);
    
    buckets_s3_response_t put_res;
    memset(&put_res, 0, sizeof(put_res));
    buckets_s3_put_object(&req, &put_res);
    if (put_res.body) buckets_free(put_res.body);
    
    /* HEAD object */
    buckets_s3_response_t head_res;
    memset(&head_res, 0, sizeof(head_res));
    
    int ret = buckets_s3_head_object(&req, &head_res);
    cr_assert_eq(ret, BUCKETS_OK, "HEAD should succeed");
    cr_assert_eq(head_res.status_code, 200, "HEAD should return 200");
    cr_assert(head_res.etag[0] != '\0', "HEAD should return ETag");
    cr_assert_null(head_res.body, "HEAD should not return body");
}
