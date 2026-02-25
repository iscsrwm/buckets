/**
 * Buckets - Main Entry Point
 * 
 * High-performance S3-compatible object storage server
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "buckets.h"

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
    printf("  server <endpoints>  Start object storage server\n");
    printf("  version             Print version information\n");
    printf("  help                Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s server /data\n", progname);
    printf("  %s server http://node{1...4}:9000/data{1...4}\n", progname);
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
        
        if (argc < 3) {
            fprintf(stderr, "Error: server command requires endpoints\n\n");
            print_usage(argv[0]);
            ret = 1;
            goto cleanup;
        }

        buckets_info("Starting Buckets server...");
        buckets_info("Endpoints: %s", argv[2]);
        
        /* TODO: Implement server logic */
        buckets_warn("Server implementation pending");
        buckets_info("Project structure initialized");
        buckets_info("Ready to begin development");
        
    } else {
        fprintf(stderr, "Unknown command: %s\n\n", command);
        print_usage(argv[0]);
        ret = 1;
    }

cleanup:
    buckets_cleanup();
    return ret;
}
