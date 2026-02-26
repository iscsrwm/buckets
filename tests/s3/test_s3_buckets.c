/**
 * S3 Bucket Operations Tests
 * 
 * Tests for PUT/DELETE/HEAD bucket and LIST buckets operations.
 */

#include <criterion/criterion.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_s3.h"

/* Setup/teardown */
void setup_s3_buckets(void)
{
    /* Clean up any previous test data */
    int ret = system("rm -rf /tmp/buckets-data");
    (void)ret;
    
    /* Create fresh test data directory */
    ret = system("mkdir -p /tmp/buckets-data");
    (void)ret;
    
    buckets_init();
}

void teardown_s3_buckets(void)
{
    buckets_cleanup();
    
    /* Cleanup test data */
    int ret = system("rm -rf /tmp/buckets-data");
    (void)ret; /* Ignore return value */
}

TestSuite(s3_buckets, .init = setup_s3_buckets, .fini = teardown_s3_buckets);

/* ===================================================================
 * PUT Bucket Tests
 * ===================================================================*/

Test(s3_buckets, put_bucket_success)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    
    int ret = buckets_s3_put_bucket(&req, &res);
    cr_assert_eq(ret, BUCKETS_OK, "PUT bucket should succeed");
    cr_assert_eq(res.status_code, 200, "Status should be 200 OK");
    
    /* Verify bucket directory was created */
    struct stat st;
    cr_assert_eq(stat("/tmp/buckets-data/test-bucket", &st), 0,
                 "Bucket directory should exist");
    cr_assert(S_ISDIR(st.st_mode), "Should be a directory");
}

Test(s3_buckets, put_bucket_already_exists)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res1 = {0};
    buckets_s3_response_t res2 = {0};
    
    strcpy(req.bucket, "existing-bucket");
    
    /* Create bucket first time */
    int ret = buckets_s3_put_bucket(&req, &res1);
    cr_assert_eq(ret, BUCKETS_OK, "First PUT should succeed");
    cr_assert_eq(res1.status_code, 200);
    
    /* Try to create again - should return 409 Conflict */
    ret = buckets_s3_put_bucket(&req, &res2);
    cr_assert_eq(ret, BUCKETS_OK, "PUT should not fail");
    cr_assert_eq(res2.status_code, 409, "Status should be 409 Conflict");
    cr_assert_not_null(res2.body, "Should have error body");
    cr_assert(strstr(res2.body, "BucketAlreadyOwnedByYou") != NULL,
              "Error should be BucketAlreadyOwnedByYou");
    
    buckets_free(res2.body);
}

Test(s3_buckets, put_bucket_invalid_name)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    /* Invalid bucket name (too short) */
    strcpy(req.bucket, "ab");
    
    int ret = buckets_s3_put_bucket(&req, &res);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_eq(res.status_code, 400, "Status should be 400 Bad Request");
    cr_assert_not_null(res.body);
    cr_assert(strstr(res.body, "InvalidBucketName") != NULL);
    
    buckets_free(res.body);
}

/* ===================================================================
 * DELETE Bucket Tests
 * ===================================================================*/

Test(s3_buckets, delete_bucket_success)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res_put = {0};
    buckets_s3_response_t res_del = {0};
    
    strcpy(req.bucket, "bucket-to-delete");
    
    /* Create bucket first */
    buckets_s3_put_bucket(&req, &res_put);
    cr_assert_eq(res_put.status_code, 200);
    
    /* Delete it */
    int ret = buckets_s3_delete_bucket(&req, &res_del);
    cr_assert_eq(ret, BUCKETS_OK, "DELETE should succeed");
    cr_assert_eq(res_del.status_code, 204, "Status should be 204 No Content");
    
    /* Verify bucket directory was removed */
    struct stat st;
    cr_assert_neq(stat("/tmp/buckets-data/bucket-to-delete", &st), 0,
                  "Bucket directory should not exist");
}

Test(s3_buckets, delete_bucket_not_found)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "nonexistent-bucket");
    
    int ret = buckets_s3_delete_bucket(&req, &res);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_eq(res.status_code, 404, "Status should be 404 Not Found");
    cr_assert_not_null(res.body);
    cr_assert(strstr(res.body, "NoSuchBucket") != NULL);
    
    buckets_free(res.body);
}

Test(s3_buckets, delete_bucket_not_empty)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res_put_bucket = {0};
    buckets_s3_response_t res_put_object = {0};
    buckets_s3_response_t res_del = {0};
    
    strcpy(req.bucket, "nonempty-bucket");
    
    /* Create bucket */
    buckets_s3_put_bucket(&req, &res_put_bucket);
    cr_assert_eq(res_put_bucket.status_code, 200);
    
    /* Put an object in it */
    strcpy(req.key, "test-object");
    req.body = "test data";
    req.body_len = 9;
    buckets_s3_put_object(&req, &res_put_object);
    cr_assert_eq(res_put_object.status_code, 200);
    buckets_free(res_put_object.body);
    
    /* Try to delete bucket - should fail */
    req.key[0] = '\0'; /* Clear key */
    int ret = buckets_s3_delete_bucket(&req, &res_del);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_eq(res_del.status_code, 409, "Status should be 409 Conflict");
    cr_assert_not_null(res_del.body);
    cr_assert(strstr(res_del.body, "BucketNotEmpty") != NULL);
    
    buckets_free(res_del.body);
}

/* ===================================================================
 * HEAD Bucket Tests
 * ===================================================================*/

Test(s3_buckets, head_bucket_exists)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res_put = {0};
    buckets_s3_response_t res_head = {0};
    
    strcpy(req.bucket, "bucket-exists");
    
    /* Create bucket */
    buckets_s3_put_bucket(&req, &res_put);
    cr_assert_eq(res_put.status_code, 200);
    
    /* HEAD it */
    int ret = buckets_s3_head_bucket(&req, &res_head);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_eq(res_head.status_code, 200, "Status should be 200 OK");
    cr_assert_null(res_head.body, "HEAD should not have body");
}

Test(s3_buckets, head_bucket_not_found)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "bucket-not-found");
    
    int ret = buckets_s3_head_bucket(&req, &res);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_eq(res.status_code, 404, "Status should be 404 Not Found");
    cr_assert_not_null(res.body);
    cr_assert(strstr(res.body, "NoSuchBucket") != NULL);
    
    buckets_free(res.body);
}

/* ===================================================================
 * LIST Buckets Tests
 * ===================================================================*/

Test(s3_buckets, list_buckets_empty)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    /* No bucket name for LIST buckets */
    req.bucket[0] = '\0';
    
    int ret = buckets_s3_list_buckets(&req, &res);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_eq(res.status_code, 200, "Status should be 200 OK");
    cr_assert_not_null(res.body, "Should have XML body");
    cr_assert(strstr(res.body, "ListAllMyBucketsResult") != NULL);
    cr_assert(strstr(res.body, "<Buckets>") != NULL);
    
    buckets_free(res.body);
}

Test(s3_buckets, list_buckets_with_buckets)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res_put1 = {0};
    buckets_s3_response_t res_put2 = {0};
    buckets_s3_response_t res_list = {0};
    
    /* Create two buckets */
    strcpy(req.bucket, "bucket-one");
    buckets_s3_put_bucket(&req, &res_put1);
    cr_assert_eq(res_put1.status_code, 200);
    
    strcpy(req.bucket, "bucket-two");
    buckets_s3_put_bucket(&req, &res_put2);
    cr_assert_eq(res_put2.status_code, 200);
    
    /* LIST buckets */
    req.bucket[0] = '\0';
    int ret = buckets_s3_list_buckets(&req, &res_list);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_eq(res_list.status_code, 200);
    cr_assert_not_null(res_list.body);
    
    /* Verify both buckets are in the list */
    cr_assert(strstr(res_list.body, "bucket-one") != NULL,
              "bucket-one should be in list");
    cr_assert(strstr(res_list.body, "bucket-two") != NULL,
              "bucket-two should be in list");
    cr_assert(strstr(res_list.body, "<CreationDate>") != NULL,
              "Should have creation dates");
    
    buckets_free(res_list.body);
}
