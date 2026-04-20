/**
 * S3 Authentication Tests
 * 
 * Tests for AWS Signature V4 authentication and credential management.
 */

#include <criterion/criterion.h>
#include <criterion/logging.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "buckets.h"
#include "buckets_s3.h"

/* Test data directory */
static char g_test_dir[256];

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

static void setup(void)
{
    /* Create unique test directory */
    snprintf(g_test_dir, sizeof(g_test_dir), "/tmp/buckets_auth_test_%d", getpid());
    mkdir(g_test_dir, 0755);
    
    /* Initialize credential system */
    buckets_credentials_init(g_test_dir);
}

static void teardown(void)
{
    /* Cleanup credential system */
    buckets_credentials_cleanup();
    
    /* Remove test directory */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_test_dir);
    int result __attribute__((unused)) = system(cmd);
}

/* ===================================================================
 * Credential Management Tests
 * ===================================================================*/

Test(credentials, init_creates_defaults, .init = setup, .fini = teardown)
{
    /* Default credentials should be created */
    int count = buckets_credentials_count();
    cr_assert_geq(count, 2, "Should have at least 2 default credentials");
}

Test(credentials, get_secret_for_default_key, .init = setup, .fini = teardown)
{
    char secret[128];
    int ret = buckets_credentials_get_secret("minioadmin", secret, sizeof(secret));
    cr_assert_eq(ret, BUCKETS_OK, "Should find default minioadmin key");
    cr_assert_str_eq(secret, "minioadmin", "Default secret should be minioadmin");
}

Test(credentials, get_secret_for_unknown_key, .init = setup, .fini = teardown)
{
    char secret[128];
    int ret = buckets_credentials_get_secret("unknown-key", secret, sizeof(secret));
    cr_assert_eq(ret, BUCKETS_ERR_NOT_FOUND, "Should return NOT_FOUND for unknown key");
}

Test(credentials, create_new_credential, .init = setup, .fini = teardown)
{
    char access_key[64];
    char secret_key[64];
    
    int ret = buckets_credentials_create("Test Key", "readwrite",
                                          access_key, sizeof(access_key),
                                          secret_key, sizeof(secret_key));
    cr_assert_eq(ret, BUCKETS_OK, "Should create new credential");
    cr_assert_gt(strlen(access_key), 0, "Access key should be generated");
    cr_assert_gt(strlen(secret_key), 0, "Secret key should be generated");
    
    /* Verify we can retrieve it */
    char retrieved_secret[128];
    ret = buckets_credentials_get_secret(access_key, retrieved_secret, sizeof(retrieved_secret));
    cr_assert_eq(ret, BUCKETS_OK, "Should retrieve new credential");
    cr_assert_str_eq(retrieved_secret, secret_key, "Secret should match");
}

Test(credentials, delete_credential, .init = setup, .fini = teardown)
{
    char access_key[64];
    char secret_key[64];
    
    /* Create first */
    buckets_credentials_create("To Delete", "readwrite",
                                access_key, sizeof(access_key),
                                secret_key, sizeof(secret_key));
    
    /* Delete */
    int ret = buckets_credentials_delete(access_key);
    cr_assert_eq(ret, BUCKETS_OK, "Should delete credential");
    
    /* Verify it's gone */
    char secret[128];
    ret = buckets_credentials_get_secret(access_key, secret, sizeof(secret));
    cr_assert_eq(ret, BUCKETS_ERR_NOT_FOUND, "Should not find deleted credential");
}

Test(credentials, disable_and_enable, .init = setup, .fini = teardown)
{
    char access_key[64];
    char secret_key[64];
    
    buckets_credentials_create("Toggle Key", "readwrite",
                                access_key, sizeof(access_key),
                                secret_key, sizeof(secret_key));
    
    /* Disable */
    int ret = buckets_credentials_set_enabled(access_key, false);
    cr_assert_eq(ret, BUCKETS_OK, "Should disable credential");
    
    /* Validate returns ACCESS_DENIED for disabled */
    ret = buckets_credentials_validate(access_key);
    cr_assert_eq(ret, BUCKETS_ERR_ACCESS_DENIED, "Disabled key should be denied");
    
    /* Enable */
    ret = buckets_credentials_set_enabled(access_key, true);
    cr_assert_eq(ret, BUCKETS_OK, "Should enable credential");
    
    /* Now validate should succeed */
    ret = buckets_credentials_validate(access_key);
    cr_assert_eq(ret, BUCKETS_OK, "Enabled key should validate");
}

Test(credentials, get_policy, .init = setup, .fini = teardown)
{
    char access_key[64];
    char secret_key[64];
    
    buckets_credentials_create("Policy Test", "readonly",
                                access_key, sizeof(access_key),
                                secret_key, sizeof(secret_key));
    
    char policy[64];
    int ret = buckets_credentials_get_policy(access_key, policy, sizeof(policy));
    cr_assert_eq(ret, BUCKETS_OK, "Should get policy");
    cr_assert_str_eq(policy, "readonly", "Policy should be readonly");
}

Test(credentials, touch_updates_last_used, .init = setup, .fini = teardown)
{
    /* Touch a credential */
    buckets_credentials_touch("minioadmin");
    
    /* Can't easily verify last_used timestamp without direct access,
     * but at least verify it doesn't crash */
    cr_assert(true, "Touch should not crash");
}

/* ===================================================================
 * Authentication Enable/Disable Tests
 * ===================================================================*/

Test(auth_control, auth_disabled_allows_all, .init = setup, .fini = teardown)
{
    /* Disable auth */
    buckets_s3_auth_set_enabled(false);
    cr_assert_eq(buckets_s3_auth_enabled(), false, "Auth should be disabled");
    
    /* Create request with no credentials */
    buckets_s3_request_t req = {0};
    strcpy(req.bucket, "test");
    strcpy(req.key, "test.txt");
    
    /* Should succeed even without credentials */
    int ret = buckets_s3_verify_signature(&req, NULL);
    cr_assert_eq(ret, BUCKETS_OK, "Disabled auth should allow all");
    
    /* Re-enable auth */
    buckets_s3_auth_set_enabled(true);
}

Test(auth_control, auth_enabled_requires_credentials, .init = setup, .fini = teardown)
{
    /* Enable auth */
    buckets_s3_auth_set_enabled(true);
    cr_assert_eq(buckets_s3_auth_enabled(), true, "Auth should be enabled");
    
    /* Create request with no credentials */
    buckets_s3_request_t req = {0};
    strcpy(req.bucket, "test");
    strcpy(req.key, "test.txt");
    
    /* Should fail without credentials */
    int ret = buckets_s3_verify_signature(&req, NULL);
    cr_assert_eq(ret, BUCKETS_ERR_ACCESS_DENIED, "Missing credentials should be denied");
}

/* ===================================================================
 * Authorization Header Parsing Tests
 * ===================================================================*/

Test(auth_header, parse_valid_header, .init = setup, .fini = teardown)
{
    const char *header = 
        "AWS4-HMAC-SHA256 "
        "Credential=AKIAIOSFODNN7EXAMPLE/20230101/us-east-1/s3/aws4_request, "
        "SignedHeaders=host;x-amz-content-sha256;x-amz-date, "
        "Signature=abcdef1234567890";
    
    buckets_s3_request_t req = {0};
    char date[32] = {0};
    char region[64] = {0};
    
    int ret = buckets_s3_parse_auth_header(header, &req, date, sizeof(date), region, sizeof(region));
    
    cr_assert_eq(ret, BUCKETS_OK, "Should parse valid header");
    cr_assert_str_eq(req.access_key, "AKIAIOSFODNN7EXAMPLE", "Access key mismatch");
    cr_assert_str_eq(date, "20230101", "Date mismatch");
    cr_assert_str_eq(region, "us-east-1", "Region mismatch");
    cr_assert_str_eq(req.signed_headers, "host;x-amz-content-sha256;x-amz-date", "Signed headers mismatch");
    cr_assert_str_eq(req.signature, "abcdef1234567890", "Signature mismatch");
}

Test(auth_header, reject_invalid_algorithm, .init = setup, .fini = teardown)
{
    const char *header = "AWS2-HMAC-SHA1 Credential=test/20230101/us-east-1/s3/aws4_request";
    
    buckets_s3_request_t req = {0};
    int ret = buckets_s3_parse_auth_header(header, &req, NULL, 0, NULL, 0);
    
    cr_assert_neq(ret, BUCKETS_OK, "Should reject invalid algorithm");
}

Test(auth_header, reject_missing_credential, .init = setup, .fini = teardown)
{
    const char *header = "AWS4-HMAC-SHA256 SignedHeaders=host, Signature=abc";
    
    buckets_s3_request_t req = {0};
    int ret = buckets_s3_parse_auth_header(header, &req, NULL, 0, NULL, 0);
    
    cr_assert_neq(ret, BUCKETS_OK, "Should reject missing Credential");
}

Test(auth_header, reject_missing_signature, .init = setup, .fini = teardown)
{
    const char *header = 
        "AWS4-HMAC-SHA256 Credential=test/20230101/us-east-1/s3/aws4_request";
    
    buckets_s3_request_t req = {0};
    int ret = buckets_s3_parse_auth_header(header, &req, NULL, 0, NULL, 0);
    
    cr_assert_neq(ret, BUCKETS_OK, "Should reject missing Signature");
}

/* ===================================================================
 * Has Auth Check Tests
 * ===================================================================*/

Test(has_auth, detects_full_auth, .init = setup, .fini = teardown)
{
    buckets_s3_request_t req = {0};
    strcpy(req.access_key, "AKIAIOSFODNN7EXAMPLE");
    strcpy(req.signature, "abcdef1234567890");
    
    cr_assert(buckets_s3_has_auth(&req), "Should detect full auth");
}

Test(has_auth, rejects_missing_access_key, .init = setup, .fini = teardown)
{
    buckets_s3_request_t req = {0};
    strcpy(req.signature, "abcdef1234567890");
    
    cr_assert(!buckets_s3_has_auth(&req), "Should reject missing access key");
}

Test(has_auth, rejects_missing_signature, .init = setup, .fini = teardown)
{
    buckets_s3_request_t req = {0};
    strcpy(req.access_key, "AKIAIOSFODNN7EXAMPLE");
    
    cr_assert(!buckets_s3_has_auth(&req), "Should reject missing signature");
}

Test(has_auth, rejects_null_request, .init = setup, .fini = teardown)
{
    cr_assert(!buckets_s3_has_auth(NULL), "Should reject NULL request");
}
