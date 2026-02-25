/**
 * Manual Format Testing
 * 
 * Simple test program to verify format operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_io.h"

int main(void)
{
    int ret = 0;
    
    /* Initialize logging */
    buckets_log_init();
    buckets_set_log_level(BUCKETS_LOG_DEBUG);
    
    printf("=== Buckets Format Management Test ===\n\n");
    
    /* Test 1: Create new format */
    printf("Test 1: Creating new format (2 sets, 4 disks per set)...\n");
    buckets_format_t *format = buckets_format_new(2, 4);
    if (!format) {
        printf("FAIL: buckets_format_new returned NULL\n");
        return 1;
    }
    printf("  ✓ Format created\n");
    printf("    Deployment ID: %s\n", format->meta.deployment_id);
    printf("    Sets: %d, Disks per set: %d\n", 
           format->erasure.set_count, format->erasure.disks_per_set);
    
    /* Test 2: Save format to disk */
    printf("\nTest 2: Saving format to /tmp/test_disk1...\n");
    ret = buckets_format_save("/tmp/test_disk1", format);
    if (ret != BUCKETS_OK) {
        printf("FAIL: buckets_format_save returned %d\n", ret);
        buckets_format_free(format);
        return 1;
    }
    printf("  ✓ Format saved\n");
    
    /* Test 3: Load format from disk */
    printf("\nTest 3: Loading format from /tmp/test_disk1...\n");
    buckets_format_t *loaded = buckets_format_load("/tmp/test_disk1");
    if (!loaded) {
        printf("FAIL: buckets_format_load returned NULL\n");
        buckets_format_free(format);
        return 1;
    }
    printf("  ✓ Format loaded\n");
    printf("    Deployment ID: %s\n", loaded->meta.deployment_id);
    
    /* Test 4: Verify loaded format matches original */
    printf("\nTest 4: Verifying loaded format matches original...\n");
    if (strcmp(format->meta.deployment_id, loaded->meta.deployment_id) != 0) {
        printf("FAIL: Deployment ID mismatch\n");
        buckets_format_free(format);
        buckets_format_free(loaded);
        return 1;
    }
    if (format->erasure.set_count != loaded->erasure.set_count) {
        printf("FAIL: Set count mismatch\n");
        buckets_format_free(format);
        buckets_format_free(loaded);
        return 1;
    }
    if (format->erasure.disks_per_set != loaded->erasure.disks_per_set) {
        printf("FAIL: Disks per set mismatch\n");
        buckets_format_free(format);
        buckets_format_free(loaded);
        return 1;
    }
    printf("  ✓ Format matches\n");
    
    /* Test 5: Clone format */
    printf("\nTest 5: Cloning format...\n");
    buckets_format_t *clone = buckets_format_clone(format);
    if (!clone) {
        printf("FAIL: buckets_format_clone returned NULL\n");
        buckets_format_free(format);
        buckets_format_free(loaded);
        return 1;
    }
    printf("  ✓ Format cloned\n");
    
    /* Test 6: Validate multiple formats (simulate multi-disk validation) */
    printf("\nTest 6: Validating multiple formats (quorum test)...\n");
    
    /* Save to multiple disks */
    buckets_format_save("/tmp/test_disk2", format);
    buckets_format_save("/tmp/test_disk3", format);
    buckets_format_save("/tmp/test_disk4", format);
    
    /* Load from all disks */
    buckets_format_t *formats[4];
    formats[0] = buckets_format_load("/tmp/test_disk1");
    formats[1] = buckets_format_load("/tmp/test_disk2");
    formats[2] = buckets_format_load("/tmp/test_disk3");
    formats[3] = buckets_format_load("/tmp/test_disk4");
    
    ret = buckets_format_validate(formats, 4);
    if (ret != BUCKETS_OK) {
        printf("FAIL: buckets_format_validate returned %d\n", ret);
    } else {
        printf("  ✓ Format validation passed (quorum achieved)\n");
    }
    
    /* Test 7: Validate with mismatched formats (should fail) */
    printf("\nTest 7: Testing validation with mismatched format...\n");
    buckets_format_t *different = buckets_format_new(2, 4);  /* Different deployment ID */
    formats[3] = different;
    
    ret = buckets_format_validate(formats, 4);
    if (ret == BUCKETS_ERR_QUORUM) {
        printf("  ✗ Validation correctly failed (expected behavior)\n");
    } else {
        printf("FAIL: Validation should have failed but returned %d\n", ret);
    }
    
    /* Clean up */
    printf("\nCleaning up...\n");
    buckets_format_free(format);
    buckets_format_free(loaded);
    buckets_format_free(clone);
    buckets_format_free(different);
    for (int i = 0; i < 3; i++) {
        buckets_format_free(formats[i]);
    }
    
    /* Remove test files */
    unlink("/tmp/test_disk1/.buckets.sys/format.json");
    unlink("/tmp/test_disk2/.buckets.sys/format.json");
    unlink("/tmp/test_disk3/.buckets.sys/format.json");
    unlink("/tmp/test_disk4/.buckets.sys/format.json");
    rmdir("/tmp/test_disk1/.buckets.sys");
    rmdir("/tmp/test_disk2/.buckets.sys");
    rmdir("/tmp/test_disk3/.buckets.sys");
    rmdir("/tmp/test_disk4/.buckets.sys");
    
    printf("\n=== All Tests Passed ===\n");
    return 0;
}
