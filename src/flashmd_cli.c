/*
 * FlashMD CLI Frontend
 * Sega Genesis/Mega Drive ROM Flasher - Command Line Interface
 */

#include "flashmd_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
#endif

static void sigint_handler(int sig) {
    (void)sig;
    flashmd_set_interrupted(1);
    printf("\nInterrupted!\n");
}

static void print_usage(const char *progname) {
    printf("flashmd thingy\n\n");
    printf("Usage:\n");
    printf("  %s [options] <command>\n\n", progname);
    printf("Options:\n");
    printf("  -v, --verbose            Verbose mode - show all firmware messages\n");
    printf("  -s, --size <KB>          Size in kilobytes (for erase, read, write)\n");
    printf("                           Use 0 for auto-detect (read) or full erase\n");
    printf("  -n, --no-trim            Don't trim trailing 0xFF bytes (read only)\n");
    printf("                           File will be exactly the specified size\n\n");
    printf("Commands:\n");
    printf("  -r, --read <file>        Read ROM to file (use -s for size, 0=auto)\n");
    printf("  -w, --write <file>       Write ROM file to flash (use -s to limit size)\n");
    printf("  -e, --erase              Erase flash (use -s for size, 0=full)\n");
    printf("  connect                  Test connection to device\n");
    printf("  id                       Read flash chip ID\n");
    printf("  clear                    Clear device buffer\n\n");
    printf("Examples:\n");
    printf("  %s -e -s 1024            Erase 1MB (1024 KB)\n", progname);
    printf("  %s -w original.bin      Write file (uses file size)\n", progname);
    printf("  %s -w original.bin -s 768  Write 768 KB from file\n", progname);
    printf("  %s -r dump.bin -s 768    Read 768 KB to file (trimmed)\n", progname);
    printf("  %s -r dump.bin -s 1024 -n  Read 1MB, no trim (exactly 1MB)\n", progname);
    printf("  %s -r dump.bin -s 0      Auto-detect size (read 4MB and trim)\n", progname);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);

    /* Get real user ID (the user who ran sudo) - Unix only */
#ifndef _WIN32
    const char *sudo_uid_str = getenv("SUDO_UID");
    const char *sudo_gid_str = getenv("SUDO_GID");

    if (sudo_uid_str && sudo_gid_str) {
        flashmd_set_real_ids(atoi(sudo_uid_str), atoi(sudo_gid_str));
    } else {
        flashmd_set_real_ids(getuid(), getgid());
    }
#else
    /* On Windows, file ownership is handled differently, so we can skip this */
    flashmd_set_real_ids(-1, -1);
#endif

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* Parse arguments */
    int do_read = 0, do_write = 0, do_erase = 0;
    const char *read_file = NULL;
    const char *write_file = NULL;
    uint32_t size_kb = 0;
    int no_trim = 0;
    int verbose = 0;
    const char *legacy_command = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        }
        else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--size") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -s requires a size value\n");
                return 1;
            }
            size_kb = (uint32_t)atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--read") == 0) {
            do_read = 1;
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -r requires a filename\n");
                return 1;
            }
            read_file = argv[++i];
        }
        else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--write") == 0) {
            do_write = 1;
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -w requires a filename\n");
                return 1;
            }
            write_file = argv[++i];
        }
        else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--erase") == 0) {
            do_erase = 1;
        }
        else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no-trim") == 0) {
            no_trim = 1;
        }
        else if (argv[i][0] != '-') {
            if (!legacy_command) {
                legacy_command = argv[i];
            }
        }
    }

    /* Initialize config */
    flashmd_config_t config;
    flashmd_config_init(&config);
    config.verbose = verbose;
    config.no_trim = no_trim;
    /* Use NULL callbacks = default to printf */

    /* Support legacy command format */
    if (legacy_command && !do_read && !do_write && !do_erase) {
        flashmd_result_t r = flashmd_open();
        if (r != FLASHMD_OK) {
            fprintf(stderr, "Could not open USB device: %s\n", flashmd_error_string(r));
            return 1;
        }

        flashmd_result_t result = FLASHMD_OK;
        if (strcmp(legacy_command, "connect") == 0) {
            result = flashmd_connect(&config);
        }
        else if (strcmp(legacy_command, "id") == 0) {
            result = flashmd_check_id(&config);
        }
        else if (strcmp(legacy_command, "clear") == 0) {
            result = flashmd_clear_buffer(&config);
        }
        else {
            fprintf(stderr, "Unknown command: %s\n", legacy_command);
            print_usage(argv[0]);
            flashmd_close();
            return 1;
        }
        flashmd_close();
        return (result == FLASHMD_OK) ? 0 : 1;
    }

    /* Validate that exactly one action is specified */
    int action_count = (do_read ? 1 : 0) + (do_write ? 1 : 0) + (do_erase ? 1 : 0);
    if (action_count == 0) {
        fprintf(stderr, "Error: No action specified. Use -r, -w, or -e\n");
        print_usage(argv[0]);
        return 1;
    }
    if (action_count > 1) {
        fprintf(stderr, "Error: Only one action (-r, -w, or -e) can be specified\n");
        return 1;
    }

    flashmd_result_t r = flashmd_open();
    if (r != FLASHMD_OK) {
        fprintf(stderr, "Could not open USB device: %s\n", flashmd_error_string(r));
        return 1;
    }

    flashmd_result_t result = FLASHMD_OK;

    if (do_erase) {
        result = flashmd_erase(size_kb, &config);
    }
    else if (do_read) {
        if (!read_file) {
            fprintf(stderr, "Error: -r requires a filename\n");
            result = FLASHMD_ERR_INVALID_PARAM;
        } else {
            result = flashmd_read_rom(read_file, size_kb, &config);
        }
    }
    else if (do_write) {
        if (!write_file) {
            fprintf(stderr, "Error: -w requires a filename\n");
            result = FLASHMD_ERR_INVALID_PARAM;
        } else {
            result = flashmd_write_rom(write_file, size_kb, &config);
        }
    }

    flashmd_close();
    return (result == FLASHMD_OK) ? 0 : 1;
}
