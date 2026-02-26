/**
 * RPC Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <criterion/criterion.h>

#include "buckets.h"
#include "buckets_net.h"
#include "cJSON.h"

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

static buckets_conn_pool_t *pool = NULL;
static buckets_rpc_context_t *ctx = NULL;

void setup_rpc(void)
{
    buckets_init();
    pool = buckets_conn_pool_create(10);
    ctx = buckets_rpc_context_create(pool);
}

void teardown_rpc(void)
{
    if (ctx) {
        buckets_rpc_context_free(ctx);
    }
    if (pool) {
        buckets_conn_pool_free(pool);
    }
    buckets_cleanup();
}

TestSuite(rpc, .init = setup_rpc, .fini = teardown_rpc);

/* ===================================================================
 * RPC Context Tests
 * ===================================================================*/

Test(rpc, context_create)
{
    cr_assert_not_null(ctx, "RPC context should be created");
}

Test(rpc, context_create_null_pool)
{
    buckets_rpc_context_t *bad_ctx = buckets_rpc_context_create(NULL);
    cr_assert_null(bad_ctx, "RPC context should fail with NULL pool");
}

/* ===================================================================
 * RPC Request Tests
 * ===================================================================*/

Test(rpc, request_serialize_parse)
{
    /* Create request */
    buckets_rpc_request_t request;
    memset(&request, 0, sizeof(request));
    
    strcpy(request.id, "test-request-id-123");
    strcpy(request.method, "test.method");
    request.timestamp = 1708896000;
    request.params = cJSON_CreateObject();
    cJSON_AddStringToObject(request.params, "key1", "value1");
    cJSON_AddNumberToObject(request.params, "key2", 42);
    
    /* Serialize */
    char *json = NULL;
    int ret = buckets_rpc_request_serialize(&request, &json);
    cr_assert_eq(ret, BUCKETS_OK, "Request serialize should succeed");
    cr_assert_not_null(json, "JSON should not be NULL");
    
    /* Parse */
    buckets_rpc_request_t *parsed = NULL;
    ret = buckets_rpc_request_parse(json, &parsed);
    cr_assert_eq(ret, BUCKETS_OK, "Request parse should succeed");
    cr_assert_not_null(parsed, "Parsed request should not be NULL");
    
    /* Verify fields */
    cr_assert_str_eq(parsed->id, "test-request-id-123", "ID should match");
    cr_assert_str_eq(parsed->method, "test.method", "Method should match");
    cr_assert_eq(parsed->timestamp, 1708896000, "Timestamp should match");
    
    /* Verify params */
    cr_assert_not_null(parsed->params, "Params should not be NULL");
    cJSON *key1 = cJSON_GetObjectItem(parsed->params, "key1");
    cr_assert_not_null(key1, "key1 should exist");
    cr_assert_str_eq(key1->valuestring, "value1", "key1 value should match");
    
    cJSON *key2 = cJSON_GetObjectItem(parsed->params, "key2");
    cr_assert_not_null(key2, "key2 should exist");
    cr_assert_eq(key2->valueint, 42, "key2 value should match");
    
    /* Cleanup */
    cJSON_Delete(request.params);
    buckets_free(json);
    buckets_rpc_request_free(parsed);
}

Test(rpc, request_parse_invalid_json)
{
    buckets_rpc_request_t *request = NULL;
    int ret = buckets_rpc_request_parse("invalid json", &request);
    cr_assert_neq(ret, BUCKETS_OK, "Parse should fail with invalid JSON");
    cr_assert_null(request, "Request should be NULL on error");
}

Test(rpc, request_parse_missing_fields)
{
    /* Missing method field */
    const char *json = "{\"id\":\"123\",\"timestamp\":1708896000}";
    buckets_rpc_request_t *request = NULL;
    int ret = buckets_rpc_request_parse(json, &request);
    cr_assert_neq(ret, BUCKETS_OK, "Parse should fail with missing fields");
    cr_assert_null(request, "Request should be NULL on error");
}

Test(rpc, request_without_params)
{
    /* Create request without params */
    buckets_rpc_request_t request;
    memset(&request, 0, sizeof(request));
    
    strcpy(request.id, "test-id");
    strcpy(request.method, "test.method");
    request.timestamp = 1708896000;
    request.params = NULL;
    
    /* Serialize */
    char *json = NULL;
    int ret = buckets_rpc_request_serialize(&request, &json);
    cr_assert_eq(ret, BUCKETS_OK, "Serialize should succeed without params");
    
    /* Parse */
    buckets_rpc_request_t *parsed = NULL;
    ret = buckets_rpc_request_parse(json, &parsed);
    cr_assert_eq(ret, BUCKETS_OK, "Parse should succeed");
    cr_assert_null(parsed->params, "Params should be NULL");
    
    /* Cleanup */
    buckets_free(json);
    buckets_rpc_request_free(parsed);
}

/* ===================================================================
 * RPC Response Tests
 * ===================================================================*/

Test(rpc, response_serialize_parse_success)
{
    /* Create success response */
    buckets_rpc_response_t response;
    memset(&response, 0, sizeof(response));
    
    strcpy(response.id, "test-response-id");
    response.timestamp = 1708896001;
    response.error_code = 0;
    response.error_message[0] = '\0';
    response.result = cJSON_CreateObject();
    cJSON_AddStringToObject(response.result, "status", "ok");
    cJSON_AddNumberToObject(response.result, "count", 10);
    
    /* Serialize */
    char *json = NULL;
    int ret = buckets_rpc_response_serialize(&response, &json);
    cr_assert_eq(ret, BUCKETS_OK, "Response serialize should succeed");
    cr_assert_not_null(json, "JSON should not be NULL");
    
    /* Parse */
    buckets_rpc_response_t *parsed = NULL;
    ret = buckets_rpc_response_parse(json, &parsed);
    cr_assert_eq(ret, BUCKETS_OK, "Response parse should succeed");
    cr_assert_not_null(parsed, "Parsed response should not be NULL");
    
    /* Verify fields */
    cr_assert_str_eq(parsed->id, "test-response-id", "ID should match");
    cr_assert_eq(parsed->timestamp, 1708896001, "Timestamp should match");
    cr_assert_eq(parsed->error_code, 0, "Error code should be 0");
    cr_assert_str_eq(parsed->error_message, "", "Error message should be empty");
    
    /* Verify result */
    cr_assert_not_null(parsed->result, "Result should not be NULL");
    cJSON *status = cJSON_GetObjectItem(parsed->result, "status");
    cr_assert_not_null(status, "status should exist");
    cr_assert_str_eq(status->valuestring, "ok", "status value should match");
    
    /* Cleanup */
    cJSON_Delete(response.result);
    buckets_free(json);
    buckets_rpc_response_free(parsed);
}

Test(rpc, response_serialize_parse_error)
{
    /* Create error response */
    buckets_rpc_response_t response;
    memset(&response, 0, sizeof(response));
    
    strcpy(response.id, "test-error-id");
    response.timestamp = 1708896001;
    response.error_code = BUCKETS_ERR_INVALID_ARG;
    strcpy(response.error_message, "Invalid argument provided");
    response.result = NULL;
    
    /* Serialize */
    char *json = NULL;
    int ret = buckets_rpc_response_serialize(&response, &json);
    cr_assert_eq(ret, BUCKETS_OK, "Error response serialize should succeed");
    
    /* Parse */
    buckets_rpc_response_t *parsed = NULL;
    ret = buckets_rpc_response_parse(json, &parsed);
    cr_assert_eq(ret, BUCKETS_OK, "Error response parse should succeed");
    
    /* Verify fields */
    cr_assert_eq(parsed->error_code, BUCKETS_ERR_INVALID_ARG, "Error code should match");
    cr_assert_str_eq(parsed->error_message, "Invalid argument provided", 
                     "Error message should match");
    cr_assert_null(parsed->result, "Result should be NULL on error");
    
    /* Cleanup */
    buckets_free(json);
    buckets_rpc_response_free(parsed);
}

/* ===================================================================
 * RPC Handler Tests
 * ===================================================================*/

static int test_handler_called = 0;

static int test_handler(const char *method, cJSON *params,
                       cJSON **result, int *error_code,
                       char *error_message, void *user_data)
{
    (void)params;
    (void)user_data;
    
    test_handler_called++;
    
    /* Simple echo handler */
    *result = cJSON_CreateObject();
    cJSON_AddStringToObject(*result, "method", method);
    cJSON_AddStringToObject(*result, "status", "success");
    *error_code = 0;
    error_message[0] = '\0';
    
    return BUCKETS_OK;
}

Test(rpc, register_handler)
{
    int ret = buckets_rpc_register_handler(ctx, "test.echo", test_handler, NULL);
    cr_assert_eq(ret, BUCKETS_OK, "Handler registration should succeed");
}

Test(rpc, register_duplicate_handler)
{
    int ret = buckets_rpc_register_handler(ctx, "test.dup", test_handler, NULL);
    cr_assert_eq(ret, BUCKETS_OK, "First registration should succeed");
    
    /* Try to register same method again */
    ret = buckets_rpc_register_handler(ctx, "test.dup", test_handler, NULL);
    cr_assert_neq(ret, BUCKETS_OK, "Duplicate registration should fail");
}

Test(rpc, dispatch_success)
{
    /* Register handler */
    buckets_rpc_register_handler(ctx, "test.dispatch", test_handler, NULL);
    
    /* Create request */
    buckets_rpc_request_t request;
    memset(&request, 0, sizeof(request));
    strcpy(request.id, "dispatch-test");
    strcpy(request.method, "test.dispatch");
    request.timestamp = time(NULL);
    request.params = NULL;
    
    /* Dispatch */
    test_handler_called = 0;
    buckets_rpc_response_t *response = NULL;
    int ret = buckets_rpc_dispatch(ctx, &request, &response);
    
    cr_assert_eq(ret, BUCKETS_OK, "Dispatch should succeed");
    cr_assert_not_null(response, "Response should not be NULL");
    cr_assert_eq(test_handler_called, 1, "Handler should be called once");
    cr_assert_eq(response->error_code, 0, "Response should have no error");
    cr_assert_str_eq(response->id, "dispatch-test", "Response ID should match request");
    
    /* Verify result */
    cr_assert_not_null(response->result, "Result should not be NULL");
    cJSON *method = cJSON_GetObjectItem(response->result, "method");
    cr_assert_not_null(method, "method should exist in result");
    cr_assert_str_eq(method->valuestring, "test.dispatch", "method should match");
    
    /* Cleanup */
    buckets_rpc_response_free(response);
}

Test(rpc, dispatch_method_not_found)
{
    /* Create request for unregistered method */
    buckets_rpc_request_t request;
    memset(&request, 0, sizeof(request));
    strcpy(request.id, "not-found-test");
    strcpy(request.method, "test.nonexistent");
    request.timestamp = time(NULL);
    request.params = NULL;
    
    /* Dispatch */
    buckets_rpc_response_t *response = NULL;
    int ret = buckets_rpc_dispatch(ctx, &request, &response);
    
    cr_assert_eq(ret, BUCKETS_OK, "Dispatch should succeed even if method not found");
    cr_assert_not_null(response, "Response should not be NULL");
    cr_assert_neq(response->error_code, 0, "Response should have error code");
    cr_assert_null(response->result, "Result should be NULL on error");
    
    /* Cleanup */
    buckets_rpc_response_free(response);
}
