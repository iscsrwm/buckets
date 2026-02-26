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
    printf("Examples:\n");
    printf("  %s server                           # Start on default port 9000\n", progname);
    printf("  %s server --port 8080               # Start on port 8080\n", progname);
    printf("  %s server --config config/node1.json  # Start with config file\n", progname);
    printf("  %s format --config config/node1.json  # Format disks for node1\n", progname);
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
    
    buckets_format_t *format = buckets_format_new(sets, disks_per_set);
    if (!format) {
        buckets_error("Failed to create format structure");
        buckets_config_free(config);
        return 1;
    }
    
    buckets_info("Created format with deployment_id: %s", format->meta.deployment_id);
    
    /* Assign disk UUIDs and save format to each disk */
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
    buckets_info("Disks formatted: %d", disk_idx);
    
    /* Cleanup */
    buckets_topology_free(topology);
    buckets_format_free(format);
    buckets_config_free(config);
    
    return 0;
}

int main(int argc, char *argv[]) {
    int ret = 0;

    /* Initialize logging first */
    buckets_log_init();

    /* Initialize Buckets */
    if (buckets_init() != 0) {
        fprintf(stderr, "Failed to initialize Buckets\n");
        return 1;
    }

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
                    
                    /* Initialize storage layer */
                    buckets_info("Initializing storage layer...");
                    buckets_storage_config_t storage_cfg = {
                        .data_dir = config->node.data_dir,
                        .inline_threshold = 128 * 1024,  /* 128 KB */
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
        }
        buckets_info("Press Ctrl+C to stop");
        
        /* Create HTTP server */
        const char *bind_addr = config ? config->server.bind_address : "0.0.0.0";
        buckets_http_server_t *server = buckets_http_server_create(bind_addr, port);
        if (!server) {
            buckets_error("Failed to create HTTP server");
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
        
        /* Set S3 handler as default handler for all requests */
        if (buckets_http_server_set_default_handler(server, buckets_s3_handler, NULL) != BUCKETS_OK) {
            buckets_error("Failed to set S3 handler");
            buckets_http_server_free(server);
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
        if (buckets_http_server_start(server) != BUCKETS_OK) {
            buckets_error("Failed to start HTTP server");
            buckets_http_server_free(server);
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
        buckets_info("Example commands:");
        buckets_info("  # List buckets");
        buckets_info("  curl -v http://localhost:%d/", port);
        buckets_info("");
        buckets_info("  # Create bucket");
        buckets_info("  curl -v -X PUT http://localhost:%d/my-bucket", port);
        buckets_info("");
        buckets_info("  # Upload object");
        buckets_info("  echo 'Hello World' | curl -v -X PUT --data-binary @- http://localhost:%d/my-bucket/hello.txt", port);
        buckets_info("");
        buckets_info("  # Download object");
        buckets_info("  curl -v http://localhost:%d/my-bucket/hello.txt", port);
        buckets_info("");
        buckets_info("Server is running. Press Ctrl+C to stop...");
        
        /* Keep server running - in a real implementation, we'd have a signal handler */
        /* For now, just loop forever - the HTTP server polls internally */
        while (1) {
            /* Server runs in mongoose polling loop */
            /* User can stop with Ctrl+C which will cleanup automatically */
            sleep(1);
        }
        
        /* Cleanup (reached on Ctrl+C) */
        buckets_info("Shutting down server...");
        buckets_http_server_stop(server);
        buckets_http_server_free(server);
        if (config) {
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
        
    } else {
        fprintf(stderr, "Unknown command: %s\n\n", command);
        print_usage(argv[0]);
        ret = 1;
    }

cleanup:
    buckets_cleanup();
    return ret;
}
