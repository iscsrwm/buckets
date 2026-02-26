/**
 * Migration Orchestrator Implementation
 * 
 * Coordinates scanner and worker pool for complete migration workflow.
 * 
 * State Machine:
 * IDLE → SCANNING → MIGRATING → COMPLETED
 *              ↓         ↕
 *           FAILED    PAUSED
 * 
 * Responsibilities:
 * 1. Job lifecycle management (create, start, stop)
 * 2. State machine transitions
 * 3. Progress tracking and ETA calculation
 * 4. Pause/resume capability
 * 5. Event callbacks for status updates
 * 6. Job persistence (save/load)
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_migration.h"
#include "buckets_io.h"
#include "cJSON.h"

/* ===================================================================
 * State Machine Helpers
 * ===================================================================*/

/**
 * Check if state transition is valid
 */
static bool is_valid_transition(buckets_migration_state_t from,
                                  buckets_migration_state_t to)
{
    switch (from) {
        case BUCKETS_MIGRATION_STATE_IDLE:
            return (to == BUCKETS_MIGRATION_STATE_SCANNING ||
                    to == BUCKETS_MIGRATION_STATE_FAILED);
        
        case BUCKETS_MIGRATION_STATE_SCANNING:
            return (to == BUCKETS_MIGRATION_STATE_MIGRATING ||
                    to == BUCKETS_MIGRATION_STATE_COMPLETED ||
                    to == BUCKETS_MIGRATION_STATE_FAILED);
        
        case BUCKETS_MIGRATION_STATE_MIGRATING:
            return (to == BUCKETS_MIGRATION_STATE_PAUSED ||
                    to == BUCKETS_MIGRATION_STATE_COMPLETED ||
                    to == BUCKETS_MIGRATION_STATE_FAILED);
        
        case BUCKETS_MIGRATION_STATE_PAUSED:
            return (to == BUCKETS_MIGRATION_STATE_MIGRATING ||
                    to == BUCKETS_MIGRATION_STATE_FAILED);
        
        case BUCKETS_MIGRATION_STATE_COMPLETED:
        case BUCKETS_MIGRATION_STATE_FAILED:
            return false;  /* Terminal states */
        
        default:
            return false;
    }
}

/**
 * Transition to new state
 */
static int transition_state(buckets_migration_job_t *job,
                             buckets_migration_state_t new_state)
{
    pthread_mutex_lock(&job->lock);
    
    buckets_migration_state_t old_state = job->state;
    
    if (!is_valid_transition(old_state, new_state)) {
        pthread_mutex_unlock(&job->lock);
        buckets_error("Invalid state transition: %d -> %d", old_state, new_state);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    job->state = new_state;
    
    pthread_mutex_unlock(&job->lock);
    
    buckets_info("Job %s: %d -> %d", job->job_id, old_state, new_state);
    
    /* Fire event callback */
    if (job->callback) {
        job->callback(job, "state_change", job->callback_user_data);
    }
    
    return BUCKETS_OK;
}

/* ===================================================================
 * Progress Tracking
 * ===================================================================*/

/**
 * Update progress from worker pool stats
 */
static void update_progress(buckets_migration_job_t *job)
{
    if (!job->worker_pool) {
        return;
    }
    
    buckets_worker_stats_t stats;
    buckets_worker_pool_get_stats(job->worker_pool, &stats);
    
    pthread_mutex_lock(&job->lock);
    
    job->migrated_objects = stats.tasks_completed;
    job->failed_objects = stats.tasks_failed;
    job->bytes_migrated = stats.bytes_migrated;
    
    /* Calculate ETA */
    if (stats.throughput_mbps > 0 && job->bytes_total > 0) {
        i64 bytes_remaining = job->bytes_total - job->bytes_migrated;
        double mb_remaining = (double)bytes_remaining / (1024.0 * 1024.0);
        job->estimated_completion = (i64)(mb_remaining / stats.throughput_mbps);
    }
    
    pthread_mutex_unlock(&job->lock);
}

/**
 * Check if checkpoint should be saved
 * 
 * Criteria: Every 1000 objects OR every 5 minutes
 */
static bool should_checkpoint(buckets_migration_job_t *job)
{
    time_t now = time(NULL);
    i64 objects_since_checkpoint = job->migrated_objects - job->last_checkpoint_objects;
    time_t time_since_checkpoint = now - job->last_checkpoint_time;
    
    /* Checkpoint every 1000 objects */
    if (objects_since_checkpoint >= 1000) {
        return true;
    }
    
    /* Checkpoint every 5 minutes (300 seconds) */
    if (time_since_checkpoint >= 300) {
        return true;
    }
    
    return false;
}

/**
 * Save checkpoint if needed
 */
static void save_checkpoint_if_needed(buckets_migration_job_t *job)
{
    if (!should_checkpoint(job)) {
        return;
    }
    
    buckets_info("Job %s: Saving checkpoint (objects: %lld, time: %llds since last)",
                 job->job_id,
                 (long long)job->migrated_objects,
                 (long long)(time(NULL) - job->last_checkpoint_time));
    
    int ret = buckets_migration_job_save(job, job->checkpoint_path);
    if (ret == BUCKETS_OK) {
        job->last_checkpoint_time = time(NULL);
        job->last_checkpoint_objects = job->migrated_objects;
        buckets_info("Job %s: Checkpoint saved to %s", job->job_id, job->checkpoint_path);
    } else {
        buckets_warn("Job %s: Failed to save checkpoint: %d", job->job_id, ret);
    }
}

/* ===================================================================
 * Public API
 * ===================================================================*/

buckets_migration_job_t* buckets_migration_job_create(i64 source_gen,
                                                       i64 target_gen,
                                                       buckets_cluster_topology_t *old_topology,
                                                       buckets_cluster_topology_t *new_topology,
                                                       char **disk_paths,
                                                       int disk_count)
{
    if (!old_topology || !new_topology || !disk_paths || disk_count <= 0) {
        return NULL;
    }
    
    buckets_migration_job_t *job = buckets_calloc(1, sizeof(buckets_migration_job_t));
    if (!job) {
        return NULL;
    }
    
    /* Generate job ID */
    snprintf(job->job_id, sizeof(job->job_id),
             "migration-gen-%lld-to-%lld",
             (long long)source_gen, (long long)target_gen);
    
    job->source_generation = source_gen;
    job->target_generation = target_gen;
    job->start_time = 0;  /* Set when started */
    job->estimated_completion = 0;
    job->state = BUCKETS_MIGRATION_STATE_IDLE;
    
    /* Initialize progress */
    job->total_objects = 0;
    job->migrated_objects = 0;
    job->failed_objects = 0;
    job->bytes_total = 0;
    job->bytes_migrated = 0;
    
    /* Store topology references (not owned) */
    job->old_topology = old_topology;
    job->new_topology = new_topology;
    
    /* Store disk paths (not owned) */
    job->disk_paths = disk_paths;
    job->disk_count = disk_count;
    
    /* Components initialized on demand */
    job->scanner = NULL;
    job->worker_pool = NULL;
    job->throttle = NULL;  /* Throttle is optional */
    
    /* Checkpointing */
    job->last_checkpoint_time = 0;
    job->last_checkpoint_objects = 0;
    snprintf(job->checkpoint_path, sizeof(job->checkpoint_path),
             "/tmp/%s.checkpoint", job->job_id);
    
    /* No callback by default */
    job->callback = NULL;
    job->callback_user_data = NULL;
    
    pthread_mutex_init(&job->lock, NULL);
    
    buckets_info("Created migration job: %s", job->job_id);
    
    return job;
}

int buckets_migration_job_start(buckets_migration_job_t *job)
{
    if (!job) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Must be in IDLE state */
    pthread_mutex_lock(&job->lock);
    if (job->state != BUCKETS_MIGRATION_STATE_IDLE) {
        pthread_mutex_unlock(&job->lock);
        return BUCKETS_ERR_INVALID_ARG;
    }
    pthread_mutex_unlock(&job->lock);
    
    /* Transition to SCANNING */
    int ret = transition_state(job, BUCKETS_MIGRATION_STATE_SCANNING);
    if (ret != BUCKETS_OK) {
        return ret;
    }
    
    job->start_time = time(NULL);
    job->last_checkpoint_time = job->start_time;  /* Initialize checkpoint timer */
    job->last_checkpoint_objects = 0;
    
    /* Initialize scanner */
    job->scanner = buckets_scanner_init(job->disk_paths, job->disk_count,
                                         job->old_topology, job->new_topology);
    if (!job->scanner) {
        transition_state(job, BUCKETS_MIGRATION_STATE_FAILED);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Perform scan */
    buckets_info("Job %s: Starting scan...", job->job_id);
    
    buckets_migration_task_t *tasks = NULL;
    int queue_size = 0;
    int task_count = 0;
    
    ret = buckets_scanner_scan(job->scanner, &tasks, &queue_size, &task_count);
    if (ret != BUCKETS_OK) {
        buckets_error("Job %s: Scan failed", job->job_id);
        transition_state(job, BUCKETS_MIGRATION_STATE_FAILED);
        return ret;
    }
    
    /* Update job with scan results */
    pthread_mutex_lock(&job->lock);
    job->total_objects = task_count;
    
    /* Calculate total bytes */
    job->bytes_total = 0;
    for (int i = 0; i < task_count; i++) {
        job->bytes_total += tasks[i].size;
    }
    pthread_mutex_unlock(&job->lock);
    
    buckets_info("Job %s: Scan complete - %d objects (%lld bytes)",
                 job->job_id, task_count, (long long)job->bytes_total);
    
    if (task_count == 0) {
        /* Nothing to migrate */
        buckets_info("Job %s: No objects need migration", job->job_id);
        buckets_free(tasks);
        transition_state(job, BUCKETS_MIGRATION_STATE_COMPLETED);
        return BUCKETS_OK;
    }
    
    /* Transition to MIGRATING */
    ret = transition_state(job, BUCKETS_MIGRATION_STATE_MIGRATING);
    if (ret != BUCKETS_OK) {
        buckets_free(tasks);
        return ret;
    }
    
    /* Initialize worker pool (16 workers) */
    job->worker_pool = buckets_worker_pool_create(16, job->old_topology, job->new_topology,
                                                    job->disk_paths, job->disk_count);
    if (!job->worker_pool) {
        buckets_free(tasks);
        transition_state(job, BUCKETS_MIGRATION_STATE_FAILED);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Start workers */
    ret = buckets_worker_pool_start(job->worker_pool);
    if (ret != BUCKETS_OK) {
        buckets_free(tasks);
        transition_state(job, BUCKETS_MIGRATION_STATE_FAILED);
        return ret;
    }
    
    /* Submit tasks */
    buckets_info("Job %s: Submitting %d tasks to worker pool", job->job_id, task_count);
    ret = buckets_worker_pool_submit(job->worker_pool, tasks, task_count);
    buckets_free(tasks);  /* Worker pool has copied tasks */
    
    if (ret != BUCKETS_OK) {
        transition_state(job, BUCKETS_MIGRATION_STATE_FAILED);
        return ret;
    }
    
    buckets_info("Job %s: Migration started", job->job_id);
    
    return BUCKETS_OK;
}

int buckets_migration_job_pause(buckets_migration_job_t *job)
{
    if (!job) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Must be in MIGRATING state */
    pthread_mutex_lock(&job->lock);
    if (job->state != BUCKETS_MIGRATION_STATE_MIGRATING) {
        pthread_mutex_unlock(&job->lock);
        return BUCKETS_ERR_INVALID_ARG;
    }
    pthread_mutex_unlock(&job->lock);
    
    /* Stop worker pool */
    if (job->worker_pool) {
        int ret = buckets_worker_pool_stop(job->worker_pool);
        if (ret != BUCKETS_OK) {
            return ret;
        }
    }
    
    return transition_state(job, BUCKETS_MIGRATION_STATE_PAUSED);
}

int buckets_migration_job_resume(buckets_migration_job_t *job)
{
    if (!job) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Must be in PAUSED state */
    pthread_mutex_lock(&job->lock);
    if (job->state != BUCKETS_MIGRATION_STATE_PAUSED) {
        pthread_mutex_unlock(&job->lock);
        return BUCKETS_ERR_INVALID_ARG;
    }
    pthread_mutex_unlock(&job->lock);
    
    /* Restart worker pool */
    if (job->worker_pool) {
        int ret = buckets_worker_pool_start(job->worker_pool);
        if (ret != BUCKETS_OK) {
            return ret;
        }
    }
    
    return transition_state(job, BUCKETS_MIGRATION_STATE_MIGRATING);
}

int buckets_migration_job_stop(buckets_migration_job_t *job)
{
    if (!job) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* If already in a terminal state, nothing to do */
    if (job->state == BUCKETS_MIGRATION_STATE_COMPLETED ||
        job->state == BUCKETS_MIGRATION_STATE_FAILED) {
        return BUCKETS_OK;
    }
    
    /* Stop worker pool if running */
    if (job->worker_pool) {
        buckets_worker_pool_stop(job->worker_pool);
    }
    
    return transition_state(job, BUCKETS_MIGRATION_STATE_FAILED);
}

int buckets_migration_job_wait(buckets_migration_job_t *job)
{
    if (!job) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Wait for terminal state */
    while (true) {
        pthread_mutex_lock(&job->lock);
        buckets_migration_state_t state = job->state;
        pthread_mutex_unlock(&job->lock);
        
        if (state == BUCKETS_MIGRATION_STATE_COMPLETED ||
            state == BUCKETS_MIGRATION_STATE_FAILED) {
            break;
        }
        
        /* Update progress if migrating */
        if (state == BUCKETS_MIGRATION_STATE_MIGRATING) {
            update_progress(job);
            
            /* Save checkpoint if needed (every 1000 objects or 5 minutes) */
            save_checkpoint_if_needed(job);
            
            /* Fire progress callback */
            if (job->callback) {
                job->callback(job, "progress", job->callback_user_data);
            }
            
            /* Check if migration complete */
            if (job->worker_pool) {
                buckets_worker_pool_wait(job->worker_pool);
                
                /* All tasks processed - mark complete */
                transition_state(job, BUCKETS_MIGRATION_STATE_COMPLETED);
                
                /* Save final checkpoint */
                buckets_migration_job_save(job, job->checkpoint_path);
                
                break;
            }
        }
        
        /* Sleep 100ms */
        struct timespec ts = {0, 100000000L};
        nanosleep(&ts, NULL);
    }
    
    buckets_info("Job %s: Complete", job->job_id);
    
    return BUCKETS_OK;
}

buckets_migration_state_t buckets_migration_job_get_state(buckets_migration_job_t *job)
{
    if (!job) {
        return BUCKETS_MIGRATION_STATE_FAILED;
    }
    
    pthread_mutex_lock(&job->lock);
    buckets_migration_state_t state = job->state;
    pthread_mutex_unlock(&job->lock);
    
    return state;
}

int buckets_migration_job_get_progress(buckets_migration_job_t *job,
                                         i64 *total,
                                         i64 *completed,
                                         i64 *failed,
                                         double *percent,
                                         i64 *eta)
{
    if (!job) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Update from worker pool first */
    if (job->state == BUCKETS_MIGRATION_STATE_MIGRATING) {
        update_progress(job);
    }
    
    pthread_mutex_lock(&job->lock);
    
    if (total) *total = job->total_objects;
    if (completed) *completed = job->migrated_objects;
    if (failed) *failed = job->failed_objects;
    
    if (percent) {
        if (job->total_objects > 0) {
            *percent = (double)job->migrated_objects / (double)job->total_objects * 100.0;
        } else {
            *percent = 0.0;
        }
    }
    
    if (eta) *eta = job->estimated_completion;
    
    pthread_mutex_unlock(&job->lock);
    
    return BUCKETS_OK;
}

int buckets_migration_job_set_callback(buckets_migration_job_t *job,
                                         buckets_migration_event_callback_t callback,
                                         void *user_data)
{
    if (!job) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&job->lock);
    job->callback = callback;
    job->callback_user_data = user_data;
    pthread_mutex_unlock(&job->lock);
    
    return BUCKETS_OK;
}

int buckets_migration_job_save(buckets_migration_job_t *job, const char *path)
{
    if (!job || !path) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&job->lock);
    
    // Create checkpoint JSON
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        pthread_mutex_unlock(&job->lock);
        return BUCKETS_ERR_NOMEM;
    }
    
    // Job identification
    cJSON_AddStringToObject(root, "job_id", job->job_id);
    cJSON_AddNumberToObject(root, "source_generation", (double)job->source_generation);
    cJSON_AddNumberToObject(root, "target_generation", (double)job->target_generation);
    
    // State
    cJSON_AddNumberToObject(root, "state", (double)job->state);
    cJSON_AddNumberToObject(root, "checkpoint_time", (double)time(NULL));
    cJSON_AddNumberToObject(root, "start_time", (double)job->start_time);
    
    // Progress
    cJSON_AddNumberToObject(root, "total_objects", (double)job->total_objects);
    cJSON_AddNumberToObject(root, "migrated_objects", (double)job->migrated_objects);
    cJSON_AddNumberToObject(root, "failed_objects", (double)job->failed_objects);
    cJSON_AddNumberToObject(root, "bytes_total", (double)job->bytes_total);
    cJSON_AddNumberToObject(root, "bytes_migrated", (double)job->bytes_migrated);
    
    pthread_mutex_unlock(&job->lock);
    
    // Serialize to string
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    
    if (!json_str) {
        return BUCKETS_ERR_NOMEM;
    }
    
    // Write atomically to file
    int ret = buckets_atomic_write(path, json_str, strlen(json_str));
    cJSON_free(json_str);
    
    if (ret != BUCKETS_OK) {
        buckets_error("Job %s: Failed to save checkpoint to %s", job->job_id, path);
        return ret;
    }
    
    buckets_info("Job %s: Checkpoint saved to %s", job->job_id, path);
    
    return BUCKETS_OK;
}

buckets_migration_job_t* buckets_migration_job_load(const char *path)
{
    if (!path) {
        return NULL;
    }
    
    // Read checkpoint file
    void *json_data = NULL;
    size_t json_len = 0;
    
    int ret = buckets_atomic_read(path, &json_data, &json_len);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to load checkpoint from %s", path);
        return NULL;
    }
    
    // Parse JSON
    cJSON *root = cJSON_Parse((char*)json_data);
    buckets_free(json_data);
    
    if (!root) {
        buckets_error("Failed to parse checkpoint JSON from %s", path);
        return NULL;
    }
    
    // Extract job ID
    cJSON *job_id_json = cJSON_GetObjectItem(root, "job_id");
    if (!job_id_json || !cJSON_IsString(job_id_json)) {
        buckets_error("Checkpoint missing job_id");
        cJSON_Delete(root);
        return NULL;
    }
    
    // Extract generations
    cJSON *source_gen_json = cJSON_GetObjectItem(root, "source_generation");
    cJSON *target_gen_json = cJSON_GetObjectItem(root, "target_generation");
    
    if (!source_gen_json || !target_gen_json) {
        buckets_error("Checkpoint missing generation numbers");
        cJSON_Delete(root);
        return NULL;
    }
    
    i64 source_gen = (i64)cJSON_GetNumberValue(source_gen_json);
    i64 target_gen = (i64)cJSON_GetNumberValue(target_gen_json);
    
    // Create job structure (Note: topologies and disk paths need to be set by caller)
    buckets_migration_job_t *job = buckets_calloc(1, sizeof(buckets_migration_job_t));
    if (!job) {
        cJSON_Delete(root);
        return NULL;
    }
    
    // Copy job ID
    strncpy(job->job_id, cJSON_GetStringValue(job_id_json), sizeof(job->job_id) - 1);
    
    job->source_generation = source_gen;
    job->target_generation = target_gen;
    
    // Extract state
    cJSON *state_json = cJSON_GetObjectItem(root, "state");
    if (state_json) {
        job->state = (buckets_migration_state_t)cJSON_GetNumberValue(state_json);
    }
    
    // Extract timestamps
    cJSON *start_time_json = cJSON_GetObjectItem(root, "start_time");
    if (start_time_json) {
        job->start_time = (time_t)cJSON_GetNumberValue(start_time_json);
    }
    
    // Extract progress
    cJSON *total_json = cJSON_GetObjectItem(root, "total_objects");
    cJSON *migrated_json = cJSON_GetObjectItem(root, "migrated_objects");
    cJSON *failed_json = cJSON_GetObjectItem(root, "failed_objects");
    cJSON *bytes_total_json = cJSON_GetObjectItem(root, "bytes_total");
    cJSON *bytes_migrated_json = cJSON_GetObjectItem(root, "bytes_migrated");
    
    if (total_json) job->total_objects = (i64)cJSON_GetNumberValue(total_json);
    if (migrated_json) job->migrated_objects = (i64)cJSON_GetNumberValue(migrated_json);
    if (failed_json) job->failed_objects = (i64)cJSON_GetNumberValue(failed_json);
    if (bytes_total_json) job->bytes_total = (i64)cJSON_GetNumberValue(bytes_total_json);
    if (bytes_migrated_json) job->bytes_migrated = (i64)cJSON_GetNumberValue(bytes_migrated_json);
    
    cJSON_Delete(root);
    
    pthread_mutex_init(&job->lock, NULL);
    
    buckets_info("Loaded checkpoint: %s (gen %lld->%lld, %lld/%lld objects)",
                 job->job_id,
                 (long long)source_gen, (long long)target_gen,
                 (long long)job->migrated_objects, (long long)job->total_objects);
    
    return job;
}

buckets_migration_job_t* buckets_migration_job_resume_from_checkpoint(
    const char *checkpoint_path,
    buckets_cluster_topology_t *old_topology,
    buckets_cluster_topology_t *new_topology,
    char **disk_paths,
    int disk_count)
{
    if (!checkpoint_path || !old_topology || !new_topology || !disk_paths || disk_count <= 0) {
        return NULL;
    }
    
    /* Load checkpoint */
    buckets_migration_job_t *job = buckets_migration_job_load(checkpoint_path);
    if (!job) {
        buckets_error("Failed to load checkpoint from %s", checkpoint_path);
        return NULL;
    }
    
    /* Restore topologies and disk paths (not stored in checkpoint) */
    job->old_topology = old_topology;
    job->new_topology = new_topology;
    job->disk_paths = disk_paths;
    job->disk_count = disk_count;
    
    /* Initialize checkpoint path */
    memcpy(job->checkpoint_path, checkpoint_path, 
           strlen(checkpoint_path) < sizeof(job->checkpoint_path) ? 
           strlen(checkpoint_path) : sizeof(job->checkpoint_path) - 1);
    
    /* Initialize checkpoint tracking */
    job->last_checkpoint_time = time(NULL);
    job->last_checkpoint_objects = job->migrated_objects;
    
    buckets_info("Job %s: Resuming from checkpoint (%lld/%lld objects migrated)",
                 job->job_id,
                 (long long)job->migrated_objects,
                 (long long)job->total_objects);
    
    /* If job was in MIGRATING state when saved, transition to PAUSED */
    /* Caller should call buckets_migration_job_resume() to continue */
    if (job->state == BUCKETS_MIGRATION_STATE_MIGRATING) {
        job->state = BUCKETS_MIGRATION_STATE_PAUSED;
        buckets_info("Job %s: Loaded in PAUSED state, call resume() to continue", 
                     job->job_id);
    }
    
    return job;
}

void buckets_migration_job_cleanup(buckets_migration_job_t *job)
{
    if (!job) {
        return;
    }
    
    buckets_info("Cleaning up job: %s", job->job_id);
    
    /* Stop and cleanup worker pool */
    if (job->worker_pool) {
        buckets_worker_pool_free(job->worker_pool);
        job->worker_pool = NULL;
    }
    
    /* Cleanup scanner */
    if (job->scanner) {
        buckets_scanner_cleanup(job->scanner);
        job->scanner = NULL;
    }
    
    pthread_mutex_destroy(&job->lock);
    
    buckets_free(job);
}
