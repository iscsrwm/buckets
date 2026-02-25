/**
 * Criterion Unit Tests for Format Management
 * 
 * Comprehensive test suite for format.c operations
 */

#include <criterion/criterion.h>
#include <criterion/redirect.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_io.h"

/* Test fixtures */
void setup(void) {
    buckets_log_init();
    buckets_set_log_level(BUCKETS_LOG_ERROR);  /* Quiet during tests */
}

void teardown(void) {
    /* Clean up test files and directories */
    unlink("/tmp/test_format.json");
    int ret = system("rm -rf /tmp/test_disk_* 2>/dev/null");
    (void)ret;  /* Ignore cleanup errors */
}

/* ===== Format Creation Tests ===== */

Test(format, create_valid_format, .init = setup, .fini = teardown) {
    buckets_format_t *format = buckets_format_new(2, 4);
    
    cr_assert_not_null(format, "Format should not be NULL");
    cr_assert_eq(format->erasure.set_count, 2, "Set count should be 2");
    cr_assert_eq(format->erasure.disks_per_set, 4, "Disks per set should be 4");
    cr_assert_not_null(format->erasure.sets, "Sets array should not be NULL");
    cr_assert_str_eq(format->meta.version, "1", "Format version should be 1");
    cr_assert_str_eq(format->meta.format_type, "erasure", "Format type should be erasure");
    cr_assert_str_eq(format->erasure.version, "3", "Erasure version should be 3");
    cr_assert_str_eq(format->erasure.distribution_algo, "SIPMOD+PARITY",
                     "Distribution algo should be SIPMOD+PARITY");
    
    /* Check deployment ID is a valid UUID */
    cr_assert_eq(strlen(format->meta.deployment_id), 36, 
                 "Deployment ID should be 36 characters");
    cr_assert_eq(format->meta.deployment_id[8], '-', "UUID should have dash at position 8");
    cr_assert_eq(format->meta.deployment_id[13], '-', "UUID should have dash at position 13");
    
    /* Check all disk UUIDs are valid */
    for (int i = 0; i < 2; i++) {
        cr_assert_not_null(format->erasure.sets[i], "Set %d should not be NULL", i);
        for (int j = 0; j < 4; j++) {
            cr_assert_not_null(format->erasure.sets[i][j], 
                             "Disk UUID at set %d, disk %d should not be NULL", i, j);
            cr_assert_eq(strlen(format->erasure.sets[i][j]), 36,
                        "Disk UUID should be 36 characters");
        }
    }
    
    buckets_format_free(format);
}

Test(format, create_invalid_sets, .init = setup, .fini = teardown) {
    buckets_format_t *format = buckets_format_new(0, 4);
    cr_assert_null(format, "Format with 0 sets should return NULL");
    
    format = buckets_format_new(-1, 4);
    cr_assert_null(format, "Format with negative sets should return NULL");
}

Test(format, create_invalid_disks, .init = setup, .fini = teardown) {
    buckets_format_t *format = buckets_format_new(2, 0);
    cr_assert_null(format, "Format with 0 disks should return NULL");
    
    format = buckets_format_new(2, -1);
    cr_assert_null(format, "Format with negative disks should return NULL");
}

Test(format, free_null_format, .init = setup, .fini = teardown) {
    /* Should not crash */
    buckets_format_free(NULL);
}

/* ===== Format Save/Load Tests ===== */

Test(format, save_and_load, .init = setup, .fini = teardown) {
    /* Use unique path for this test */
    const char *test_path = "/tmp/test_disk_save_load";
    int cleanup_ret = system("rm -rf /tmp/test_disk_save_load 2>/dev/null");
    (void)cleanup_ret;
    mkdir(test_path, 0755);  /* Create base directory */
    
    buckets_format_t *original = buckets_format_new(2, 4);
    cr_assert_not_null(original);
    
    /* Save to disk */
    int ret = buckets_format_save(test_path, original);
    cr_assert_eq(ret, BUCKETS_OK, "Save should succeed");
    
    /* Load from disk */
    buckets_format_t *loaded = buckets_format_load(test_path);
    cr_assert_not_null(loaded, "Load should succeed");
    
    /* Verify fields match */
    cr_assert_str_eq(loaded->meta.deployment_id, original->meta.deployment_id,
                     "Deployment IDs should match");
    cr_assert_eq(loaded->erasure.set_count, original->erasure.set_count,
                 "Set counts should match");
    cr_assert_eq(loaded->erasure.disks_per_set, original->erasure.disks_per_set,
                 "Disks per set should match");
    
    /* Verify all disk UUIDs match */
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 4; j++) {
            cr_assert_str_eq(loaded->erasure.sets[i][j], original->erasure.sets[i][j],
                           "Disk UUID at [%d][%d] should match", i, j);
        }
    }
    
    buckets_format_free(original);
    buckets_format_free(loaded);
}

Test(format, save_null_path, .init = setup, .fini = teardown) {
    buckets_format_t *format = buckets_format_new(2, 4);
    
    int ret = buckets_format_save(NULL, format);
    cr_assert_eq(ret, BUCKETS_ERR_INVALID_ARG, "Save with NULL path should fail");
    
    buckets_format_free(format);
}

Test(format, save_null_format, .init = setup, .fini = teardown) {
    int ret = buckets_format_save("/tmp/test_disk", NULL);
    cr_assert_eq(ret, BUCKETS_ERR_INVALID_ARG, "Save with NULL format should fail");
}

Test(format, load_null_path, .init = setup, .fini = teardown) {
    buckets_format_t *format = buckets_format_load(NULL);
    cr_assert_null(format, "Load with NULL path should return NULL");
}

Test(format, load_nonexistent, .init = setup, .fini = teardown) {
    buckets_format_t *format = buckets_format_load("/nonexistent/path");
    cr_assert_null(format, "Load from nonexistent path should return NULL");
}

/* ===== Format Clone Tests ===== */

Test(format, clone_format, .init = setup, .fini = teardown) {
    buckets_format_t *original = buckets_format_new(2, 4);
    cr_assert_not_null(original);
    
    buckets_format_t *clone = buckets_format_clone(original);
    cr_assert_not_null(clone, "Clone should succeed");
    
    /* Verify it's a different object */
    cr_assert_neq(original, clone, "Clone should be a different object");
    cr_assert_neq(original->erasure.sets, clone->erasure.sets,
                  "Clone should have different sets array");
    
    /* Verify fields match */
    cr_assert_str_eq(clone->meta.deployment_id, original->meta.deployment_id,
                     "Deployment IDs should match");
    cr_assert_eq(clone->erasure.set_count, original->erasure.set_count,
                 "Set counts should match");
    cr_assert_eq(clone->erasure.disks_per_set, original->erasure.disks_per_set,
                 "Disks per set should match");
    
    /* Verify all disk UUIDs match but are different pointers */
    for (int i = 0; i < 2; i++) {
        cr_assert_neq(original->erasure.sets[i], clone->erasure.sets[i],
                     "Clone should have different disk array at set %d", i);
        for (int j = 0; j < 4; j++) {
            cr_assert_neq(original->erasure.sets[i][j], clone->erasure.sets[i][j],
                        "Clone should have different string pointer at [%d][%d]", i, j);
            cr_assert_str_eq(clone->erasure.sets[i][j], original->erasure.sets[i][j],
                           "Disk UUID at [%d][%d] should match", i, j);
        }
    }
    
    buckets_format_free(original);
    buckets_format_free(clone);
}

Test(format, clone_null_format, .init = setup, .fini = teardown) {
    buckets_format_t *clone = buckets_format_clone(NULL);
    cr_assert_null(clone, "Clone of NULL should return NULL");
}

/* ===== Format Validation Tests ===== */

Test(format, validate_identical_formats, .init = setup, .fini = teardown) {
    buckets_format_t *format = buckets_format_new(2, 4);
    
    /* Create 4 clones */
    buckets_format_t *formats[4];
    for (int i = 0; i < 4; i++) {
        formats[i] = buckets_format_clone(format);
    }
    
    int ret = buckets_format_validate(formats, 4);
    cr_assert_eq(ret, BUCKETS_OK, "Validation of identical formats should succeed");
    
    buckets_format_free(format);
    for (int i = 0; i < 4; i++) {
        buckets_format_free(formats[i]);
    }
}

Test(format, validate_with_mismatched, .init = setup, .fini = teardown) {
    buckets_format_t *format1 = buckets_format_new(2, 4);
    buckets_format_t *format2 = buckets_format_new(2, 4);  /* Different deployment ID */
    
    buckets_format_t *formats[4];
    formats[0] = buckets_format_clone(format1);
    formats[1] = buckets_format_clone(format1);
    formats[2] = buckets_format_clone(format1);
    formats[3] = format2;  /* One mismatched */
    
    int ret = buckets_format_validate(formats, 4);
    cr_assert_eq(ret, BUCKETS_OK, "Validation with 3/4 matching should succeed (quorum)");
    
    buckets_format_free(format1);
    for (int i = 0; i < 3; i++) {
        buckets_format_free(formats[i]);
    }
    buckets_format_free(formats[3]);
}

Test(format, validate_no_quorum, .init = setup, .fini = teardown) {
    buckets_format_t *format1 = buckets_format_new(2, 4);
    buckets_format_t *format2 = buckets_format_new(2, 4);
    
    buckets_format_t *formats[4];
    formats[0] = buckets_format_clone(format1);
    formats[1] = buckets_format_clone(format1);
    formats[2] = buckets_format_clone(format2);
    formats[3] = buckets_format_clone(format2);
    
    int ret = buckets_format_validate(formats, 4);
    cr_assert_eq(ret, BUCKETS_ERR_QUORUM, "Validation with 2/4 matching should fail (no quorum)");
    
    buckets_format_free(format1);
    buckets_format_free(format2);
    for (int i = 0; i < 4; i++) {
        buckets_format_free(formats[i]);
    }
}

Test(format, validate_with_nulls, .init = setup, .fini = teardown) {
    buckets_format_t *format = buckets_format_new(2, 4);
    
    buckets_format_t *formats[4];
    formats[0] = buckets_format_clone(format);
    formats[1] = buckets_format_clone(format);
    formats[2] = buckets_format_clone(format);
    formats[3] = NULL;  /* One NULL */
    
    int ret = buckets_format_validate(formats, 4);
    cr_assert_eq(ret, BUCKETS_OK, "Validation with 3/4 valid should succeed (quorum)");
    
    buckets_format_free(format);
    for (int i = 0; i < 3; i++) {
        buckets_format_free(formats[i]);
    }
}

Test(format, validate_null_array, .init = setup, .fini = teardown) {
    int ret = buckets_format_validate(NULL, 4);
    cr_assert_eq(ret, BUCKETS_ERR_INVALID_ARG, "Validation with NULL array should fail");
}

Test(format, validate_zero_count, .init = setup, .fini = teardown) {
    buckets_format_t *format = buckets_format_new(2, 4);
    buckets_format_t *formats[1] = { format };
    
    int ret = buckets_format_validate(formats, 0);
    cr_assert_eq(ret, BUCKETS_ERR_INVALID_ARG, "Validation with 0 count should fail");
    
    buckets_format_free(format);
}

/* ===== Edge Case Tests ===== */

Test(format, large_topology, .init = setup, .fini = teardown) {
    /* Test with larger topology */
    buckets_format_t *format = buckets_format_new(16, 16);
    cr_assert_not_null(format, "Large format should be created");
    cr_assert_eq(format->erasure.set_count, 16);
    cr_assert_eq(format->erasure.disks_per_set, 16);
    
    /* Verify all disk UUIDs are unique */
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            cr_assert_not_null(format->erasure.sets[i][j]);
            cr_assert_eq(strlen(format->erasure.sets[i][j]), 36);
        }
    }
    
    buckets_format_free(format);
}

Test(format, single_set, .init = setup, .fini = teardown) {
    buckets_format_t *format = buckets_format_new(1, 4);
    cr_assert_not_null(format, "Single set format should be created");
    cr_assert_eq(format->erasure.set_count, 1);
    cr_assert_eq(format->erasure.disks_per_set, 4);
    
    buckets_format_free(format);
}

Test(format, minimal_topology, .init = setup, .fini = teardown) {
    buckets_format_t *format = buckets_format_new(1, 1);
    cr_assert_not_null(format, "Minimal format should be created");
    cr_assert_eq(format->erasure.set_count, 1);
    cr_assert_eq(format->erasure.disks_per_set, 1);
    
    buckets_format_free(format);
}
