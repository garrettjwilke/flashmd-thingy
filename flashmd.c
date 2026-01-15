/*
 * Sega Genesis/Mega Drive ROM Flasher
 * Uses libusb for reliable USB CDC communication
 *
 * Compile: gcc -o flashmd flashmd.c -lusb-1.0 -Wall
 * macOS:   gcc -o flashmd flashmd.c -I/opt/homebrew/include -L/opt/homebrew/lib -lusb-1.0 -Wall
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <libusb-1.0/libusb.h>

/* USB device identifiers */
#define VENDOR_ID   0x0483  /* STMicroelectronics */
#define PRODUCT_ID  0x5740  /* Virtual COM Port */

/* USB endpoints for CDC */
#define EP_OUT      0x01
#define EP_IN       0x81
#define CDC_IFACE   1
#define TIMEOUT_MS  1000

/* Command codes */
#define CMD_READ_ROM      0x0A
#define CMD_WRITE_ROM     0x0B
#define CMD_CONNECT       0x0C
#define CMD_CHECK_ID      0x0D
#define CMD_FULL_ERASE    0x0E
#define CMD_CLEAR_BUFFER  0x0F
#define CMD_READ_SRAM     0x1A
#define CMD_WRITE_SRAM    0x1B
#define CMD_SECTOR_ERASE  0x1E

/* Magic bytes for command packets */
#define MAGIC_1 0xAA
#define MAGIC_2 0x55
#define MAGIC_3 0xAA
#define MAGIC_4 0xBB

/* Sizes */
#define CMD_PACKET_SIZE   64
#define DATA_CHUNK_SIZE   1024

/* Timing configuration (adjust these for speed optimization) */
#define WRITE_DELAY_US    1000   /* Delay after sending data chunk (microseconds) - try 1000-10000 */
#define POLL_INTERVAL_MS  30     /* Polling interval for reading responses (milliseconds) - try 1-50 */
#define CLEANUP_DELAY_US  100000 /* Delay after finding end pattern in read_until_complete (microseconds) */

/* Message filtering configuration */
/* Add firmware messages here that you want filtered out by default */
/* Use -v flag to show all messages including filtered ones */
static const char *filtered_messages[] = {
    "BUFF IS CLEAR",
    "ROM DUMP START!!!",
    "DUMPER ROM FINISH!!!",
    "PUSH SAVE GAME BUTTON!!!",
    NULL  /* End marker */
};

/* Global verbose flag - set to 1 with -v to show filtered messages */
static int verbose_mode = 0;

/* Size codes */
typedef enum {
    SIZE_512K = 0x01,
    SIZE_1M   = 0x02,
    SIZE_2M   = 0x03,
    SIZE_4M   = 0x04,
    SIZE_8M   = 0x05
} rom_size_t;

/* Global libusb handles */
static libusb_context *ctx = NULL;
static libusb_device_handle *dev_handle = NULL;

static volatile int interrupted = 0;

/* Store real user ID (the user who ran sudo) */
static uid_t real_uid = -1;
static gid_t real_gid = -1;

/*
 * Change file ownership to the real user (who ran sudo)
 * Can be called with either a file descriptor (before closing) or filename (after closing)
 */
static void fix_file_ownership_fd(int fd) {
    if (real_uid != (uid_t)-1 && real_gid != (gid_t)-1 && fd >= 0) {
        if (fchown(fd, real_uid, real_gid) < 0) {
            /* Non-fatal: silently ignore if we can't change ownership */
        }
    }
}

static void fix_file_ownership(const char *filename) {
    if (real_uid != (uid_t)-1 && real_gid != (gid_t)-1) {
        if (chown(filename, real_uid, real_gid) < 0) {
            /* Non-fatal: silently ignore if we can't change ownership */
        }
    }
}

void sigint_handler(int sig) {
    (void)sig;
    interrupted = 1;
    printf("\nInterrupted!\n");
}

/*
 * Open USB device
 */
int usb_open(void) {
    int r = libusb_init(&ctx);
    if (r < 0) {
        fprintf(stderr, "Failed to initialize libusb: %s\n", libusb_strerror(r));
        return -1;
    }

    dev_handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
    if (!dev_handle) {
        fprintf(stderr, "Could not find USB device %04x:%04x\n", VENDOR_ID, PRODUCT_ID);
        fprintf(stderr, "Make sure the device is connected and you have permissions.\n");
        libusb_exit(ctx);
        ctx = NULL;
        return -1;
    }

    /* Detach kernel driver if attached (required on macOS/Linux) */
    if (libusb_kernel_driver_active(dev_handle, CDC_IFACE) == 1) {
        r = libusb_detach_kernel_driver(dev_handle, CDC_IFACE);
        if (r < 0 && r != LIBUSB_ERROR_NOT_SUPPORTED) {
            fprintf(stderr, "Warning: Could not detach kernel driver: %s\n", libusb_strerror(r));
        }
    }

    /* Claim the CDC data interface */
    r = libusb_claim_interface(dev_handle, CDC_IFACE);
    if (r < 0) {
        fprintf(stderr, "Could not claim USB interface: %s\n", libusb_strerror(r));
        libusb_close(dev_handle);
        libusb_exit(ctx);
        dev_handle = NULL;
        ctx = NULL;
        return -1;
    }

    return 0;
}

void usb_close(void) {
    if (dev_handle) {
        libusb_release_interface(dev_handle, CDC_IFACE);
        libusb_close(dev_handle);
        dev_handle = NULL;
    }
    if (ctx) {
        libusb_exit(ctx);
        ctx = NULL;
    }
}

/*
 * Write data to USB device
 */
int usb_write(const uint8_t *data, int len) {
    int transferred = 0;
    int r = libusb_bulk_transfer(dev_handle, EP_OUT, (uint8_t *)data, len, &transferred, TIMEOUT_MS);
    if (r < 0) {
        fprintf(stderr, "USB write error: %s\n", libusb_strerror(r));
        return -1;
    }
    return transferred;
}

/*
 * Read data from USB device
 */
int usb_read(uint8_t *buf, int max_len, int timeout_ms) {
    int transferred = 0;
    int r = libusb_bulk_transfer(dev_handle, EP_IN, buf, max_len, &transferred, timeout_ms);
    if (r == LIBUSB_ERROR_TIMEOUT) {
        return 0;  /* Timeout, no data */
    }
    if (r < 0) {
        fprintf(stderr, "USB read error: %s\n", libusb_strerror(r));
        return -1;
    }
    return transferred;
}

/*
 * Send a command packet
 */
int send_command(uint8_t cmd, const uint8_t *params, size_t param_len) {
    uint8_t packet[CMD_PACKET_SIZE] = {0};

    packet[0] = cmd;
    packet[1] = MAGIC_1;
    packet[2] = MAGIC_2;
    packet[3] = MAGIC_3;
    packet[4] = MAGIC_4;

    if (params && param_len > 0) {
        if (param_len > CMD_PACKET_SIZE - 5) {
            param_len = CMD_PACKET_SIZE - 5;
        }
        memcpy(&packet[5], params, param_len);
    }

    return usb_write(packet, CMD_PACKET_SIZE);
}

/*
 * Check if a message should be filtered (suppressed unless verbose mode)
 */
static int should_filter_message(const char *msg) {
    if (verbose_mode) {
        return 0;  /* Don't filter in verbose mode */
    }
    
    if (!msg || !msg[0]) {
        return 0;
    }
    
    /* Check against filter list */
    for (int i = 0; filtered_messages[i] != NULL; i++) {
        if (strstr(msg, filtered_messages[i]) != NULL) {
            return 1;  /* Filter this message */
        }
    }
    
    return 0;  /* Don't filter */
}

/*
 * Print filtered output (suppresses filtered messages unless verbose)
 */
static void print_filtered(const char *data, size_t len) {
    if (!data || len == 0) return;
    
    /* Create a null-terminated copy for filtering */
    char *temp = malloc(len + 1);
    if (!temp) {
        fwrite(data, 1, len, stdout);
        return;
    }
    memcpy(temp, data, len);
    temp[len] = '\0';
    
    /* Check if entire message should be filtered */
    if (!should_filter_message(temp)) {
        fwrite(data, 1, len, stdout);
    }
    
    free(temp);
}

/*
 * Read text response until newline or timeout
 */
int read_response(char *buf, size_t max_len, int timeout_ms) {
    size_t total = 0;
    int elapsed = 0;
    int poll_interval = POLL_INTERVAL_MS;

    while (total < max_len - 1 && elapsed < timeout_ms) {
        uint8_t temp[256];
        int n = usb_read(temp, sizeof(temp), poll_interval);
        if (n < 0) return -1;
        if (n > 0) {
            size_t to_copy = n;
            if (total + to_copy >= max_len) {
                to_copy = max_len - 1 - total;
            }
            memcpy(buf + total, temp, to_copy);
            total += to_copy;

            /* Check for end of response */
            if (total > 0 && buf[total-1] == '\n') {
                break;
            }
            elapsed = 0;  /* Reset timeout on data received */
        } else {
            elapsed += poll_interval;
        }
    }

    buf[total] = '\0';
    return (int)total;
}

/*
 * Read and print responses until pattern found or timeout
 */
int read_until_complete(const char *end_pattern, int timeout_ms) {
    char buf[4096];
    size_t acc_len = 0;
    int elapsed = 0;
    int poll_interval = POLL_INTERVAL_MS;

    while (elapsed < timeout_ms) {
        uint8_t temp[512];
        int n = usb_read(temp, sizeof(temp), poll_interval);
        if (n < 0) return -1;
        if (n > 0) {
            /* Print immediately (with filtering) */
            print_filtered((const char *)temp, n);
            fflush(stdout);

            /* Accumulate for pattern matching */
            if (acc_len + n < sizeof(buf) - 1) {
                memcpy(buf + acc_len, temp, n);
                acc_len += n;
                buf[acc_len] = '\0';
            }

            if (strstr(buf, end_pattern)) {
                /* Read any trailing data */
                usleep(CLEANUP_DELAY_US);
                while ((n = usb_read(temp, sizeof(temp), 100)) > 0) {
                    print_filtered((const char *)temp, n);
                }
                return 0;
            }
            elapsed = 0;
        } else {
            elapsed += poll_interval;
        }
    }

    fprintf(stderr, "\nTimeout waiting for response\n");
    return -1;
}

/*
 * Read binary data
 */
int read_binary(uint8_t *buf, size_t len, int timeout_ms) {
    size_t total = 0;
    int elapsed = 0;
    int poll_interval = POLL_INTERVAL_MS;

    while (total < len && elapsed < timeout_ms) {
        int n = usb_read(buf + total, len - total, poll_interval);
        if (n < 0) return -1;
        if (n > 0) {
            total += n;
            elapsed = 0;
        } else {
            elapsed += poll_interval;
        }
    }

    if (total < len) {
        fprintf(stderr, "Timeout: got %zu of %zu bytes\n", total, len);
        return -1;
    }
    return (int)total;
}

/*
 * Read and print all responses
 */
void read_all_responses(int timeout_ms) {
    uint8_t buf[512];
    int elapsed = 0;
    int poll_interval = POLL_INTERVAL_MS;

    while (elapsed < timeout_ms) {
        int n = usb_read(buf, sizeof(buf), poll_interval);
        if (n > 0) {
            print_filtered((const char *)buf, n);
            fflush(stdout);
            elapsed = 0;
        } else {
            elapsed += poll_interval;
        }
    }
}

/* Command implementations */

/* Forward declarations */
int cmd_connect(void);
int cmd_check_id(void);
int cmd_clear_buffer(void);

/*
 * Convert size in KB to device size code
 * Returns size code or 0 if invalid
 */
static uint8_t kb_to_size_code(uint32_t kb) {
    if (kb <= 512) return SIZE_512K;
    if (kb <= 1024) return SIZE_1M;
    if (kb <= 2048) return SIZE_2M;
    if (kb <= 4096) return SIZE_4M;
    return SIZE_8M;  /* For anything larger, use 8M */
}

/*
 * Get device bytes for a given size code
 */
static uint32_t size_code_to_bytes(uint8_t size_code) {
    switch (size_code) {
        case SIZE_512K: return 512 * 1024;
        case SIZE_1M:   return 1024 * 1024;
        case SIZE_2M:   return 2 * 1024 * 1024;
        case SIZE_4M:   return 4 * 1024 * 1024;
        case SIZE_8M:   return 8 * 1024 * 1024;
        default: return 0;
    }
}

/*
 * Initialize device: connect, check ID, and clear buffer
 * Call this before read/write operations to ensure clean state
 */
int device_init(void) {
    if (cmd_connect() < 0) {
        fprintf(stderr, "Failed to connect to device\n");
        return -1;
    }

    /* Wait a bit for device to be ready */
    usleep(100000);  /* 100ms */

    if (cmd_check_id() < 0) {
        fprintf(stderr, "Failed to read device ID\n");
        return -1;
    }

    /* Wait a bit for device to be ready */
    usleep(100000);  /* 100ms */

    if (cmd_clear_buffer() < 0) {
        fprintf(stderr, "Failed to clear device buffer\n");
        return -1;
    }

    return 0;
}

int cmd_connect(void) {
    printf("Connecting to FlashMaster MD Dumper...\n");

    if (send_command(CMD_CONNECT, NULL, 0) < 0) return -1;

    char response[256];
    if (read_response(response, sizeof(response), 2000) > 0) {
        if (!should_filter_message(response)) {
            printf("%s", response);
        }
        if (strstr(response, "connected")) {
            printf("Connection successful!\n");
            return 0;
        }
    }

    fprintf(stderr, "No response from device\n");
    return -1;
}

int cmd_check_id(void) {
    printf("Reading flash chip ID...\n");
    if (send_command(CMD_CHECK_ID, NULL, 0) < 0) return -1;
    read_all_responses(3000);
    return 0;
}

int cmd_clear_buffer(void) {
    printf("Clearing device buffer...\n");
    if (send_command(CMD_CLEAR_BUFFER, NULL, 0) < 0) return -1;
    read_all_responses(2000);
    return 0;
}

int cmd_erase(uint32_t size_kb) {
    /* Initialize device before erase */
    if (device_init() < 0) {
        return -1;
    }

    /* size_kb = 0 means full erase */
    if (size_kb == 0) {
        printf("Performing full chip erase (this may take 1-2 minutes)...\n");
        if (send_command(CMD_FULL_ERASE, NULL, 0) < 0) return -1;
        read_until_complete("SRAM ERASE FINISH", 3000);
        return 0;
    }

    uint8_t size_code = kb_to_size_code(size_kb);
    uint32_t erase_bytes = size_code_to_bytes(size_code);
    
    printf("Erasing %u KB (using %u KB sector)...\n", size_kb, erase_bytes / 1024);
    uint8_t params[1] = {size_code};
    if (send_command(CMD_SECTOR_ERASE, params, 1) < 0) return -1;
    read_until_complete("ERASE OK", 5000);
    return 0;
}

/*
 * Trim trailing 0xFF bytes from a file
 */
long trim_rom_file(const char *filename) {
    FILE *fp = fopen(filename, "r+b");
    if (!fp) {
        perror("trim_rom_file: Error opening file");
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        perror("trim_rom_file: Error seeking to end of file");
        fclose(fp);
        return -1;
    }

    long file_size = ftell(fp);
    if (file_size < 0) {
        perror("trim_rom_file: Error getting file size");
        fclose(fp);
        return -1;
    }

    if (file_size == 0) {
        fclose(fp);
        return 0;
    }

    long new_size = file_size;

    #define TRIM_BUFFER_SIZE 4096
    uint8_t buffer[TRIM_BUFFER_SIZE];
    long pos = file_size;

    while (pos > 0) {
        long seek_to = pos - TRIM_BUFFER_SIZE;
        size_t chunk_size = TRIM_BUFFER_SIZE;
        if (seek_to < 0) {
            chunk_size = pos;
            seek_to = 0;
        }

        if (fseek(fp, seek_to, SEEK_SET) != 0) {
            perror("trim_rom_file: Error seeking in file");
            fclose(fp);
            return -1;
        }

        size_t bytes_read = fread(buffer, 1, chunk_size, fp);
        if (bytes_read < chunk_size && ferror(fp)) {
            perror("trim_rom_file: Error reading file");
            fclose(fp);
            return -1;
        }

        if (bytes_read == 0) {
            break;
        }

        for (long i = bytes_read - 1; i >= 0; i--) {
            if (buffer[i] != 0xFF) {
                new_size = seek_to + i + 1;
                goto end_trim_loop;
            }
        }
        pos = seek_to;
    }

    if (pos == 0) { // File is all 0xFFs
        new_size = 0;
    }

end_trim_loop:
    if (new_size < file_size) {
        if (ftruncate(fileno(fp), new_size) != 0) {
            perror("\nFailed to trim file");
        } else {
            printf("\nROM file trimmed to %ld bytes.", new_size);
        }
    } else {
        printf("\nROM file has no trailing 0xFF padding.");
    }
    printf("\n");

    fclose(fp);
    return new_size;
}

int cmd_read_rom(uint32_t size_kb, const char *filename, int no_trim) {
    uint8_t size_code;
    uint32_t total_bytes, device_bytes;

    /* size_kb = 0 means auto-detect */
    if (size_kb == 0) {
        size_code = SIZE_4M; 
        total_bytes = 4*1024*1024; 
        device_bytes = 4*1024*1024;
        printf("Auto-detecting ROM size by reading 4MB and trimming...\n");
    } else {
        size_code = kb_to_size_code(size_kb);
        device_bytes = size_code_to_bytes(size_code);
        total_bytes = size_kb * 1024;
        if (total_bytes > device_bytes) {
            total_bytes = device_bytes;
        }
        printf("Reading %u KB ROM to %s...\n", size_kb, filename);
    }

    /* Initialize device before read */
    if (device_init() < 0) {
        return -1;
    }

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("Error opening output file");
        return -1;
    }

    /* Change ownership immediately after creating the file */
    //fix_file_ownership_fd(fileno(fp));

    uint8_t params[1] = {size_code};
    if (send_command(CMD_READ_ROM, params, 1) < 0) {
        fclose(fp);
        return -1;
    }

    /* Read initial response */
    char text[256];
    read_response(text, sizeof(text), 2000);
    if (!should_filter_message(text)) {
        printf("%s", text);
    }

    /* Read binary data */
    uint8_t buffer[DATA_CHUNK_SIZE];
    uint32_t saved = 0;
    uint32_t device_chunks = device_bytes / DATA_CHUNK_SIZE;

    for (uint32_t i = 0; i < device_chunks && !interrupted; i++) {
        int is_last_chunk = (i == device_chunks - 1);
        int is_near_end = (i >= device_chunks - 3);  /* Last 3 chunks use tolerant reading */
        size_t chunk_bytes_read = 0;
        
        if (is_last_chunk || is_near_end) {
            /* For last few chunks, try to read what we can (more tolerant) */
            size_t remaining = DATA_CHUNK_SIZE;
            int elapsed = 0;
            int poll_interval = POLL_INTERVAL_MS;
            int timeout = is_last_chunk ? 10000 : 8000;  /* Give last chunk more time */
            
            while (chunk_bytes_read < DATA_CHUNK_SIZE && elapsed < timeout) {
                int n = usb_read(buffer + chunk_bytes_read, remaining, poll_interval);
                if (n < 0) {
                    if (chunk_bytes_read > 0) {
                        /* Got some data, that's okay for near-end chunks */
                        break;
                    }
                    /* For last chunk, wait a bit longer before giving up */
                    if (is_last_chunk && elapsed < 5000) {
                        elapsed += poll_interval * 2;
                        continue;
                    }
                    fprintf(stderr, "\nError reading chunk %u (near end)\n", i);
                    fclose(fp);
                    return -1;
                }
                if (n > 0) {
                    chunk_bytes_read += n;
                    remaining -= n;
                    elapsed = 0;
                } else {
                    elapsed += poll_interval;
                    /* If we got partial data, wait a bit longer before accepting */
                    if (chunk_bytes_read > 0) {
                        if (is_last_chunk && elapsed > 2000) {
                            /* Last chunk: wait up to 2 seconds after getting data */
                            break;
                        } else if (elapsed > 1500) {
                            /* Near-end chunks: wait 1.5 seconds */
                            break;
                        }
                    }
                }
            }
            
            if (chunk_bytes_read == 0 && is_last_chunk) {
                /* For last chunk, try one more time with longer timeout */
                usleep(200000);  /* 200ms delay */
                int n = usb_read(buffer, DATA_CHUNK_SIZE, 3000);
                if (n > 0) {
                    chunk_bytes_read = n;
                } else {
                    fprintf(stderr, "\nWarning: got no data for last chunk %u, but continuing...\n", i);
                    /* Don't fail - just continue and trim what we have */
                    break;
                }
            } else if (chunk_bytes_read == 0 && !is_last_chunk) {
                fprintf(stderr, "\nError: got no data for chunk %u\n", i);
                fclose(fp);
                return -1;
            }
            
            /* Write whatever we got */
            if (saved < total_bytes) {
                uint32_t to_write = chunk_bytes_read;
                if (saved + to_write > total_bytes) to_write = total_bytes - saved;
                if (to_write > 0) {
                    fwrite(buffer, 1, to_write, fp);
                    saved += to_write;
                }
            }
        } else {
            /* For earlier chunks, require full read */
            if (read_binary(buffer, DATA_CHUNK_SIZE, 5000) < 0) {
                fprintf(stderr, "\nError reading chunk %u\n", i);
                fclose(fp);
                return -1;
            }

            if (saved < total_bytes) {
                uint32_t to_write = DATA_CHUNK_SIZE;
                if (saved + to_write > total_bytes) to_write = total_bytes - saved;
                fwrite(buffer, 1, to_write, fp);
                saved += to_write;
            }
        }

        printf("\rProgress: %u / %u KB", saved/1024, total_bytes/1024);
        fflush(stdout);
    }

    printf("\n");
    
    /* If no-trim flag is set and we have a specific size, ensure exact size */
    if (no_trim && size_kb > 0) {
        /* Ensure file is exactly the requested size */
        if (saved < total_bytes) {
            /* Pad with 0xFF to reach exact size */
            uint32_t pad_size = total_bytes - saved;
            uint8_t *pad_buffer = malloc(pad_size);
            if (pad_buffer) {
                memset(pad_buffer, 0xFF, pad_size);
                fwrite(pad_buffer, 1, pad_size, fp);
                free(pad_buffer);
            } else {
                /* Fallback: pad byte by byte if malloc fails */
                uint8_t pad_byte = 0xFF;
                for (uint32_t i = saved; i < total_bytes; i++) {
                    fwrite(&pad_byte, 1, 1, fp);
                }
            }
            saved = total_bytes;
        } else if (saved > total_bytes) {
            /* Truncate if somehow we got more (shouldn't happen) */
            ftruncate(fileno(fp), total_bytes);
            saved = total_bytes;
        }
    }
    
    /* Change ownership to real user before closing */
    fflush(fp);
    fsync(fileno(fp));
    fix_file_ownership_fd(fileno(fp));
    fclose(fp);
    
    read_all_responses(2000);
    printf("ROM read complete: %u bytes written to %s\n", saved, filename);

    if (!no_trim) {
        trim_rom_file(filename);
        /* Fix ownership again after trim (trim modifies the file) */
        fix_file_ownership(filename);
    } else if (size_kb > 0) {
        printf("File size preserved at exactly %u KB (no trimming)\n", size_kb);
    }

    return 0;
}

int cmd_read_sram(const char *filename) {
    /* Initialize device before read */
    if (device_init() < 0) {
        return -1;
    }

    uint32_t total_bytes = 32 * 1024;

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("Error opening output file");
        return -1;
    }

    /* Change ownership immediately after creating the file */
    //fix_file_ownership_fd(fileno(fp));

    printf("Reading 32K SRAM to %s...\n", filename);

    uint8_t params[1] = {0x01};
    if (send_command(CMD_READ_SRAM, params, 1) < 0) {
        fclose(fp);
        return -1;
    }

    char text[256];
    read_response(text, sizeof(text), 2000);
    printf("%s", text);

    uint8_t buffer[DATA_CHUNK_SIZE];
    uint32_t received = 0;

    while (received < total_bytes && !interrupted) {
        if (read_binary(buffer, DATA_CHUNK_SIZE, 5000) < 0) {
            fclose(fp);
            return -1;
        }
        fwrite(buffer, 1, DATA_CHUNK_SIZE, fp);
        received += DATA_CHUNK_SIZE;
    }

    /* Change ownership to real user before closing */
    fflush(fp);
    fsync(fileno(fp));
    fix_file_ownership_fd(fileno(fp));
    fclose(fp);
    
    read_all_responses(2000);
    printf("SRAM read complete: %u bytes written to %s\n", received, filename);
    return 0;
}

int cmd_write_rom(const char *filename, uint32_t size_kb) {
    /* Initialize device before write */
    if (device_init() < 0) {
        return -1;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("Error opening ROM file");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        fprintf(stderr, "Invalid file size\n");
        fclose(fp);
        return -1;
    }

    /* If size specified, use that; otherwise use file size */
    uint32_t write_size = (size_kb > 0) ? (size_kb * 1024) : (uint32_t)file_size;
    if (write_size > (uint32_t)file_size) {
        write_size = (uint32_t)file_size;
    }

    printf("Writing %u bytes from %s to flash...\n", write_size, filename);

    uint8_t buffer[DATA_CHUNK_SIZE];
    uint32_t written = 0;
    uint8_t bank = 0;
    uint8_t addj = 0;

    while (written < write_size && !interrupted) {
        size_t to_read = DATA_CHUNK_SIZE;
        if (written + to_read > write_size) {
            to_read = write_size - written;
            memset(buffer, 0xFF, DATA_CHUNK_SIZE);
        }
        if (fread(buffer, 1, to_read, fp) != to_read) {
            fprintf(stderr, "Error reading file\n");
            fclose(fp);
            return -1;
        }

        /* Send all 1024 bytes in one bulk transfer - this is the key! */
        if (usb_write(buffer, DATA_CHUNK_SIZE) < 0) {
            fclose(fp);
            return -1;
        }

        usleep(WRITE_DELAY_US);

        /* Send write command */
        uint8_t params[2] = {addj, bank};
        if (send_command(CMD_WRITE_ROM, params, 2) < 0) {
            fclose(fp);
            return -1;
        }

        /* Wait for acknowledgement */
        char response[256];
        int n = read_response(response, sizeof(response), 5000);
        if (n <= 0) {
            fprintf(stderr, "\nNo response at offset %u\n", written);
            fclose(fp);
            return -1;
        }

        written += DATA_CHUNK_SIZE;
        addj++;
        if (addj >= 64) {
            addj = 0;
            bank++;
        }

        printf("\rProgress: %u / %u bytes", written, write_size);
        fflush(stdout);
    }

    printf("\n");
    fclose(fp);

    /* Send clear buffer to signal end */
    send_command(CMD_CLEAR_BUFFER, NULL, 0);
    read_all_responses(1000);

    printf("ROM write complete: %u bytes written\n", written);
    return 0;
}

int cmd_write_sram(const char *filename) {
    /* Initialize device before write */
    if (device_init() < 0) {
        return -1;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("Error opening SRAM file");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size > 32 * 1024) {
        file_size = 32 * 1024;
        printf("Warning: File truncated to 32K\n");
    }

    printf("Writing %ld bytes from %s to SRAM...\n", file_size, filename);

    uint8_t buffer[DATA_CHUNK_SIZE];
    uint32_t written = 0;
    uint8_t bank = 0;
    uint8_t addj = 0;

    while (written < (uint32_t)file_size && !interrupted) {
        size_t to_read = DATA_CHUNK_SIZE;
        if (written + to_read > (uint32_t)file_size) {
            to_read = file_size - written;
            memset(buffer, 0x00, DATA_CHUNK_SIZE);
        }
        fread(buffer, 1, to_read, fp);

        if (usb_write(buffer, DATA_CHUNK_SIZE) < 0) {
            fclose(fp);
            return -1;
        }

        usleep(WRITE_DELAY_US);

        uint8_t params[2] = {addj, bank};
        if (send_command(CMD_WRITE_SRAM, params, 2) < 0) {
            fclose(fp);
            return -1;
        }

        char response[256];
        read_response(response, sizeof(response), 5000);

        written += DATA_CHUNK_SIZE;
        addj++;
        if (addj >= 64) {
            addj = 0;
            bank++;
        }

        printf("\rProgress: %u / %ld bytes", written, file_size);
        fflush(stdout);
    }

    printf("\n");
    fclose(fp);

    send_command(CMD_CLEAR_BUFFER, NULL, 0);
    read_all_responses(1000);

    printf("SRAM write complete: %u bytes written\n", written);
    return 0;
}

void print_usage(const char *progname) {
    printf("FlashMaster MD - Host Tool (libusb version)\n\n");
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

    /* Get real user ID (the user who ran sudo) */
    /* On macOS/Linux, sudo sets SUDO_UID and SUDO_GID environment variables */
    const char *sudo_uid_str = getenv("SUDO_UID");
    const char *sudo_gid_str = getenv("SUDO_GID");
    
    if (sudo_uid_str && sudo_gid_str) {
        real_uid = (uid_t)atoi(sudo_uid_str);
        real_gid = (gid_t)atoi(sudo_gid_str);
    } else {
        /* Fallback: use getuid() if not running under sudo */
        real_uid = getuid();
        real_gid = getgid();
    }

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
    const char *legacy_command = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose_mode = 1;
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
            /* Legacy command format support */
            if (!legacy_command) {
                legacy_command = argv[i];
            }
        }
    }

    /* Support legacy command format */
    if (legacy_command && !do_read && !do_write && !do_erase) {
        if (usb_open() < 0) {
            return 1;
        }

        int result = 0;
        if (strcmp(legacy_command, "connect") == 0) {
            result = cmd_connect();
        }
        else if (strcmp(legacy_command, "id") == 0) {
            result = cmd_check_id();
        }
        else if (strcmp(legacy_command, "clear") == 0) {
            result = cmd_clear_buffer();
        }
        else {
            fprintf(stderr, "Unknown command: %s\n", legacy_command);
            print_usage(argv[0]);
            result = 1;
        }
        usb_close();
        return result;
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

    if (usb_open() < 0) {
        return 1;
    }

    int result = 0;

    if (do_erase) {
        result = cmd_erase(size_kb);
    }
    else if (do_read) {
        if (!read_file) {
            fprintf(stderr, "Error: -r requires a filename\n");
            result = 1;
        } else {
            result = cmd_read_rom(size_kb, read_file, no_trim);
        }
    }
    else if (do_write) {
        if (!write_file) {
            fprintf(stderr, "Error: -w requires a filename\n");
            result = 1;
        } else {
            result = cmd_write_rom(write_file, size_kb);
        }
    }

    usb_close();
    return result;
}
