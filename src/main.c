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
    printf("  server [port]       Start S3 API server (default port: 9000)\n");
    printf("  version             Print version information\n");
    printf("  help                Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s server           # Start on default port 9000\n", progname);
    printf("  %s server 8080      # Start on port 8080\n", progname);
    printf("\n");
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
        
        /* Parse options: port (default 9000) */
        int port = 9000;
        if (argc >= 3) {
            port = atoi(argv[2]);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "Error: invalid port number: %s\n\n", argv[2]);
                ret = 1;
                goto cleanup;
            }
        }

        buckets_info("Starting Buckets S3 server on port %d...", port);
        buckets_info("Storage directory: /tmp/buckets-data/");
        buckets_info("Press Ctrl+C to stop");
        
        /* Create HTTP server */
        char addr[32];
        snprintf(addr, sizeof(addr), "0.0.0.0:%d", port);
        buckets_http_server_t *server = buckets_http_server_create(addr, port);
        if (!server) {
            buckets_error("Failed to create HTTP server");
            ret = 1;
            goto cleanup;
        }
        
        /* Set S3 handler as default handler for all requests */
        if (buckets_http_server_set_default_handler(server, buckets_s3_handler, NULL) != BUCKETS_OK) {
            buckets_error("Failed to set S3 handler");
            buckets_http_server_free(server);
            ret = 1;
            goto cleanup;
        }
        
        /* Start server */
        if (buckets_http_server_start(server) != BUCKETS_OK) {
            buckets_error("Failed to start HTTP server");
            buckets_http_server_free(server);
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
        buckets_info("Server stopped");
        
    } else {
        fprintf(stderr, "Unknown command: %s\n\n", command);
        print_usage(argv[0]);
        ret = 1;
    }

cleanup:
    buckets_cleanup();
    return ret;
}
