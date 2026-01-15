/*
 * FlashMD Core Library
 * Sega Genesis/Mega Drive ROM Flasher - Core API
 *
 * This provides a callback-based interface for both CLI and GUI frontends.
 */

#ifndef FLASHMD_CORE_H
#define FLASHMD_CORE_H

#include <stdint.h>
#include <stddef.h>

/* Size codes for ROM operations */
typedef enum {
    FLASHMD_SIZE_512K = 0x01,
    FLASHMD_SIZE_1M   = 0x02,
    FLASHMD_SIZE_2M   = 0x03,
    FLASHMD_SIZE_4M   = 0x04,
    FLASHMD_SIZE_8M   = 0x05
} flashmd_size_t;

/* Operation result codes */
typedef enum {
    FLASHMD_OK = 0,
    FLASHMD_ERR_USB_INIT = -1,
    FLASHMD_ERR_DEVICE_NOT_FOUND = -2,
    FLASHMD_ERR_CLAIM_INTERFACE = -3,
    FLASHMD_ERR_TIMEOUT = -4,
    FLASHMD_ERR_IO = -5,
    FLASHMD_ERR_FILE = -6,
    FLASHMD_ERR_INTERRUPTED = -7,
    FLASHMD_ERR_INVALID_PARAM = -8
} flashmd_result_t;

/*
 * Progress callback - called during read/write operations
 * Parameters:
 *   current   - bytes processed so far
 *   total     - total bytes to process
 *   user_data - user-provided context pointer
 */
typedef void (*flashmd_progress_cb)(uint32_t current, uint32_t total, void *user_data);

/*
 * Message callback - called for status and error messages
 * Parameters:
 *   msg       - message text (may include newlines)
 *   is_error  - 1 if error message, 0 if status message
 *   user_data - user-provided context pointer
 */
typedef void (*flashmd_message_cb)(const char *msg, int is_error, void *user_data);

/*
 * Configuration structure - passed to all operations
 */
typedef struct {
    int verbose;                    /* Show filtered messages (1) or filter them (0) */
    int no_trim;                    /* Don't trim 0xFF bytes from read files */
    flashmd_progress_cb progress;   /* Progress callback (NULL = no progress) */
    flashmd_message_cb message;     /* Message callback (NULL = use printf) */
    void *user_data;                /* User data passed to callbacks */
} flashmd_config_t;

/*
 * Initialize configuration with defaults
 */
void flashmd_config_init(flashmd_config_t *config);

/*
 * USB Connection Management
 */

/* Open USB connection to device */
flashmd_result_t flashmd_open(void);

/* Close USB connection */
void flashmd_close(void);

/* Check if device is open */
int flashmd_is_open(void);

/*
 * Interrupt Handling
 */

/* Set interrupted flag (call from signal handler) */
void flashmd_set_interrupted(int value);

/* Get interrupted flag */
int flashmd_get_interrupted(void);

/*
 * Device Commands
 */

/* Connect/ping device */
flashmd_result_t flashmd_connect(const flashmd_config_t *config);

/* Read flash chip ID */
flashmd_result_t flashmd_check_id(const flashmd_config_t *config);

/* Clear device buffer */
flashmd_result_t flashmd_clear_buffer(const flashmd_config_t *config);

/* Initialize device (connect + check_id + clear_buffer) */
flashmd_result_t flashmd_device_init(const flashmd_config_t *config);

/*
 * Flash Operations
 */

/* Erase flash memory
 * size_kb = 0 for full erase, or specify KB to erase */
flashmd_result_t flashmd_erase(uint32_t size_kb, const flashmd_config_t *config);

/* Read ROM to file
 * size_kb = 0 for auto-detect (read 4MB and trim) */
flashmd_result_t flashmd_read_rom(const char *filename, uint32_t size_kb,
                                   const flashmd_config_t *config);

/* Write ROM from file
 * size_kb = 0 to use file size */
flashmd_result_t flashmd_write_rom(const char *filename, uint32_t size_kb,
                                    const flashmd_config_t *config);

/* Read SRAM (32KB) to file */
flashmd_result_t flashmd_read_sram(const char *filename, const flashmd_config_t *config);

/* Write SRAM from file */
flashmd_result_t flashmd_write_sram(const char *filename, const flashmd_config_t *config);

/*
 * Utility Functions
 */

/* Get error string for result code */
const char *flashmd_error_string(flashmd_result_t result);

/* Convert size code to bytes */
uint32_t flashmd_size_to_bytes(flashmd_size_t size);

/* Convert KB to size code */
flashmd_size_t flashmd_kb_to_size(uint32_t kb);

/*
 * File Ownership (for sudo compatibility)
 */

/* Set real user/group IDs for file ownership */
void flashmd_set_real_ids(int uid, int gid);

#endif /* FLASHMD_CORE_H */
