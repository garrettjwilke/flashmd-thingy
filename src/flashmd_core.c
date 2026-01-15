/*
 * FlashMD Core Library
 * Sega Genesis/Mega Drive ROM Flasher - Core Implementation
 */

#include "flashmd_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
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

/* Timing configuration */
#define WRITE_DELAY_US    1000
#define POLL_INTERVAL_MS  30
#define CLEANUP_DELAY_US  100000

/* Message filtering configuration */
static const char *filtered_messages[] = {
    "BUFF IS CLEAR",
    "ROM DUMP START!!!",
    "DUMPER ROM FINISH!!!",
    "PUSH SAVE GAME BUTTON!!!",
    NULL
};

/* Global state */
static libusb_context *ctx = NULL;
static libusb_device_handle *dev_handle = NULL;
static volatile int interrupted = 0;
static uid_t real_uid = -1;
static gid_t real_gid = -1;

/*
 * Internal helper: emit a message via callback or printf
 */
static void emit_msg(const flashmd_config_t *config, int is_error, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (config && config->message) {
        config->message(buf, is_error, config->user_data);
    } else {
        if (is_error) {
            fprintf(stderr, "%s", buf);
        } else {
            printf("%s", buf);
        }
    }
}

/*
 * Internal helper: emit progress via callback
 */
static void emit_progress(const flashmd_config_t *config, uint32_t current, uint32_t total) {
    if (config && config->progress) {
        config->progress(current, total, config->user_data);
    } else {
        printf("\rProgress: %u / %u KB", current/1024, total/1024);
        fflush(stdout);
    }
}

/*
 * Check if a message should be filtered
 */
static int should_filter_message(const flashmd_config_t *config, const char *msg) {
    if (config && config->verbose) {
        return 0;
    }

    if (!msg || !msg[0]) {
        return 0;
    }

    for (int i = 0; filtered_messages[i] != NULL; i++) {
        if (strstr(msg, filtered_messages[i]) != NULL) {
            return 1;
        }
    }

    return 0;
}

/*
 * Print filtered output
 */
static void print_filtered(const flashmd_config_t *config, const char *data, size_t len) {
    if (!data || len == 0) return;

    char *temp = malloc(len + 1);
    if (!temp) {
        emit_msg(config, 0, "%.*s", (int)len, data);
        return;
    }
    memcpy(temp, data, len);
    temp[len] = '\0';

    if (!should_filter_message(config, temp)) {
        emit_msg(config, 0, "%s", temp);
    }

    free(temp);
}

/*
 * File ownership helpers
 */
static void fix_file_ownership_fd(int fd) {
    if (real_uid != (uid_t)-1 && real_gid != (gid_t)-1 && fd >= 0) {
        fchown(fd, real_uid, real_gid);
    }
}

static void fix_file_ownership(const char *filename) {
    if (real_uid != (uid_t)-1 && real_gid != (gid_t)-1) {
        chown(filename, real_uid, real_gid);
    }
}

void flashmd_set_real_ids(int uid, int gid) {
    real_uid = (uid_t)uid;
    real_gid = (gid_t)gid;
}

/*
 * Configuration initialization
 */
void flashmd_config_init(flashmd_config_t *config) {
    if (config) {
        config->verbose = 0;
        config->no_trim = 0;
        config->progress = NULL;
        config->message = NULL;
        config->user_data = NULL;
    }
}

/*
 * Interrupt handling
 */
void flashmd_set_interrupted(int value) {
    interrupted = value;
}

int flashmd_get_interrupted(void) {
    return interrupted;
}

/*
 * USB Operations
 */
static int usb_write(const uint8_t *data, int len) {
    int transferred = 0;
    int r = libusb_bulk_transfer(dev_handle, EP_OUT, (uint8_t *)data, len, &transferred, TIMEOUT_MS);
    if (r < 0) {
        return -1;
    }
    return transferred;
}

static int usb_read(uint8_t *buf, int max_len, int timeout_ms) {
    int transferred = 0;
    int r = libusb_bulk_transfer(dev_handle, EP_IN, buf, max_len, &transferred, timeout_ms);
    if (r == LIBUSB_ERROR_TIMEOUT) {
        return 0;
    }
    if (r < 0) {
        return -1;
    }
    return transferred;
}

flashmd_result_t flashmd_open(void) {
    int r = libusb_init(&ctx);
    if (r < 0) {
        return FLASHMD_ERR_USB_INIT;
    }

    dev_handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
    if (!dev_handle) {
        libusb_exit(ctx);
        ctx = NULL;
        return FLASHMD_ERR_DEVICE_NOT_FOUND;
    }

    if (libusb_kernel_driver_active(dev_handle, CDC_IFACE) == 1) {
        r = libusb_detach_kernel_driver(dev_handle, CDC_IFACE);
        if (r < 0 && r != LIBUSB_ERROR_NOT_SUPPORTED) {
            /* Warning, but continue */
        }
    }

    r = libusb_claim_interface(dev_handle, CDC_IFACE);
    if (r < 0) {
        libusb_close(dev_handle);
        libusb_exit(ctx);
        dev_handle = NULL;
        ctx = NULL;
        return FLASHMD_ERR_CLAIM_INTERFACE;
    }

    return FLASHMD_OK;
}

void flashmd_close(void) {
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

int flashmd_is_open(void) {
    return dev_handle != NULL;
}

/*
 * Command Protocol
 */
static int send_command(uint8_t cmd, const uint8_t *params, size_t param_len) {
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

static int read_response(char *buf, size_t max_len, int timeout_ms) {
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

            if (total > 0 && buf[total-1] == '\n') {
                break;
            }
            elapsed = 0;
        } else {
            elapsed += poll_interval;
        }
    }

    buf[total] = '\0';
    return (int)total;
}

static int read_until_complete(const flashmd_config_t *config, const char *end_pattern, int timeout_ms) {
    char buf[4096];
    size_t acc_len = 0;
    int elapsed = 0;
    int poll_interval = POLL_INTERVAL_MS;

    while (elapsed < timeout_ms) {
        uint8_t temp[512];
        int n = usb_read(temp, sizeof(temp), poll_interval);
        if (n < 0) return -1;
        if (n > 0) {
            print_filtered(config, (const char *)temp, n);

            if (acc_len + n < sizeof(buf) - 1) {
                memcpy(buf + acc_len, temp, n);
                acc_len += n;
                buf[acc_len] = '\0';
            }

            if (strstr(buf, end_pattern)) {
                usleep(CLEANUP_DELAY_US);
                while ((n = usb_read(temp, sizeof(temp), 100)) > 0) {
                    print_filtered(config, (const char *)temp, n);
                }
                return 0;
            }
            elapsed = 0;
        } else {
            elapsed += poll_interval;
        }
    }

    emit_msg(config, 1, "\nTimeout waiting for response\n");
    return -1;
}

static int read_binary(uint8_t *buf, size_t len, int timeout_ms) {
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
        return -1;
    }
    return (int)total;
}

static void read_all_responses(const flashmd_config_t *config, int timeout_ms) {
    uint8_t buf[512];
    int elapsed = 0;
    int poll_interval = POLL_INTERVAL_MS;

    while (elapsed < timeout_ms) {
        int n = usb_read(buf, sizeof(buf), poll_interval);
        if (n > 0) {
            print_filtered(config, (const char *)buf, n);
            elapsed = 0;
        } else {
            elapsed += poll_interval;
        }
    }
}

/*
 * Size conversion utilities
 */
uint32_t flashmd_size_to_bytes(flashmd_size_t size) {
    switch (size) {
        case FLASHMD_SIZE_512K: return 512 * 1024;
        case FLASHMD_SIZE_1M:   return 1024 * 1024;
        case FLASHMD_SIZE_2M:   return 2 * 1024 * 1024;
        case FLASHMD_SIZE_4M:   return 4 * 1024 * 1024;
        case FLASHMD_SIZE_8M:   return 8 * 1024 * 1024;
        default: return 0;
    }
}

flashmd_size_t flashmd_kb_to_size(uint32_t kb) {
    if (kb <= 512) return FLASHMD_SIZE_512K;
    if (kb <= 1024) return FLASHMD_SIZE_1M;
    if (kb <= 2048) return FLASHMD_SIZE_2M;
    if (kb <= 4096) return FLASHMD_SIZE_4M;
    return FLASHMD_SIZE_8M;
}

const char *flashmd_error_string(flashmd_result_t result) {
    switch (result) {
        case FLASHMD_OK: return "Success";
        case FLASHMD_ERR_USB_INIT: return "Failed to initialize USB";
        case FLASHMD_ERR_DEVICE_NOT_FOUND: return "Device not found";
        case FLASHMD_ERR_CLAIM_INTERFACE: return "Could not claim USB interface";
        case FLASHMD_ERR_TIMEOUT: return "Operation timed out";
        case FLASHMD_ERR_IO: return "I/O error";
        case FLASHMD_ERR_FILE: return "File error";
        case FLASHMD_ERR_INTERRUPTED: return "Operation interrupted";
        case FLASHMD_ERR_INVALID_PARAM: return "Invalid parameter";
        default: return "Unknown error";
    }
}

/*
 * Device Commands
 */
flashmd_result_t flashmd_connect(const flashmd_config_t *config) {
    emit_msg(config, 0, "Connecting to FlashMaster MD Dumper...\n");

    if (send_command(CMD_CONNECT, NULL, 0) < 0) {
        return FLASHMD_ERR_IO;
    }

    char response[256];
    if (read_response(response, sizeof(response), 2000) > 0) {
        if (!should_filter_message(config, response)) {
            emit_msg(config, 0, "%s", response);
        }
        if (strstr(response, "connected")) {
            emit_msg(config, 0, "Connection successful!\n");
            return FLASHMD_OK;
        }
    }

    emit_msg(config, 1, "No response from device\n");
    return FLASHMD_ERR_TIMEOUT;
}

flashmd_result_t flashmd_check_id(const flashmd_config_t *config) {
    emit_msg(config, 0, "Reading flash chip ID...\n");
    if (send_command(CMD_CHECK_ID, NULL, 0) < 0) {
        return FLASHMD_ERR_IO;
    }
    read_all_responses(config, 3000);
    return FLASHMD_OK;
}

flashmd_result_t flashmd_clear_buffer(const flashmd_config_t *config) {
    emit_msg(config, 0, "Clearing device buffer...\n");
    if (send_command(CMD_CLEAR_BUFFER, NULL, 0) < 0) {
        return FLASHMD_ERR_IO;
    }
    read_all_responses(config, 2000);
    return FLASHMD_OK;
}

flashmd_result_t flashmd_device_init(const flashmd_config_t *config) {
    flashmd_result_t r;

    r = flashmd_connect(config);
    if (r != FLASHMD_OK) {
        emit_msg(config, 1, "Failed to connect to device\n");
        return r;
    }

    usleep(100000);

    r = flashmd_check_id(config);
    if (r != FLASHMD_OK) {
        emit_msg(config, 1, "Failed to read device ID\n");
        return r;
    }

    usleep(100000);

    r = flashmd_clear_buffer(config);
    if (r != FLASHMD_OK) {
        emit_msg(config, 1, "Failed to clear device buffer\n");
        return r;
    }

    return FLASHMD_OK;
}

/*
 * Flash Operations
 */
flashmd_result_t flashmd_erase(uint32_t size_kb, const flashmd_config_t *config) {
    flashmd_result_t r = flashmd_device_init(config);
    if (r != FLASHMD_OK) {
        return r;
    }

    if (size_kb == 0) {
        emit_msg(config, 0, "Performing full chip erase (this may take 1-2 minutes)...\n");
        if (send_command(CMD_FULL_ERASE, NULL, 0) < 0) {
            return FLASHMD_ERR_IO;
        }
        read_until_complete(config, "SRAM ERASE FINISH", 3000);
        return FLASHMD_OK;
    }

    uint8_t size_code = (uint8_t)flashmd_kb_to_size(size_kb);
    uint32_t erase_bytes = flashmd_size_to_bytes(size_code);

    emit_msg(config, 0, "Erasing %u KB (using %u KB sector)...\n", size_kb, erase_bytes / 1024);
    uint8_t params[1] = {size_code};
    if (send_command(CMD_SECTOR_ERASE, params, 1) < 0) {
        return FLASHMD_ERR_IO;
    }
    read_until_complete(config, "ERASE OK", 5000);
    return FLASHMD_OK;
}

/*
 * Trim trailing 0xFF bytes from a file
 */
static long trim_rom_file(const flashmd_config_t *config, const char *filename) {
    FILE *fp = fopen(filename, "r+b");
    if (!fp) {
        emit_msg(config, 1, "Error opening file for trimming: %s\n", strerror(errno));
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    long file_size = ftell(fp);
    if (file_size <= 0) {
        fclose(fp);
        return file_size;
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
            fclose(fp);
            return -1;
        }

        size_t bytes_read = fread(buffer, 1, chunk_size, fp);
        if (bytes_read < chunk_size && ferror(fp)) {
            fclose(fp);
            return -1;
        }

        if (bytes_read == 0) break;

        for (long i = bytes_read - 1; i >= 0; i--) {
            if (buffer[i] != 0xFF) {
                new_size = seek_to + i + 1;
                goto end_trim_loop;
            }
        }
        pos = seek_to;
    }

    if (pos == 0) {
        new_size = 0;
    }

end_trim_loop:
    if (new_size < file_size) {
        if (ftruncate(fileno(fp), new_size) != 0) {
            emit_msg(config, 1, "\nFailed to trim file\n");
        } else {
            emit_msg(config, 0, "\nROM file trimmed to %ld bytes.\n", new_size);
        }
    } else {
        emit_msg(config, 0, "\nROM file has no trailing 0xFF padding.\n");
    }

    fclose(fp);
    return new_size;
}

flashmd_result_t flashmd_read_rom(const char *filename, uint32_t size_kb,
                                   const flashmd_config_t *config) {
    uint8_t size_code;
    uint32_t total_bytes, device_bytes;

    if (size_kb == 0) {
        size_code = FLASHMD_SIZE_4M;
        total_bytes = 4*1024*1024;
        device_bytes = 4*1024*1024;
        emit_msg(config, 0, "Auto-detecting ROM size by reading 4MB and trimming...\n");
    } else {
        size_code = (uint8_t)flashmd_kb_to_size(size_kb);
        device_bytes = flashmd_size_to_bytes(size_code);
        total_bytes = size_kb * 1024;
        if (total_bytes > device_bytes) {
            total_bytes = device_bytes;
        }
        emit_msg(config, 0, "Reading %u KB ROM to %s...\n", size_kb, filename);
    }

    flashmd_result_t r = flashmd_device_init(config);
    if (r != FLASHMD_OK) {
        return r;
    }

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        emit_msg(config, 1, "Error opening output file: %s\n", strerror(errno));
        return FLASHMD_ERR_FILE;
    }

    uint8_t params[1] = {size_code};
    if (send_command(CMD_READ_ROM, params, 1) < 0) {
        fclose(fp);
        return FLASHMD_ERR_IO;
    }

    char text[256];
    read_response(text, sizeof(text), 2000);
    if (!should_filter_message(config, text)) {
        emit_msg(config, 0, "%s", text);
    }

    uint8_t buffer[DATA_CHUNK_SIZE];
    uint32_t saved = 0;
    uint32_t device_chunks = device_bytes / DATA_CHUNK_SIZE;

    for (uint32_t i = 0; i < device_chunks && !interrupted; i++) {
        int is_last_chunk = (i == device_chunks - 1);
        int is_near_end = (i >= device_chunks - 3);
        size_t chunk_bytes_read = 0;

        if (is_last_chunk || is_near_end) {
            size_t remaining = DATA_CHUNK_SIZE;
            int elapsed = 0;
            int poll_interval = POLL_INTERVAL_MS;
            int timeout = is_last_chunk ? 10000 : 8000;

            while (chunk_bytes_read < DATA_CHUNK_SIZE && elapsed < timeout) {
                int n = usb_read(buffer + chunk_bytes_read, remaining, poll_interval);
                if (n < 0) {
                    if (chunk_bytes_read > 0) break;
                    if (is_last_chunk && elapsed < 5000) {
                        elapsed += poll_interval * 2;
                        continue;
                    }
                    emit_msg(config, 1, "\nError reading chunk %u (near end)\n", i);
                    fclose(fp);
                    return FLASHMD_ERR_IO;
                }
                if (n > 0) {
                    chunk_bytes_read += n;
                    remaining -= n;
                    elapsed = 0;
                } else {
                    elapsed += poll_interval;
                    if (chunk_bytes_read > 0) {
                        if (is_last_chunk && elapsed > 2000) break;
                        else if (elapsed > 1500) break;
                    }
                }
            }

            if (chunk_bytes_read == 0 && is_last_chunk) {
                usleep(200000);
                int n = usb_read(buffer, DATA_CHUNK_SIZE, 3000);
                if (n > 0) {
                    chunk_bytes_read = n;
                } else {
                    break;
                }
            } else if (chunk_bytes_read == 0 && !is_last_chunk) {
                emit_msg(config, 1, "\nError: got no data for chunk %u\n", i);
                fclose(fp);
                return FLASHMD_ERR_IO;
            }

            if (saved < total_bytes) {
                uint32_t to_write = chunk_bytes_read;
                if (saved + to_write > total_bytes) to_write = total_bytes - saved;
                if (to_write > 0) {
                    fwrite(buffer, 1, to_write, fp);
                    saved += to_write;
                }
            }
        } else {
            if (read_binary(buffer, DATA_CHUNK_SIZE, 5000) < 0) {
                emit_msg(config, 1, "\nError reading chunk %u\n", i);
                fclose(fp);
                return FLASHMD_ERR_IO;
            }

            if (saved < total_bytes) {
                uint32_t to_write = DATA_CHUNK_SIZE;
                if (saved + to_write > total_bytes) to_write = total_bytes - saved;
                fwrite(buffer, 1, to_write, fp);
                saved += to_write;
            }
        }

        emit_progress(config, saved, total_bytes);
    }

    if (interrupted) {
        fclose(fp);
        return FLASHMD_ERR_INTERRUPTED;
    }

    emit_msg(config, 0, "\n");

    /* Handle no-trim padding */
    if (config && config->no_trim && size_kb > 0) {
        if (saved < total_bytes) {
            uint32_t pad_size = total_bytes - saved;
            uint8_t *pad_buffer = malloc(pad_size);
            if (pad_buffer) {
                memset(pad_buffer, 0xFF, pad_size);
                fwrite(pad_buffer, 1, pad_size, fp);
                free(pad_buffer);
            }
            saved = total_bytes;
        } else if (saved > total_bytes) {
            ftruncate(fileno(fp), total_bytes);
            saved = total_bytes;
        }
    }

    fflush(fp);
    fsync(fileno(fp));
    fix_file_ownership_fd(fileno(fp));
    fclose(fp);

    read_all_responses(config, 2000);
    emit_msg(config, 0, "ROM read complete: %u bytes written to %s\n", saved, filename);

    if (!config || !config->no_trim) {
        emit_msg(config, 0, "Attempting to trim ROM file...\n");
        trim_rom_file(config, filename);
        fix_file_ownership(filename);
    } else if (size_kb > 0) {
        emit_msg(config, 0, "File size preserved at exactly %u KB (no trimming)\n", size_kb);
    }

    return FLASHMD_OK;
}

flashmd_result_t flashmd_read_sram(const char *filename, const flashmd_config_t *config) {
    flashmd_result_t r = flashmd_device_init(config);
    if (r != FLASHMD_OK) {
        return r;
    }

    uint32_t total_bytes = 32 * 1024;

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        emit_msg(config, 1, "Error opening output file: %s\n", strerror(errno));
        return FLASHMD_ERR_FILE;
    }

    emit_msg(config, 0, "Reading 32K SRAM to %s...\n", filename);

    uint8_t params[1] = {0x01};
    if (send_command(CMD_READ_SRAM, params, 1) < 0) {
        fclose(fp);
        return FLASHMD_ERR_IO;
    }

    char text[256];
    read_response(text, sizeof(text), 2000);
    emit_msg(config, 0, "%s", text);

    uint8_t buffer[DATA_CHUNK_SIZE];
    uint32_t received = 0;

    while (received < total_bytes && !interrupted) {
        if (read_binary(buffer, DATA_CHUNK_SIZE, 5000) < 0) {
            fclose(fp);
            return FLASHMD_ERR_IO;
        }
        fwrite(buffer, 1, DATA_CHUNK_SIZE, fp);
        received += DATA_CHUNK_SIZE;
        emit_progress(config, received, total_bytes);
    }

    if (interrupted) {
        fclose(fp);
        return FLASHMD_ERR_INTERRUPTED;
    }

    fflush(fp);
    fsync(fileno(fp));
    fix_file_ownership_fd(fileno(fp));
    fclose(fp);

    read_all_responses(config, 2000);
    emit_msg(config, 0, "\nSRAM read complete: %u bytes written to %s\n", received, filename);
    return FLASHMD_OK;
}

flashmd_result_t flashmd_write_rom(const char *filename, uint32_t size_kb,
                                    const flashmd_config_t *config) {
    flashmd_result_t r = flashmd_device_init(config);
    if (r != FLASHMD_OK) {
        return r;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        emit_msg(config, 1, "Error opening ROM file: %s\n", strerror(errno));
        return FLASHMD_ERR_FILE;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        emit_msg(config, 1, "Invalid file size\n");
        fclose(fp);
        return FLASHMD_ERR_FILE;
    }

    uint32_t write_size = (size_kb > 0) ? (size_kb * 1024) : (uint32_t)file_size;
    if (write_size > (uint32_t)file_size) {
        write_size = (uint32_t)file_size;
    }

    emit_msg(config, 0, "Writing %u bytes from %s to flash...\n", write_size, filename);

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
            emit_msg(config, 1, "Error reading file\n");
            fclose(fp);
            return FLASHMD_ERR_FILE;
        }

        if (usb_write(buffer, DATA_CHUNK_SIZE) < 0) {
            fclose(fp);
            return FLASHMD_ERR_IO;
        }

        usleep(WRITE_DELAY_US);

        uint8_t params[2] = {addj, bank};
        if (send_command(CMD_WRITE_ROM, params, 2) < 0) {
            fclose(fp);
            return FLASHMD_ERR_IO;
        }

        char response[256];
        int n = read_response(response, sizeof(response), 5000);
        if (n <= 0) {
            emit_msg(config, 1, "\nNo response at offset %u\n", written);
            fclose(fp);
            return FLASHMD_ERR_TIMEOUT;
        }

        written += DATA_CHUNK_SIZE;
        addj++;
        if (addj >= 64) {
            addj = 0;
            bank++;
        }

        emit_progress(config, written, write_size);
    }

    if (interrupted) {
        fclose(fp);
        return FLASHMD_ERR_INTERRUPTED;
    }

    emit_msg(config, 0, "\n");
    fclose(fp);

    send_command(CMD_CLEAR_BUFFER, NULL, 0);
    read_all_responses(config, 1000);

    emit_msg(config, 0, "ROM write complete: %u bytes written\n", written);
    return FLASHMD_OK;
}

flashmd_result_t flashmd_write_sram(const char *filename, const flashmd_config_t *config) {
    flashmd_result_t r = flashmd_device_init(config);
    if (r != FLASHMD_OK) {
        return r;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        emit_msg(config, 1, "Error opening SRAM file: %s\n", strerror(errno));
        return FLASHMD_ERR_FILE;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size > 32 * 1024) {
        file_size = 32 * 1024;
        emit_msg(config, 0, "Warning: File truncated to 32K\n");
    }

    emit_msg(config, 0, "Writing %ld bytes from %s to SRAM...\n", file_size, filename);

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
            return FLASHMD_ERR_IO;
        }

        usleep(WRITE_DELAY_US);

        uint8_t params[2] = {addj, bank};
        if (send_command(CMD_WRITE_SRAM, params, 2) < 0) {
            fclose(fp);
            return FLASHMD_ERR_IO;
        }

        char response[256];
        read_response(response, sizeof(response), 5000);

        written += DATA_CHUNK_SIZE;
        addj++;
        if (addj >= 64) {
            addj = 0;
            bank++;
        }

        emit_progress(config, written, (uint32_t)file_size);
    }

    if (interrupted) {
        fclose(fp);
        return FLASHMD_ERR_INTERRUPTED;
    }

    emit_msg(config, 0, "\n");
    fclose(fp);

    send_command(CMD_CLEAR_BUFFER, NULL, 0);
    read_all_responses(config, 1000);

    emit_msg(config, 0, "SRAM write complete: %u bytes written\n", written);
    return FLASHMD_OK;
}
