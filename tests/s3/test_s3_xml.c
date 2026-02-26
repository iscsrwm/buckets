/**
 * S3 XML Generation Tests
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

void setup_s3_xml(void)
{
    buckets_init();
}

void teardown_s3_xml(void)
{
    buckets_cleanup();
}

TestSuite(s3_xml, .init = setup_s3_xml, .fini = teardown_s3_xml);

/* ===================================================================
 * XML Generation Tests
 * ===================================================================*/

Test(s3_xml, success_response)
{
    buckets_s3_response_t res;
    memset(&res, 0, sizeof(res));
    
    strcpy(res.etag, "abc123def456");
    strcpy(res.version_id, "version-1");
    
    int ret = buckets_s3_xml_success(&res, "PutObjectResult");
    cr_assert_eq(ret, BUCKETS_OK, "XML success generation should succeed");
    cr_assert_eq(res.status_code, 200, "Status should be 200");
    cr_assert_not_null(res.body, "Body should not be NULL");
    
    /* Check XML content */
    cr_assert(strstr(res.body, "<?xml"), "Should have XML declaration");
    cr_assert(strstr(res.body, "<PutObjectResult"), "Should have root element");
    cr_assert(strstr(res.body, "<ETag>"), "Should have ETag element");
    cr_assert(strstr(res.body, "abc123def456"), "Should have ETag value");
    cr_assert(strstr(res.body, "<VersionId>"), "Should have VersionId element");
    cr_assert(strstr(res.body, "version-1"), "Should have VersionId value");
    
    buckets_free(res.body);
}

Test(s3_xml, error_response_404)
{
    buckets_s3_response_t res;
    memset(&res, 0, sizeof(res));
    
    int ret = buckets_s3_xml_error(&res, "NoSuchKey",
                                    "The specified key does not exist",
                                    "/mybucket/myobject");
    cr_assert_eq(ret, BUCKETS_OK, "XML error generation should succeed");
    cr_assert_eq(res.status_code, 404, "Status should be 404");
    cr_assert_not_null(res.body, "Body should not be NULL");
    
    /* Check XML content */
    cr_assert(strstr(res.body, "<Error>"), "Should have Error element");
    cr_assert(strstr(res.body, "<Code>NoSuchKey</Code>"), "Should have error code");
    cr_assert(strstr(res.body, "The specified key does not exist"),
              "Should have error message");
    cr_assert(strstr(res.body, "<Resource>"), "Should have resource element");
    cr_assert(strstr(res.body, "/mybucket/myobject"), "Should have resource path");
    
    /* Check error_code field */
    cr_assert_str_eq(res.error_code, "NoSuchKey", "error_code should be set");
    
    buckets_free(res.body);
}

Test(s3_xml, error_response_403)
{
    buckets_s3_response_t res;
    memset(&res, 0, sizeof(res));
    
    int ret = buckets_s3_xml_error(&res, "AccessDenied",
                                    "Access denied",
                                    "/mybucket");
    cr_assert_eq(ret, BUCKETS_OK, "XML error generation should succeed");
    cr_assert_eq(res.status_code, 403, "Status should be 403 for AccessDenied");
    cr_assert(strstr(res.body, "<Code>AccessDenied</Code>"),
              "Should have AccessDenied code");
    
    buckets_free(res.body);
}

Test(s3_xml, xml_escaping)
{
    buckets_s3_response_t res;
    memset(&res, 0, sizeof(res));
    
    /* Error message with special characters */
    int ret = buckets_s3_xml_error(&res, "InvalidRequest",
                                    "Invalid request: <test> & \"quoted\"",
                                    "/bucket");
    cr_assert_eq(ret, BUCKETS_OK, "Should succeed with special chars");
    
    /* Check that special characters are escaped */
    cr_assert(strstr(res.body, "&lt;test&gt;"), "< and > should be escaped");
    cr_assert(strstr(res.body, "&amp;"), "& should be escaped");
    cr_assert(strstr(res.body, "&quot;"), "\" should be escaped");
    
    buckets_free(res.body);
}

Test(s3_xml, null_inputs)
{
    buckets_s3_response_t res;
    memset(&res, 0, sizeof(res));
    
    /* NULL response */
    int ret = buckets_s3_xml_success(NULL, "Test");
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with NULL response");
    
    /* NULL root element */
    ret = buckets_s3_xml_success(&res, NULL);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with NULL root element");
    
    /* NULL error code */
    ret = buckets_s3_xml_error(&res, NULL, "message", "/resource");
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with NULL error code");
}
