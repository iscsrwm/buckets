/**
 * S3 XML Response Generation
 * 
 * Generates S3-compatible XML responses.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "buckets.h"
#include "buckets_s3.h"

/* ===================================================================
 * XML Generation Helpers
 * ===================================================================*/

/**
 * Escape XML special characters
 */
static void xml_escape(const char *input, char *output, size_t max_len)
{
    size_t out_pos = 0;
    
    for (size_t i = 0; input[i] != '\0' && out_pos < max_len - 6; i++) {
        switch (input[i]) {
            case '<':
                strcpy(output + out_pos, "&lt;");
                out_pos += 4;
                break;
            case '>':
                strcpy(output + out_pos, "&gt;");
                out_pos += 4;
                break;
            case '&':
                strcpy(output + out_pos, "&amp;");
                out_pos += 5;
                break;
            case '"':
                strcpy(output + out_pos, "&quot;");
                out_pos += 6;
                break;
            case '\'':
                strcpy(output + out_pos, "&apos;");
                out_pos += 6;
                break;
            default:
                output[out_pos++] = input[i];
                break;
        }
    }
    output[out_pos] = '\0';
}

/* ===================================================================
 * XML Response Generation
 * ===================================================================*/

int buckets_s3_xml_success(buckets_s3_response_t *res, const char *root_element)
{
    if (!res || !root_element) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Allocate XML buffer */
    size_t xml_size = 4096;
    char *xml = buckets_malloc(xml_size);
    if (!xml) {
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Build XML */
    int len = snprintf(xml, xml_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<%s xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n",
        root_element);
    
    /* Add ETag if present */
    if (res->etag[0] != '\0') {
        len += snprintf(xml + len, xml_size - len,
            "  <ETag>\"%s\"</ETag>\n", res->etag);
    }
    
    /* Add VersionId if present */
    if (res->version_id[0] != '\0') {
        len += snprintf(xml + len, xml_size - len,
            "  <VersionId>%s</VersionId>\n", res->version_id);
    }
    
    /* Close root element */
    len += snprintf(xml + len, xml_size - len, "</%s>\n", root_element);
    
    res->body = xml;
    res->body_len = len;
    res->status_code = 200;
    
    return BUCKETS_OK;
}

int buckets_s3_xml_error(buckets_s3_response_t *res,
                         const char *error_code,
                         const char *message,
                         const char *resource)
{
    if (!res || !error_code || !message) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Escape message and resource */
    char escaped_message[512];
    char escaped_resource[1024];
    
    xml_escape(message, escaped_message, sizeof(escaped_message));
    if (resource) {
        xml_escape(resource, escaped_resource, sizeof(escaped_resource));
    } else {
        escaped_resource[0] = '\0';
    }
    
    /* Allocate XML buffer */
    size_t xml_size = 4096;
    char *xml = buckets_malloc(xml_size);
    if (!xml) {
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Build error XML */
    int len = snprintf(xml, xml_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Error>\n"
        "  <Code>%s</Code>\n"
        "  <Message>%s</Message>\n",
        error_code, escaped_message);
    
    if (resource) {
        len += snprintf(xml + len, xml_size - len,
            "  <Resource>%s</Resource>\n", escaped_resource);
    }
    
    /* Add request ID (use current timestamp as simple ID) */
    len += snprintf(xml + len, xml_size - len,
        "  <RequestId>%ld</RequestId>\n"
        "</Error>\n",
        (long)time(NULL));
    
    res->body = xml;
    res->body_len = len;
    
    /* Copy error code for reference */
    strncpy(res->error_code, error_code, sizeof(res->error_code) - 1);
    res->error_code[sizeof(res->error_code) - 1] = '\0';
    strncpy(res->error_message, message, sizeof(res->error_message) - 1);
    res->error_message[sizeof(res->error_message) - 1] = '\0';
    
    /* Set HTTP status based on error code */
    if (strcmp(error_code, "NoSuchBucket") == 0 ||
        strcmp(error_code, "NoSuchKey") == 0 ||
        strcmp(error_code, "NoSuchUpload") == 0) {
        res->status_code = 404;
    } else if (strcmp(error_code, "AccessDenied") == 0 ||
               strcmp(error_code, "SignatureDoesNotMatch") == 0) {
        res->status_code = 403;
    } else if (strcmp(error_code, "BucketAlreadyExists") == 0 ||
               strcmp(error_code, "BucketAlreadyOwnedByYou") == 0 ||
               strcmp(error_code, "BucketNotEmpty") == 0) {
        res->status_code = 409;
    } else if (strcmp(error_code, "InvalidBucketName") == 0 ||
               strcmp(error_code, "InvalidRequest") == 0 ||
               strcmp(error_code, "InvalidPart") == 0 ||
               strcmp(error_code, "InvalidPartNumber") == 0 ||
               strcmp(error_code, "MalformedXML") == 0) {
        res->status_code = 400;
    } else {
        res->status_code = 500;
    }
    
    return BUCKETS_OK;
}

int buckets_s3_xml_add_element(char *xml_body, size_t max_len,
                               const char *key, const char *value)
{
    if (!xml_body || !key || !value) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    char escaped_value[1024];
    xml_escape(value, escaped_value, sizeof(escaped_value));
    
    size_t current_len = strlen(xml_body);
    int added = snprintf(xml_body + current_len, max_len - current_len,
                         "  <%s>%s</%s>\n", key, escaped_value, key);
    
    if (added < 0 || (size_t)added >= max_len - current_len) {
        return BUCKETS_ERR_NOMEM;
    }
    
    return BUCKETS_OK;
}
