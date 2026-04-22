/**
 * Buckets - Main Entry Point
 * 
 * High-performance S3-compatible object storage server
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "buckets.h"
#include "buckets_net.h"
#include "buckets_s3.h"
#include "buckets_config.h"
#include "buckets_storage.h"
#include "buckets_cluster.h"
#include "buckets_registry.h"
#include "buckets_placement.h"
#include "buckets_worker_pool.h"
#include "buckets_debug.h"
#include "storage/async_replication.h"

/* Global config pointer for distributed operations */
static buckets_config_t *g_global_config = NULL;

/**
 * Get global configuration
 * Used by distributed listing to find all cluster nodes
 */
buckets_config_t* buckets_get_global_config(void)
{
    return g_global_config;
}

static void print_banner(void) {
    printf("\n");
    printf(" ____             _        _       \n");
    printf("|  _ \\           | |      | |      \n");
    printf("| |_) |_   _  ___| | _____| |_ ___ \n");
    printf("|  _ <| | | |/ __| |/ / _ \\ __/ __|\n");
    printf("| |_) | |_| | (__|   <  __/ |_\\__ \\\n");
    printf("|____/ \\__,_|\\___|_|\\_\\___|\\__|___/\n");
    printf("\n");
    printf("Version %s\n", buckets_version());
    printf("High-Performance S3-Compatible Object Storage\n");
    printf("\n");
}

static void print_usage(const char *progname) {
    printf("Usage: %s [command] [options]\n\n", progname);
    printf("Commands:\n");
    printf("  server [options]    Start S3 API server\n");
    printf("  format [options]    Format disks for cluster operation\n");
    printf("  creds <subcommand>  Manage S3 credentials\n");
    printf("  debug <subcommand>  Debug instrumentation commands\n");
    printf("  version             Print version information\n");
    printf("  help                Show this help message\n");
    printf("\n");
    printf("Server Options:\n");
    printf("  --config <file>     Load configuration from JSON file\n");
    printf("  --port <port>       Server port (default: 9000)\n");
    printf("\n");
    printf("Format Options:\n");
    printf("  --config <file>     Configuration file with disk paths (required)\n");
    printf("  --force             Force formatting even if disks already formatted\n");
    printf("\n");
    printf("Credential Commands:\n");
    printf("  creds list          List all credentials (shows access keys only)\n");
    printf("  creds create        Create new credentials\n");
    printf("  creds delete <key>  Delete credential by access key\n");
    printf("  creds enable <key>  Enable a credential\n");
    printf("  creds disable <key> Disable a credential\n");
    printf("\n");
    printf("Debug Commands:\n");
    printf("  debug enable        Enable debug instrumentation\n");
    printf("  debug disable       Disable debug instrumentation\n");
    printf("  debug stats         Print debug statistics\n");
    printf("  debug reset         Reset debug statistics\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s server                           # Start on default port 9000\n", progname);
    printf("  %s server --port 8080               # Start on port 8080\n", progname);
    printf("  %s server --config config/node1.json  # Start with config file\n", progname);
    printf("  %s format --config config/node1.json  # Format disks for node1\n", progname);
    printf("  %s creds list                       # List all credentials\n", progname);
    printf("  %s creds create                     # Create new credential\n", progname);
    printf("\n");
}

/**
 * Format disks from configuration file
 * 
 * Creates format.json and topology.json on each disk
 */
static int format_disks_from_config(const char *config_file, bool force) {
    /* Load configuration */
    buckets_config_t *config = buckets_config_load(config_file);
    if (!config) {
        fprintf(stderr, "Error: failed to load configuration from: %s\n", config_file);
        return 1;
    }
    
    /* Validate configuration */
    if (buckets_config_validate(config) != BUCKETS_OK) {
        fprintf(stderr, "Error: configuration validation failed\n");
        buckets_config_free(config);
        return 1;
    }
    
    buckets_info("Formatting %d disks for cluster", config->storage.disk_count);
    buckets_info("Node ID: %s", config->node.id);
    buckets_info("Cluster mode: %s", config->cluster.enabled ? "enabled" : "disabled");
    if (config->cluster.enabled) {
        buckets_info("  Sets: %d", config->cluster.sets);
        buckets_info("  Disks per set: %d", config->cluster.disks_per_set);
    }
    if (config->erasure.enabled) {
        buckets_info("Erasure coding: K=%d, M=%d", 
                     config->erasure.data_shards, 
                     config->erasure.parity_shards);
    }
    
    /* Check if disks already formatted (unless force) */
    if (!force) {
        bool any_formatted = false;
        for (int i = 0; i < config->storage.disk_count; i++) {
            buckets_format_t *existing = buckets_format_load(config->storage.disks[i]);
            if (existing) {
                buckets_warn("Disk %s is already formatted (deployment_id=%s)",
                            config->storage.disks[i], existing->meta.deployment_id);
                any_formatted = true;
                buckets_format_free(existing);
            }
        }
        
        if (any_formatted) {
            fprintf(stderr, "\nError: Some disks are already formatted.\n");
            fprintf(stderr, "Use --force to reformat (WARNING: destroys existing data)\n");
            buckets_config_free(config);
            return 1;
        }
    }
    
    /* Create format structure */
    int sets = config->cluster.enabled ? config->cluster.sets : 1;
    int disks_per_set = config->cluster.enabled ? config->cluster.disks_per_set : config->storage.disk_count;
    
    /* Use the cluster's deployment_id from config to generate a deterministic UUID.
     * This is critical for multi-node clusters - all nodes must use the same
     * deployment_id for consistent object placement. */
    const char *cluster_deployment_id = config->cluster.enabled ? 
                                        config->cluster.deployment_id : NULL;
    
    extern buckets_format_t* buckets_format_new_with_deployment_id(int set_count, 
                                                                   int disks_per_set,
                                                                   const char *cluster_deployment_id);
    buckets_format_t *format = buckets_format_new_with_deployment_id(sets, disks_per_set,
                                                                     cluster_deployment_id);
    if (!format) {
        buckets_error("Failed to create format structure");
        buckets_config_free(config);
        return 1;
    }
    
    buckets_info("Created format with deployment_id: %s (from cluster: %s)", 
                 format->meta.deployment_id, 
                 cluster_deployment_id ? cluster_deployment_id : "random");
    
    /* For distributed clusters, we need to figure out which disk positions
     * in the topology belong to THIS node. The topology is cluster-wide,
     * but each node only formats its local disks. */
    
    if (config->cluster.enabled && config->cluster.node_count > 0) {
        /* Distributed cluster mode: find this node's disk positions */
        int this_node_idx = -1;
        for (int i = 0; i < config->cluster.node_count; i++) {
            if (strcmp(config->cluster.nodes[i].id, config->node.id) == 0) {
                this_node_idx = i;
                break;
            }
        }
        
        if (this_node_idx < 0) {
            buckets_error("This node '%s' not found in cluster.nodes", config->node.id);
            buckets_format_free(format);
            buckets_config_free(config);
            return 1;
        }
        
        /* Calculate this node's starting position in the cluster-wide disk array.
         * Disks are numbered sequentially across all nodes. */
        int disk_offset = 0;
        for (int i = 0; i < this_node_idx; i++) {
            disk_offset += config->cluster.nodes[i].disk_count;
        }
        
        buckets_info("This node (%s) is at position %d, disk offset %d",
                     config->node.id, this_node_idx, disk_offset);
        
        /* Format only this node's local disks with their correct topology positions */
        for (int local_disk = 0; local_disk < config->storage.disk_count; local_disk++) {
            int global_disk = disk_offset + local_disk;
            int set = global_disk / disks_per_set;
            int disk_in_set = global_disk % disks_per_set;
            
            if (set >= sets) {
                buckets_error("Disk %d exceeds topology (sets=%d, disks_per_set=%d)",
                              global_disk, sets, disks_per_set);
                buckets_format_free(format);
                buckets_config_free(config);
                return 1;
            }
            
            /* Set this_disk UUID from the topology */
            strncpy(format->erasure.this_disk, format->erasure.sets[set][disk_in_set],
                    sizeof(format->erasure.this_disk) - 1);
            
            /* Save format to disk */
            buckets_info("Formatting disk %s (global=%d, set=%d, disk=%d, uuid=%s)",
                        config->storage.disks[local_disk], global_disk, set, disk_in_set,
                        format->erasure.this_disk);
            
            if (buckets_format_save(config->storage.disks[local_disk], format) != BUCKETS_OK) {
                buckets_error("Failed to save format to disk: %s", config->storage.disks[local_disk]);
                buckets_format_free(format);
                buckets_config_free(config);
                return 1;
            }
        }
    } else {
        /* Single-node mode: format all local disks sequentially */
        int disk_idx = 0;
        for (int set = 0; set < sets; set++) {
            for (int disk = 0; disk < disks_per_set; disk++) {
                if (disk_idx >= config->storage.disk_count) {
                    buckets_error("Configuration error: not enough disks for topology");
                    buckets_format_free(format);
                    buckets_config_free(config);
                    return 1;
                }
                
                /* Set this_disk UUID */
                strncpy(format->erasure.this_disk, format->erasure.sets[set][disk], 
                        sizeof(format->erasure.this_disk) - 1);
                
                /* Save format to disk */
                buckets_info("Formatting disk %s (set %d, disk %d, uuid=%s)",
                            config->storage.disks[disk_idx], set, disk, format->erasure.this_disk);
                
                if (buckets_format_save(config->storage.disks[disk_idx], format) != BUCKETS_OK) {
                    buckets_error("Failed to save format to disk: %s", config->storage.disks[disk_idx]);
                    buckets_format_free(format);
                    buckets_config_free(config);
                    return 1;
                }
                
                disk_idx++;
            }
        }
    }
    
    /* Create topology from format */
    buckets_cluster_topology_t *topology = buckets_topology_from_format(format);
    if (!topology) {
        buckets_error("Failed to create topology from format");
        buckets_format_free(format);
        buckets_config_free(config);
        return 1;
    }
    
    buckets_info("Created topology with generation 0");
    
    /* Save topology to each disk */
    for (int i = 0; i < config->storage.disk_count; i++) {
        buckets_info("Saving topology to disk: %s", config->storage.disks[i]);
        if (buckets_topology_save(config->storage.disks[i], topology) != BUCKETS_OK) {
            buckets_error("Failed to save topology to disk: %s", config->storage.disks[i]);
            buckets_topology_free(topology);
            buckets_format_free(format);
            buckets_config_free(config);
            return 1;
        }
    }
    
    buckets_info("Successfully formatted all disks");
    buckets_info("Deployment ID: %s", format->meta.deployment_id);
    buckets_info("Disks formatted: %d", config->storage.disk_count);
    
    /* Cleanup */
    buckets_topology_free(topology);
    buckets_format_free(format);
    buckets_config_free(config);
    
    return 0;
}

/* ===================================================================
 * Worker Process Callback
 * ===================================================================*/

/**
 * Server configuration passed to worker processes
 */
typedef struct {
    const char *bind_addr;
    int port;
} server_worker_config_t;

/**
 * Worker process callback - runs the HTTP server
 * Called in each forked worker process
 */
static int worker_process_main(int worker_id, void *user_data)
{
    server_worker_config_t *cfg = (server_worker_config_t*)user_data;
    
    buckets_info("Worker %d starting HTTP server on %s:%d", 
                 worker_id, cfg->bind_addr, cfg->port);
    
    /* Create server */
    uv_http_server_t *uv_server = uv_http_server_create(cfg->bind_addr, cfg->port);
    if (!uv_server) {
        buckets_error("Worker %d: Failed to create HTTP server", worker_id);
        return 1;
    }
    
    /* Register streaming S3 handlers */
    if (s3_streaming_register_handlers(uv_server) != BUCKETS_OK) {
        buckets_error("Worker %d: Failed to register S3 handlers", worker_id);
        uv_http_server_free(uv_server);
        return 1;
    }
    
    /* Start server (blocks until shutdown) */
    if (uv_http_server_start(uv_server) != BUCKETS_OK) {
        buckets_error("Worker %d: Failed to start HTTP server", worker_id);
        uv_http_server_free(uv_server);
        return 1;
    }
    
    /* Keep running */
    while (1) {
        sleep(1);
    }
    
    /* Cleanup (never reached unless signal) */
    uv_http_server_stop(uv_server);
    uv_http_server_free(uv_server);
    
    return 0;
}

int main(int argc, char *argv[]) {
    int ret = 0;

    /* Set libuv thread pool size BEFORE any libuv calls.
     * Default is 4 which is too small for distributed operations.
     * Each async handler (RPC, S3 ops) uses a thread from this pool.
     * With 6 nodes making concurrent RPCs, we need more threads to
     * prevent deadlock. 32 threads should handle typical workloads. */
    if (getenv("UV_THREADPOOL_SIZE") == NULL) {
        /* Need many threads to handle concurrent RPC calls without deadlock.
         * With 6 nodes, each request can trigger 12 parallel chunk operations,
         * and each of those may require RPC calls. 128 threads provides headroom. */
        setenv("UV_THREADPOOL_SIZE", "128", 1);
    }

    /* Initialize logging first */
    buckets_log_init();

    /* Initialize Buckets */
    if (buckets_init() != 0) {
        fprintf(stderr, "Failed to initialize Buckets\n");
        return 1;
    }
    
    /* Initialize debug instrumentation */
    buckets_debug_init();

    /* Parse command */
    if (argc < 2) {
        print_banner();
        print_usage(argv[0]);
        goto cleanup;
    }

    const char *command = argv[1];

    if (strcmp(command, "version") == 0) {
        print_banner();
    } else if (strcmp(command, "help") == 0 || strcmp(command, "--help") == 0) {
        print_banner();
        print_usage(argv[0]);
    } else if (strcmp(command, "server") == 0) {
        print_banner();
        
        /* Parse options */
        const char *config_file = NULL;
        int port = 9000;
        
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--config") == 0) {
                if (i + 1 < argc) {
                    config_file = argv[++i];
                } else {
                    fprintf(stderr, "Error: --config requires a file path\n\n");
                    ret = 1;
                    goto cleanup;
                }
            } else if (strcmp(argv[i], "--port") == 0) {
                if (i + 1 < argc) {
                    port = atoi(argv[++i]);
                    if (port <= 0 || port > 65535) {
                        fprintf(stderr, "Error: invalid port number: %s\n\n", argv[i]);
                        ret = 1;
                        goto cleanup;
                    }
                } else {
                    fprintf(stderr, "Error: --port requires a port number\n\n");
                    ret = 1;
                    goto cleanup;
                }
            } else {
                /* Legacy: bare port number */
                port = atoi(argv[i]);
                if (port <= 0 || port > 65535) {
                    fprintf(stderr, "Error: invalid port number: %s\n\n", argv[i]);
                    ret = 1;
                    goto cleanup;
                }
            }
        }
        
        /* Load configuration if specified */
        buckets_config_t *config = NULL;
        if (config_file) {
            config = buckets_config_load(config_file);
            if (!config) {
                fprintf(stderr, "Error: failed to load configuration from: %s\n\n", config_file);
                ret = 1;
                goto cleanup;
            }
            
            /* Validate configuration */
            if (buckets_config_validate(config) != BUCKETS_OK) {
                fprintf(stderr, "Error: configuration validation failed\n\n");
                buckets_config_free(config);
                ret = 1;
                goto cleanup;
            }
            
            /* Store global config for distributed operations */
            g_global_config = config;
            
            /* Use configuration values */
            port = config->server.bind_port;
            
            /* Initialize multi-disk storage */
            if (config->storage.disk_count > 0) {
                buckets_info("Initializing multi-disk storage with %d disks...", config->storage.disk_count);
                const char **disk_paths = (const char **)config->storage.disks;
                if (buckets_multidisk_init(disk_paths, config->storage.disk_count) != 0) {
                    buckets_warn("Multi-disk storage initialization failed (disks not formatted?)");
                    buckets_warn("Server will run in single-node mode using /tmp/buckets-data/");
                    buckets_warn("To format disks, run: buckets format --config <config_file>");
                } else {
                    buckets_info("Multi-disk storage initialized successfully");
                    
                    /* Initialize topology manager */
                    buckets_info("Initializing topology manager...");
                    if (buckets_topology_manager_init(config->storage.disks, config->storage.disk_count) != BUCKETS_OK) {
                        buckets_error("Failed to initialize topology manager");
                        buckets_multidisk_cleanup();
                        buckets_config_free(config);
                        ret = 1;
                        goto cleanup;
                    }
                    
                    /* Load topology from disks */
                    if (buckets_topology_manager_load() != BUCKETS_OK) {
                        buckets_error("Failed to load topology from disks");
                        buckets_topology_manager_cleanup();
                        buckets_multidisk_cleanup();
                        buckets_config_free(config);
                        ret = 1;
                        goto cleanup;
                    }
                    
                    /* Get topology to log information */
                    buckets_cluster_topology_t *topology = buckets_topology_manager_get();
                    if (topology) {
                        buckets_info("Topology loaded: generation=%ld, pools=%d", 
                                    topology->generation, topology->pool_count);
                        buckets_info("Deployment ID: %s", topology->deployment_id);
                        
                        /* Populate endpoints from cluster config */
                        if (config->cluster.enabled && config->cluster.node_count > 0) {
                            buckets_info("Populating topology endpoints from cluster configuration...");
                            if (buckets_topology_populate_endpoints_from_config(topology, config) == BUCKETS_OK) {
                                buckets_info("Topology endpoints populated successfully");
                                
                                /* Save updated topology with endpoints to all disks */
                                buckets_info("Saving topology with populated endpoints...");
                                for (int i = 0; i < config->storage.disk_count; i++) {
                                    if (buckets_topology_save(config->storage.disks[i], topology) != BUCKETS_OK) {
                                        buckets_warn("Failed to save topology to disk: %s", config->storage.disks[i]);
                                    }
                                }
                                buckets_info("Topology saved to %d disks", config->storage.disk_count);
                            } else {
                                buckets_warn("Failed to populate topology endpoints");
                            }
                        }
                    }
                    
                    /* Initialize location registry */
                    buckets_info("Initializing location registry...");
                    if (buckets_registry_init(NULL) != BUCKETS_OK) {
                        buckets_error("Failed to initialize location registry");
                        buckets_topology_manager_cleanup();
                        buckets_multidisk_cleanup();
                        buckets_config_free(config);
                        ret = 1;
                        goto cleanup;
                    }
                    buckets_info("Location registry initialized");
                    
                    /* Initialize placement system (consistent hashing) */
                    buckets_info("Initializing placement system...");
                    if (buckets_placement_init() != BUCKETS_OK) {
                        buckets_error("Failed to initialize placement system");
                        buckets_registry_cleanup();
                        buckets_topology_manager_cleanup();
                        buckets_multidisk_cleanup();
                        buckets_config_free(config);
                        ret = 1;
                        goto cleanup;
                    }
                    
                    /* Log placement stats */
                    u32 total_sets, total_disks;
                    double avg_disks;
                    buckets_placement_get_stats(&total_sets, &total_disks, &avg_disks);
                    buckets_info("Placement system initialized: %u active sets, %u total disks, %.1f avg disks/set",
                                total_sets, total_disks, avg_disks);
                    
                    /* Initialize storage layer */
                    buckets_info("Initializing storage layer...");
                    buckets_storage_config_t storage_cfg = {
                        .data_dir = config->node.data_dir,
                        .inline_threshold = 512 * 1024,  /* 512 KB - optimized for network performance */
                        .default_ec_k = config->erasure.data_shards,
                        .default_ec_m = config->erasure.parity_shards,
                        .verify_checksums = true
                    };
                    if (buckets_storage_init(&storage_cfg) != 0) {
                        buckets_error("Failed to initialize storage layer");
                        buckets_registry_cleanup();
                        buckets_topology_manager_cleanup();
                        buckets_multidisk_cleanup();
                        buckets_config_free(config);
                        ret = 1;
                        goto cleanup;
                    }
                    buckets_info("Storage layer initialized");
                    
                    /* Initialize async replication system */
                    buckets_info("Initializing async replication (4 workers)...");
                    if (async_replication_init(4) != 0) {
                        buckets_warn("Failed to initialize async replication, running without it");
                    } else {
                        buckets_info("Async replication initialized");
                    }
                    
                    /* Initialize distributed storage (RPC for remote chunks) */
                    buckets_info("Initializing distributed storage...");
                    if (buckets_distributed_storage_init() != BUCKETS_OK) {
                        buckets_error("Failed to initialize distributed storage");
                        buckets_registry_cleanup();
                        buckets_topology_manager_cleanup();
                        buckets_multidisk_cleanup();
                        buckets_config_free(config);
                        ret = 1;
                        goto cleanup;
                    }
                    
                    /* Set local node endpoint for distributed operations */
                    if (config->node.endpoint && config->node.endpoint[0] != '\0') {
                        if (buckets_distributed_set_local_endpoint(config->node.endpoint) != BUCKETS_OK) {
                            buckets_warn("Failed to set local node endpoint: %s", config->node.endpoint);
                        } else {
                            buckets_info("Local node endpoint: %s", config->node.endpoint);
                        }
                    }
                    
                    buckets_info("Distributed storage initialized");
                }
            }
        }

        buckets_info("Starting Buckets S3 server on port %d...", port);
        if (config) {
            buckets_info("Node ID: %s", config->node.id);
            buckets_info("Data directory: %s", config->node.data_dir);
            buckets_info("Cluster mode: %s", config->cluster.enabled ? "enabled" : "disabled");
            if (config->cluster.enabled) {
                buckets_info("  Peers: %d", config->cluster.peer_count);
                buckets_info("  Sets: %d", config->cluster.sets);
                buckets_info("  Disks per set: %d", config->cluster.disks_per_set);
            }
            buckets_info("Erasure coding: %s", config->erasure.enabled ? "enabled" : "disabled");
            if (config->erasure.enabled) {
                buckets_info("  K (data): %d", config->erasure.data_shards);
                buckets_info("  M (parity): %d", config->erasure.parity_shards);
            }
        } else {
            buckets_info("Storage directory: /tmp/buckets-data/");
            buckets_info("Running in single-node mode (use --config for clustering)");
            
            /* Initialize storage layer with default config */
            buckets_info("Initializing storage layer...");
            buckets_storage_config_t storage_cfg = {
                .data_dir = "/tmp/buckets-data",
                .inline_threshold = 128 * 1024,  /* 128 KB */
                .default_ec_k = 2,
                .default_ec_m = 2,
                .verify_checksums = true
            };
            if (buckets_storage_init(&storage_cfg) != 0) {
                buckets_error("Failed to initialize storage layer");
                ret = 1;
                goto cleanup;
            }
            buckets_info("Storage layer initialized");
            
            /* Initialize async replication system */
            buckets_info("Initializing async replication (4 workers)...");
            if (async_replication_init(4) != 0) {
                buckets_warn("Failed to initialize async replication, running without it");
            } else {
                buckets_info("Async replication initialized");
            }
        }
        
        /* Initialize credential system */
        const char *data_dir = config ? config->node.data_dir : "/tmp/buckets-data";
        buckets_info("Initializing credential system...");
        if (buckets_credentials_init(data_dir) != BUCKETS_OK) {
            buckets_error("Failed to initialize credential system");
            /* Continue anyway - auth will be disabled */
            buckets_s3_auth_set_enabled(false);
        } else {
            buckets_info("Credential system initialized (%d keys)", buckets_credentials_count());
            buckets_s3_auth_init(true);
        }
        
        buckets_info("Press Ctrl+C to stop");
        
        const char *bind_addr = config ? config->server.bind_address : "0.0.0.0";
        
        /* ===== Check for multi-process mode ===== */
        const char *workers_env = getenv("BUCKETS_WORKERS");
        int num_workers = 0;
        if (workers_env) {
            num_workers = atoi(workers_env);
        }
        
        /* Auto-detect if BUCKETS_WORKERS=auto */
        if (workers_env && strcmp(workers_env, "auto") == 0) {
            num_workers = buckets_http_worker_get_optimal_count();
        }
        
        /* Use multi-process worker pool if requested */
        if (num_workers > 0) {
            buckets_info("==================================================");
            buckets_info("Starting in MULTI-PROCESS mode with %d workers", num_workers);
            buckets_info("Each worker runs independent event loop");
            buckets_info("==================================================");
            
            /* Prepare worker configuration */
            server_worker_config_t worker_cfg = {
                .bind_addr = bind_addr,
                .port = port
            };
            
            /* Start worker pool */
            if (buckets_http_worker_start(num_workers, worker_process_main, &worker_cfg) != 0) {
                buckets_error("Failed to start worker pool");
                if (config) {
                    buckets_storage_cleanup();
                    buckets_registry_cleanup();
                    buckets_topology_manager_cleanup();
                    buckets_multidisk_cleanup();
                    buckets_config_free(config);
                }
                ret = 1;
                goto cleanup;
            }
            
            buckets_info("Worker pool started successfully!");
            buckets_info("S3 API available at: http://%s:%d/", bind_addr, port);
            buckets_info("Running with %d worker processes (SO_REUSEPORT)", num_workers);
            
            /* Master process: monitor workers */
            ret = buckets_http_worker_run();
            
            /* Cleanup after workers exit */
            buckets_info("All workers stopped");
            s3_streaming_cleanup();
            buckets_credentials_cleanup();
            
            if (config) {
                async_replication_shutdown();
                buckets_storage_cleanup();
                buckets_registry_cleanup();
                buckets_topology_manager_cleanup();
                buckets_multidisk_cleanup();
                buckets_config_free(config);
            }
            
            goto cleanup;
        }
        
        /* ===== UV HTTP Server (single-process mode) ===== */
        buckets_info("Using libuv HTTP server with streaming support (single-process)");
        buckets_info("Set BUCKETS_WORKERS=auto for multi-process scaling");
        
        uv_http_server_t *uv_server = uv_http_server_create(bind_addr, port);
        if (!uv_server) {
            buckets_error("Failed to create UV HTTP server");
            if (config) {
                buckets_storage_cleanup();
                buckets_registry_cleanup();
                buckets_topology_manager_cleanup();
                buckets_multidisk_cleanup();
                buckets_config_free(config);
            }
            ret = 1;
            goto cleanup;
        }
        
        /* Register streaming S3 handlers */
        if (s3_streaming_register_handlers(uv_server) != BUCKETS_OK) {
            buckets_error("Failed to register streaming S3 handlers");
            uv_http_server_free(uv_server);
            if (config) {
                buckets_storage_cleanup();
                buckets_registry_cleanup();
                buckets_topology_manager_cleanup();
                buckets_multidisk_cleanup();
                buckets_config_free(config);
            }
            ret = 1;
            goto cleanup;
        }
        
        /* Start server */
        if (uv_http_server_start(uv_server) != BUCKETS_OK) {
            buckets_error("Failed to start UV HTTP server");
            uv_http_server_free(uv_server);
            if (config) {
                buckets_storage_cleanup();
                buckets_registry_cleanup();
                buckets_topology_manager_cleanup();
                buckets_multidisk_cleanup();
                buckets_config_free(config);
            }
            ret = 1;
            goto cleanup;
        }
        
        buckets_info("Server started successfully!");
        buckets_info("S3 API available at: http://localhost:%d/", port);
        buckets_info("");
        buckets_info("Streaming mode enabled - large uploads are processed without full buffering");
        buckets_info("");
        buckets_info("Example commands:");
        buckets_info("  # Upload large file (streaming)");
        buckets_info("  curl -X PUT --data-binary @bigfile.dat http://localhost:%d/my-bucket/bigfile.dat", port);
        buckets_info("");
        buckets_info("Server is running. Press Ctrl+C to stop...");
        
        /* Keep server running */
        while (1) {
            sleep(1);
        }
        
        /* Cleanup (reached on Ctrl+C) */
        buckets_info("Shutting down server...");
        uv_http_server_stop(uv_server);
        uv_http_server_free(uv_server);
        s3_streaming_cleanup();
        
        /* Cleanup credentials */
        buckets_info("Cleaning up credential system...");
        buckets_credentials_cleanup();
        
        if (config) {
            buckets_info("Shutting down async replication...");
            async_replication_shutdown();
            buckets_info("Cleaning up storage layer...");
            buckets_storage_cleanup();
            buckets_info("Cleaning up registry...");
            buckets_registry_cleanup();
            buckets_info("Cleaning up topology manager...");
            buckets_topology_manager_cleanup();
            buckets_info("Cleaning up multi-disk storage...");
            buckets_multidisk_cleanup();
            buckets_config_free(config);
        }
        buckets_info("Server stopped");
        
    } else if (strcmp(command, "format") == 0) {
        print_banner();
        
        /* Parse options */
        const char *config_file = NULL;
        bool force = false;
        
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--config") == 0) {
                if (i + 1 < argc) {
                    config_file = argv[++i];
                } else {
                    fprintf(stderr, "Error: --config requires a file path\n\n");
                    ret = 1;
                    goto cleanup;
                }
            } else if (strcmp(argv[i], "--force") == 0) {
                force = true;
            } else {
                fprintf(stderr, "Error: unknown option: %s\n\n", argv[i]);
                print_usage(argv[0]);
                ret = 1;
                goto cleanup;
            }
        }
        
        /* Config file is required for format */
        if (!config_file) {
            fprintf(stderr, "Error: --config is required for format command\n\n");
            print_usage(argv[0]);
            ret = 1;
            goto cleanup;
        }
        
        buckets_info("Formatting disks from configuration: %s", config_file);
        if (force) {
            buckets_warn("Force formatting enabled - existing data will be destroyed!");
        }
        
        /* Format disks */
        ret = format_disks_from_config(config_file, force);
        
        if (ret == 0) {
            buckets_info("Disk formatting completed successfully");
        } else {
            buckets_error("Disk formatting failed");
        }
        
    } else if (strcmp(command, "creds") == 0) {
        /* Credential management commands */
        const char *data_dir = "/tmp/buckets-data";  /* Default data dir */
        
        /* Check for --data-dir option */
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
                data_dir = argv[++i];
            }
        }
        
        /* Initialize credential system */
        if (buckets_credentials_init(data_dir) != BUCKETS_OK) {
            fprintf(stderr, "Error: Failed to initialize credential system\n");
            ret = 1;
            goto cleanup;
        }
        
        if (argc < 3) {
            fprintf(stderr, "Error: Missing subcommand for creds\n\n");
            printf("Usage: %s creds <subcommand> [options]\n\n", argv[0]);
            printf("Subcommands:\n");
            printf("  list              List all credentials\n");
            printf("  create [--name <name>] [--policy <policy>]\n");
            printf("                    Create new credential\n");
            printf("  delete <key>      Delete credential by access key\n");
            printf("  enable <key>      Enable a credential\n");
            printf("  disable <key>     Disable a credential\n");
            printf("\n");
            printf("Options:\n");
            printf("  --data-dir <dir>  Data directory (default: /tmp/buckets-data)\n");
            printf("  --name <name>     Name/description for new credential\n");
            printf("  --policy <policy> Policy: readwrite, readonly, writeonly (default: readwrite)\n");
            printf("\n");
            buckets_credentials_cleanup();
            ret = 1;
            goto cleanup;
        }
        
        const char *subcmd = argv[2];
        
        if (strcmp(subcmd, "list") == 0) {
            /* List all credentials */
            char *json = buckets_credentials_list();
            if (json) {
                printf("%s\n", json);
                buckets_free(json);
            } else {
                fprintf(stderr, "Error: Failed to list credentials\n");
                ret = 1;
            }
            
        } else if (strcmp(subcmd, "create") == 0) {
            /* Create new credential */
            const char *name = NULL;
            const char *policy = "readwrite";
            
            for (int i = 3; i < argc; i++) {
                if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
                    name = argv[++i];
                } else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) {
                    policy = argv[++i];
                }
            }
            
            char access_key[64];
            char secret_key[64];
            
            if (buckets_credentials_create(name, policy, access_key, sizeof(access_key),
                                            secret_key, sizeof(secret_key)) == BUCKETS_OK) {
                printf("{\n");
                printf("  \"access_key\": \"%s\",\n", access_key);
                printf("  \"secret_key\": \"%s\",\n", secret_key);
                printf("  \"name\": \"%s\",\n", name ? name : "");
                printf("  \"policy\": \"%s\"\n", policy);
                printf("}\n");
                printf("\n");
                printf("IMPORTANT: Save the secret key now! It cannot be retrieved later.\n");
            } else {
                fprintf(stderr, "Error: Failed to create credential\n");
                ret = 1;
            }
            
        } else if (strcmp(subcmd, "delete") == 0) {
            if (argc < 4) {
                fprintf(stderr, "Error: Missing access key\n");
                fprintf(stderr, "Usage: %s creds delete <access_key>\n", argv[0]);
                ret = 1;
            } else {
                const char *access_key = argv[3];
                if (buckets_credentials_delete(access_key) == BUCKETS_OK) {
                    printf("Deleted credential: %s\n", access_key);
                } else {
                    fprintf(stderr, "Error: Failed to delete credential (not found?)\n");
                    ret = 1;
                }
            }
            
        } else if (strcmp(subcmd, "enable") == 0) {
            if (argc < 4) {
                fprintf(stderr, "Error: Missing access key\n");
                fprintf(stderr, "Usage: %s creds enable <access_key>\n", argv[0]);
                ret = 1;
            } else {
                const char *access_key = argv[3];
                if (buckets_credentials_set_enabled(access_key, true) == BUCKETS_OK) {
                    printf("Enabled credential: %s\n", access_key);
                } else {
                    fprintf(stderr, "Error: Failed to enable credential (not found?)\n");
                    ret = 1;
                }
            }
            
        } else if (strcmp(subcmd, "disable") == 0) {
            if (argc < 4) {
                fprintf(stderr, "Error: Missing access key\n");
                fprintf(stderr, "Usage: %s creds disable <access_key>\n", argv[0]);
                ret = 1;
            } else {
                const char *access_key = argv[3];
                if (buckets_credentials_set_enabled(access_key, false) == BUCKETS_OK) {
                    printf("Disabled credential: %s\n", access_key);
                } else {
                    fprintf(stderr, "Error: Failed to disable credential (not found?)\n");
                    ret = 1;
                }
            }
            
        } else {
            fprintf(stderr, "Error: Unknown subcommand: %s\n", subcmd);
            fprintf(stderr, "Use '%s creds' for help\n", argv[0]);
            ret = 1;
        }
        
        buckets_credentials_cleanup();
        
    } else if (strcmp(command, "debug") == 0) {
        /* Debug instrumentation commands */
        if (argc < 3) {
            fprintf(stderr, "Error: Missing subcommand for debug\n\n");
            printf("Usage: %s debug <subcommand>\n\n", argv[0]);
            printf("Subcommands:\n");
            printf("  enable    Enable debug instrumentation\n");
            printf("  disable   Disable debug instrumentation\n");
            printf("  stats     Print debug statistics\n");
            printf("  reset     Reset debug statistics\n");
            printf("\n");
            ret = 1;
            goto cleanup;
        }
        
        const char *subcmd = argv[2];
        
        if (strcmp(subcmd, "enable") == 0) {
            buckets_debug_set_enabled(true);
            printf("Debug instrumentation ENABLED\n");
            printf("Logs will now include detailed timing and resource tracking\n");
            
        } else if (strcmp(subcmd, "disable") == 0) {
            buckets_debug_set_enabled(false);
            printf("Debug instrumentation DISABLED\n");
            
        } else if (strcmp(subcmd, "stats") == 0) {
            buckets_debug_print_stats();
            
        } else if (strcmp(subcmd, "reset") == 0) {
            buckets_debug_reset_stats();
            printf("Debug statistics reset\n");
            
        } else {
            fprintf(stderr, "Error: Unknown subcommand: %s\n", subcmd);
            fprintf(stderr, "Use '%s debug' for help\n", argv[0]);
            ret = 1;
        }
        
    } else {
        fprintf(stderr, "Error: unknown command: %s\n\n", command);
        print_usage(argv[0]);
        ret = 1;
    }

cleanup:
    buckets_cleanup();
    return ret;
}
