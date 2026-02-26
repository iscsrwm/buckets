/**
 * S3 Multipart Upload Operations Tests
 */

#include <criterion/criterion.h>
#include <criterion/parameterized.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "buckets.h"
#include "buckets_s3.h"

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

static void setup_test_env(void)
{
    /* Create test bucket */
    int ret = system("mkdir -p /tmp/buckets-data/test-bucket");
    (void)ret;  /* Ignore return value - best effort */
}

static void teardown_test_env(void)
{
    /* Clean up test data */
    int ret = system("rm -rf /tmp/buckets-data/test-bucket");
    (void)ret;  /* Ignore return value - best effort */
}

TestSuite(s3_multipart, .init = setup_test_env, .fini = teardown_test_env);

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

static char* extract_upload_id(const char *xml_response)
{
    /* Simple XML parser to extract UploadId */
    const char *start = strstr(xml_response, "<UploadId>");
    if (!start) return NULL;
    start += strlen("<UploadId>");
    
    const char *end = strstr(start, "</UploadId>");
    if (!end) return NULL;
    
    size_t len = end - start;
    char *upload_id = malloc(len + 1);
    memcpy(upload_id, start, len);
    upload_id[len] = '\0';
    
    return upload_id;
}

static char* extract_etag(const char *xml_response) __attribute__((unused));
static char* extract_etag(const char *xml_response)
{
    /* Simple XML parser to extract ETag */
    const char *start = strstr(xml_response, "<ETag>");
    if (!start) return NULL;
    start += strlen("<ETag>");
    
    const char *end = strstr(start, "</ETag>");
    if (!end) return NULL;
    
    size_t len = end - start;
    char *etag = malloc(len + 1);
    memcpy(etag, start, len);
    etag[len] = '\0';
    
    return etag;
}

/* ===================================================================
 * InitiateMultipartUpload Tests
 * ===================================================================*/

Test(s3_multipart, initiate_basic)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    int ret = buckets_s3_initiate_multipart_upload(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "Initiate should succeed");
    cr_assert_eq(res.status_code, 200, "Should return 200");
    cr_assert_not_null(res.body, "Response should have body");
    cr_assert(strstr(res.body, "<UploadId>") != NULL, "Should contain UploadId");
    cr_assert(strstr(res.body, "test-bucket") != NULL, "Should contain bucket name");
    cr_assert(strstr(res.body, "test-object.dat") != NULL, "Should contain key");
    
    /* Extract upload ID and verify directory was created */
    char *upload_id = extract_upload_id(res.body);
    cr_assert_not_null(upload_id, "Should extract upload ID");
    
    char upload_dir[2048];
    snprintf(upload_dir, sizeof(upload_dir), 
             "/tmp/buckets-data/test-bucket/.multipart/%s", upload_id);
    
    struct stat st;
    cr_assert_eq(stat(upload_dir, &st), 0, "Upload directory should exist");
    cr_assert(S_ISDIR(st.st_mode), "Should be a directory");
    
    /* Verify metadata file exists */
    char metadata_path[2048];
    snprintf(metadata_path, sizeof(metadata_path), 
             "/tmp/buckets-data/test-bucket/.multipart/%s/metadata.json", upload_id);
    cr_assert_eq(stat(metadata_path, &st), 0, "Metadata file should exist");
    
    /* Verify parts directory exists */
    char parts_dir[2048];
    snprintf(parts_dir, sizeof(parts_dir), 
             "/tmp/buckets-data/test-bucket/.multipart/%s/parts", upload_id);
    cr_assert_eq(stat(parts_dir, &st), 0, "Parts directory should exist");
    
    free(upload_id);
    buckets_free(res.body);
}

Test(s3_multipart, initiate_missing_bucket)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "");
    strcpy(req.key, "test-object.dat");
    
    int ret = buckets_s3_initiate_multipart_upload(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "Should return OK with error response");
    cr_assert_eq(res.status_code, 400, "Should return 400");
    cr_assert(strstr(res.body, "InvalidRequest") != NULL, "Should contain error code");
    
    buckets_free(res.body);
}

Test(s3_multipart, initiate_nonexistent_bucket)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "nonexistent-bucket");
    strcpy(req.key, "test-object.dat");
    
    int ret = buckets_s3_initiate_multipart_upload(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "Should return OK with error response");
    cr_assert_eq(res.status_code, 404, "Should return 404");
    cr_assert(strstr(res.body, "NoSuchBucket") != NULL, "Should contain error code");
    
    buckets_free(res.body);
}

/* ===================================================================
 * UploadPart Tests
 * ===================================================================*/

Test(s3_multipart, upload_part_basic)
{
    /* First initiate upload */
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    buckets_s3_initiate_multipart_upload(&req, &res);
    char *upload_id = extract_upload_id(res.body);
    cr_assert_not_null(upload_id, "Should have upload ID");
    buckets_free(res.body);
    
    /* Upload a part */
    memset(&req, 0, sizeof(req));
    memset(&res, 0, sizeof(res));
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    const char *part_data = "This is part 1 data";
    req.body = part_data;
    req.body_len = strlen(part_data);
    
    /* Set query parameters */
    char *keys[2] = {"uploadId", "partNumber"};
    char *values[2] = {upload_id, "1"};
    req.query_params_keys = keys;
    req.query_params_values = values;
    req.query_count = 2;
    
    int ret = buckets_s3_upload_part(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "Upload part should succeed");
    cr_assert_eq(res.status_code, 200, "Should return 200");
    cr_assert_not_null(res.etag[0], "Should have ETag");
    
    /* Verify part file was created */
    char part_path[2048];
    snprintf(part_path, sizeof(part_path), 
             "/tmp/buckets-data/test-bucket/.multipart/%s/parts/part.1", upload_id);
    
    struct stat st;
    cr_assert_eq(stat(part_path, &st), 0, "Part file should exist");
    cr_assert_eq(st.st_size, strlen(part_data), "Part size should match");
    
    free(upload_id);
}

Test(s3_multipart, upload_part_missing_upload_id)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    req.body = "data";
    req.body_len = 4;
    
    /* No query parameters */
    req.query_count = 0;
    
    int ret = buckets_s3_upload_part(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "Should return OK with error response");
    cr_assert_eq(res.status_code, 400, "Should return 400");
    
    buckets_free(res.body);
}

Test(s3_multipart, upload_part_invalid_part_number)
{
    /* First initiate upload */
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    buckets_s3_initiate_multipart_upload(&req, &res);
    char *upload_id = extract_upload_id(res.body);
    buckets_free(res.body);
    
    /* Upload with invalid part number */
    memset(&req, 0, sizeof(req));
    memset(&res, 0, sizeof(res));
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    req.body = "data";
    req.body_len = 4;
    
    char *keys[2] = {"uploadId", "partNumber"};
    char *values[2] = {upload_id, "10001"};  /* Too high */
    req.query_params_keys = keys;
    req.query_params_values = values;
    req.query_count = 2;
    
    int ret = buckets_s3_upload_part(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "Should return OK with error response");
    cr_assert_eq(res.status_code, 400, "Should return 400");
    cr_assert(strstr(res.body, "InvalidPartNumber") != NULL, "Should contain error code");
    
    free(upload_id);
    buckets_free(res.body);
}

Test(s3_multipart, upload_multiple_parts)
{
    /* Initiate upload */
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    buckets_s3_initiate_multipart_upload(&req, &res);
    char *upload_id = extract_upload_id(res.body);
    buckets_free(res.body);
    
    /* Upload 3 parts */
    const char *part_data[] = {
        "Part 1 data content",
        "Part 2 data content",
        "Part 3 data content"
    };
    
    /* Static part number strings to avoid stack reuse issue */
    char *part_nums[] = {"1", "2", "3"};
    
    for (int i = 0; i < 3; i++) {
        memset(&req, 0, sizeof(req));
        memset(&res, 0, sizeof(res));
        
        strcpy(req.bucket, "test-bucket");
        strcpy(req.key, "test-object.dat");
        req.body = part_data[i];
        req.body_len = strlen(part_data[i]);
        
        char *keys[2] = {"uploadId", "partNumber"};
        char *values[2] = {upload_id, part_nums[i]};
        req.query_params_keys = keys;
        req.query_params_values = values;
        req.query_count = 2;
        
        int ret = buckets_s3_upload_part(&req, &res);
        cr_assert_eq(ret, BUCKETS_OK, "Upload part %d should succeed", i + 1);
        cr_assert_eq(res.status_code, 200, "Should return 200 for part %d", i + 1);
    }
    
    free(upload_id);
}

/* ===================================================================
 * ListParts Tests
 * ===================================================================*/

Test(s3_multipart, list_parts_basic)
{
    /* Initiate and upload parts */
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    buckets_s3_initiate_multipart_upload(&req, &res);
    char *upload_id = extract_upload_id(res.body);
    buckets_free(res.body);
    
    /* Upload 2 parts */
    const char *part_data[] = {"Part 1 data", "Part 2 data"};
    char *part_nums[] = {"1", "2"};
    
    for (int i = 0; i < 2; i++) {
        memset(&req, 0, sizeof(req));
        memset(&res, 0, sizeof(res));
        
        strcpy(req.bucket, "test-bucket");
        strcpy(req.key, "test-object.dat");
        
        req.body = part_data[i];
        req.body_len = strlen(part_data[i]);
        
        char *keys[2] = {"uploadId", "partNumber"};
        char *values[2] = {upload_id, part_nums[i]};
        req.query_params_keys = keys;
        req.query_params_values = values;
        req.query_count = 2;
        
        buckets_s3_upload_part(&req, &res);
    }
    
    /* List parts */
    memset(&req, 0, sizeof(req));
    memset(&res, 0, sizeof(res));
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    char *keys[1] = {"uploadId"};
    char *values[1] = {upload_id};
    req.query_params_keys = keys;
    req.query_params_values = values;
    req.query_count = 1;
    
    int ret = buckets_s3_list_parts(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "List parts should succeed");
    cr_assert_eq(res.status_code, 200, "Should return 200");
    cr_assert_not_null(res.body, "Should have response body");
    
    /* Verify response contains part information */
    cr_assert(strstr(res.body, "<PartNumber>1</PartNumber>") != NULL, "Should contain part 1");
    cr_assert(strstr(res.body, "<PartNumber>2</PartNumber>") != NULL, "Should contain part 2");
    cr_assert(strstr(res.body, "<ETag>") != NULL, "Should contain ETags");
    cr_assert(strstr(res.body, "<Size>") != NULL, "Should contain sizes");
    cr_assert(strstr(res.body, "<IsTruncated>false</IsTruncated>") != NULL, 
              "Should not be truncated");
    
    free(upload_id);
    buckets_free(res.body);
}

Test(s3_multipart, list_parts_nonexistent_upload)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    char *keys[1] = {"uploadId"};
    char *values[1] = {"nonexistent-upload-id"};
    req.query_params_keys = keys;
    req.query_params_values = values;
    req.query_count = 1;
    
    int ret = buckets_s3_list_parts(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "Should return OK with error response");
    cr_assert_eq(res.status_code, 404, "Should return 404");
    cr_assert(strstr(res.body, "NoSuchUpload") != NULL, "Should contain error code");
    
    buckets_free(res.body);
}

/* ===================================================================
 * AbortMultipartUpload Tests
 * ===================================================================*/

Test(s3_multipart, abort_basic)
{
    /* Initiate and upload parts */
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    buckets_s3_initiate_multipart_upload(&req, &res);
    char *upload_id = extract_upload_id(res.body);
    buckets_free(res.body);
    
    /* Upload a part */
    memset(&req, 0, sizeof(req));
    memset(&res, 0, sizeof(res));
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    req.body = "test data";
    req.body_len = 9;
    
    char *keys[2] = {"uploadId", "partNumber"};
    char *values[2] = {upload_id, "1"};
    req.query_params_keys = keys;
    req.query_params_values = values;
    req.query_count = 2;
    
    buckets_s3_upload_part(&req, &res);
    
    /* Abort upload */
    memset(&req, 0, sizeof(req));
    memset(&res, 0, sizeof(res));
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    char *abort_keys[1] = {"uploadId"};
    char *abort_values[1] = {upload_id};
    req.query_params_keys = abort_keys;
    req.query_params_values = abort_values;
    req.query_count = 1;
    
    int ret = buckets_s3_abort_multipart_upload(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "Abort should succeed");
    cr_assert_eq(res.status_code, 204, "Should return 204");
    cr_assert_null(res.body, "Should have no body");
    
    /* Verify upload directory was removed */
    char upload_dir[2048];
    snprintf(upload_dir, sizeof(upload_dir), 
             "/tmp/buckets-data/test-bucket/.multipart/%s", upload_id);
    
    struct stat st;
    cr_assert_neq(stat(upload_dir, &st), 0, "Upload directory should not exist");
    
    free(upload_id);
}

Test(s3_multipart, abort_nonexistent_upload)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    char *keys[1] = {"uploadId"};
    char *values[1] = {"nonexistent-upload-id"};
    req.query_params_keys = keys;
    req.query_params_values = values;
    req.query_count = 1;
    
    int ret = buckets_s3_abort_multipart_upload(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "Should return OK with error response");
    cr_assert_eq(res.status_code, 404, "Should return 404");
    cr_assert(strstr(res.body, "NoSuchUpload") != NULL, "Should contain error code");
    
    buckets_free(res.body);
}

/* ===================================================================
 * CompleteMultipartUpload Tests
 * ===================================================================*/

Test(s3_multipart, complete_basic)
{
    /* Initiate and upload parts */
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    buckets_s3_initiate_multipart_upload(&req, &res);
    char *upload_id = extract_upload_id(res.body);
    buckets_free(res.body);
    
    /* Upload 2 parts */
    const char *part_data[] = {
        "First part content",
        "Second part content"
    };
    
    for (int i = 0; i < 2; i++) {
        memset(&req, 0, sizeof(req));
        memset(&res, 0, sizeof(res));
        
        strcpy(req.bucket, "test-bucket");
        strcpy(req.key, "test-object.dat");
        req.body = part_data[i];
        req.body_len = strlen(part_data[i]);
        
        char part_num_str[16];
        snprintf(part_num_str, sizeof(part_num_str), "%d", i + 1);
        
        char *keys[2] = {"uploadId", "partNumber"};
        char *values[2] = {upload_id, part_num_str};
        req.query_params_keys = keys;
        req.query_params_values = values;
        req.query_count = 2;
        
        buckets_s3_upload_part(&req, &res);
    }
    
    /* Complete upload */
    memset(&req, 0, sizeof(req));
    memset(&res, 0, sizeof(res));
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    const char *complete_xml = 
        "<CompleteMultipartUpload>"
        "<Part><PartNumber>1</PartNumber></Part>"
        "<Part><PartNumber>2</PartNumber></Part>"
        "</CompleteMultipartUpload>";
    req.body = complete_xml;
    req.body_len = strlen(complete_xml);
    
    char *keys[1] = {"uploadId"};
    char *values[1] = {upload_id};
    req.query_params_keys = keys;
    req.query_params_values = values;
    req.query_count = 1;
    
    int ret = buckets_s3_complete_multipart_upload(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "Complete should succeed");
    cr_assert_eq(res.status_code, 200, "Should return 200");
    cr_assert_not_null(res.body, "Should have response body");
    
    /* Verify response */
    cr_assert(strstr(res.body, "<Bucket>test-bucket</Bucket>") != NULL, 
              "Should contain bucket");
    cr_assert(strstr(res.body, "<Key>test-object.dat</Key>") != NULL, 
              "Should contain key");
    cr_assert(strstr(res.body, "<ETag>") != NULL, "Should contain ETag");
    
    /* Verify multipart ETag format (MD5-partcount) */
    cr_assert(strstr(res.etag, "-2") != NULL, "ETag should include part count");
    
    /* Verify upload directory was removed */
    char upload_dir[2048];
    struct stat st;
    snprintf(upload_dir, sizeof(upload_dir), 
             "/tmp/buckets-data/test-bucket/.multipart/%s", upload_id);
    cr_assert_neq(stat(upload_dir, &st), 0, "Upload directory should be removed");
    
    free(upload_id);
    buckets_free(res.body);
}

Test(s3_multipart, complete_no_parts)
{
    /* Initiate upload but don't upload any parts */
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    buckets_s3_initiate_multipart_upload(&req, &res);
    char *upload_id = extract_upload_id(res.body);
    buckets_free(res.body);
    
    /* Try to complete without parts */
    memset(&req, 0, sizeof(req));
    memset(&res, 0, sizeof(res));
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    const char *complete_xml = "<CompleteMultipartUpload></CompleteMultipartUpload>";
    req.body = complete_xml;
    req.body_len = strlen(complete_xml);
    
    char *keys[1] = {"uploadId"};
    char *values[1] = {upload_id};
    req.query_params_keys = keys;
    req.query_params_values = values;
    req.query_count = 1;
    
    int ret = buckets_s3_complete_multipart_upload(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "Should return OK with error response");
    cr_assert_eq(res.status_code, 400, "Should return 400");
    cr_assert(strstr(res.body, "InvalidPart") != NULL, "Should contain error code");
    
    free(upload_id);
    buckets_free(res.body);
}

Test(s3_multipart, complete_nonexistent_upload)
{
    buckets_s3_request_t req = {0};
    buckets_s3_response_t res = {0};
    
    strcpy(req.bucket, "test-bucket");
    strcpy(req.key, "test-object.dat");
    
    const char *complete_xml = "<CompleteMultipartUpload></CompleteMultipartUpload>";
    req.body = complete_xml;
    req.body_len = strlen(complete_xml);
    
    char *keys[1] = {"uploadId"};
    char *values[1] = {"nonexistent-upload-id"};
    req.query_params_keys = keys;
    req.query_params_values = values;
    req.query_count = 1;
    
    int ret = buckets_s3_complete_multipart_upload(&req, &res);
    
    cr_assert_eq(ret, BUCKETS_OK, "Should return OK with error response");
    cr_assert_eq(res.status_code, 404, "Should return 404");
    cr_assert(strstr(res.body, "NoSuchUpload") != NULL, "Should contain error code");
    
    buckets_free(res.body);
}
