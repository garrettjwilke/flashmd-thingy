/*
 * FlashMD GUI Frontend
 * Sega Genesis/Mega Drive ROM Flasher - Graphical Interface
 *
 * Uses raylib + raygui for the UI
 * Uses fork + pipes for privilege separation (GUI runs as user, USB as root)
 */

#include "flashmd_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pwd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <raylib.h>

#ifdef HAVE_LIBPORTAL
#include <libportal/portal.h>
#endif

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include "tinyfiledialogs.h"

/* Window dimensions */
#define WINDOW_WIDTH  680
#define WINDOW_HEIGHT 720

/* Console buffer */
#define CONSOLE_MAX_LINES 100
#define CONSOLE_LINE_LENGTH 256

/* Font path */
#define FONT_PATH "opensans.ttf"
#define FONT_SIZE 18
#define FONT_SIZE_SMALL 16
#define FONT_SIZE_HEADER 20
#define FONT_SIZE_TITLE 24

/* Theme colors */
typedef struct {
    Color background;
    Color panel;
    Color panel_border;
    Color input_bg;
    Color text_primary;
    Color text_secondary;
    Color text_muted;
    Color accent;
    Color success;
    Color warning;
    Color error;
    Color button_bg;
    Color button_hover;
    Color button_text;
    Color progress_bg;
    Color progress_fill;
    Color console_bg;
    Color console_text;
} theme_t;

/* Dark theme */
static const theme_t THEME_DARK = {
    .background     = {30, 32, 38, 255},
    .panel          = {42, 45, 52, 255},
    .panel_border   = {55, 58, 66, 255},
    .input_bg       = {24, 26, 30, 255},
    .text_primary   = {240, 240, 240, 255},
    .text_secondary = {180, 180, 185, 255},
    .text_muted     = {120, 122, 128, 255},
    .accent         = {88, 166, 255, 255},
    .success        = {80, 200, 120, 255},
    .warning        = {255, 180, 60, 255},
    .error          = {255, 90, 90, 255},
    .button_bg      = {55, 60, 70, 255},
    .button_hover   = {70, 75, 88, 255},
    .button_text    = {230, 230, 230, 255},
    .progress_bg    = {24, 26, 30, 255},
    .progress_fill  = {80, 200, 120, 255},
    .console_bg     = {18, 20, 24, 255},
    .console_text   = {200, 205, 210, 255},
};

/* Light theme - high contrast */
static const theme_t THEME_LIGHT = {
    .background     = {242, 243, 245, 255},
    .panel          = {255, 255, 255, 255},
    .panel_border   = {200, 202, 208, 255},
    .input_bg       = {248, 249, 251, 255},
    .text_primary   = {15, 18, 25, 255},
    .text_secondary = {45, 50, 60, 255},
    .text_muted     = {100, 105, 115, 255},
    .accent         = {30, 100, 200, 255},
    .success        = {20, 140, 50, 255},
    .warning        = {200, 120, 0, 255},
    .error          = {200, 40, 50, 255},
    .button_bg      = {228, 230, 235, 255},
    .button_hover   = {215, 218, 225, 255},
    .button_text    = {20, 25, 35, 255},
    .progress_bg    = {220, 222, 228, 255},
    .progress_fill  = {20, 140, 50, 255},
    .console_bg     = {250, 251, 253, 255},
    .console_text   = {25, 30, 40, 255},
};

/* Operation types */
typedef enum {
    OP_NONE = 0,
    OP_CONNECT,
    OP_CHECK_ID,
    OP_ERASE,
    OP_READ_ROM,
    OP_WRITE_ROM,
    OP_READ_SRAM,
    OP_WRITE_SRAM
} operation_t;

/*
 * IPC message types for privilege separation
 * GUI (user) <-> USB handler (root) communication
 */
typedef enum {
    IPC_MSG_COMMAND = 1,    /* GUI -> USB: start operation */
    IPC_MSG_PROGRESS,       /* USB -> GUI: progress update */
    IPC_MSG_LOG,            /* USB -> GUI: log message */
    IPC_MSG_RESULT,         /* USB -> GUI: operation complete */
    IPC_MSG_QUIT            /* GUI -> USB: shutdown */
} ipc_msg_type_t;

/* Command message from GUI to USB handler */
typedef struct {
    ipc_msg_type_t type;
    operation_t operation;
    char filepath[512];
    uint32_t size_kb;
    int no_trim;
    int verbose;
    int full_erase;
} ipc_command_t;

/* Progress message from USB handler to GUI */
typedef struct {
    ipc_msg_type_t type;
    uint32_t current;
    uint32_t total;
} ipc_progress_t;

/* Log message from USB handler to GUI */
typedef struct {
    ipc_msg_type_t type;
    int is_error;
    char message[256];
} ipc_log_t;

/* Result message from USB handler to GUI */
typedef struct {
    ipc_msg_type_t type;
    flashmd_result_t result;
} ipc_result_t;

/* Pipes for IPC (global for signal handler access) */
static int pipe_to_usb[2] = {-1, -1};   /* GUI writes, USB reads */
static int pipe_to_gui[2] = {-1, -1};   /* USB writes, GUI reads */
static pid_t usb_handler_pid = -1;

/* Size options for dropdown */
static const uint32_t size_values[] = {0, 128, 256, 512, 1024, 2048, 4096};

/* Global font */
static Font app_font = {0};
static const theme_t *current_theme = &THEME_DARK;

/* Application state */
typedef struct {
    /* Connection state */
    int device_connected;

    /* ROM operation state */
    char rom_filepath[512];
    int rom_size_index;
    bool no_trim;

    /* SRAM operation state */
    char sram_filepath[512];

    /* Progress */
    float progress_value;
    char progress_text[64];

    /* Console */
    char console_lines[CONSOLE_MAX_LINES][CONSOLE_LINE_LENGTH];
    int console_line_count;
    int console_scroll;

    /* Options */
    bool verbose_mode;
    bool dark_mode;
    bool full_erase;

    /* Dropdown state (for raygui) */
    bool size_dropdown_active;

    /* Operation state */
    int operation_running;
    operation_t current_operation;
    flashmd_result_t operation_result;

    /* Threading */
    pthread_t worker_thread;
    pthread_mutex_t state_mutex;

    /* Operation parameters (for worker thread) */
    char op_filepath[512];
    uint32_t op_size_kb;
    int op_no_trim;
    int op_verbose;
    bool op_full_erase;

    /* Privilege separation mode */
    bool using_ipc;
} gui_state_t;

static gui_state_t state = {0};

/*
 * Thread-safe console logging
 */
static void console_add_line(const char *text) {
    pthread_mutex_lock(&state.state_mutex);

    /* Handle multi-line text */
    const char *start = text;
    const char *end;

    while (*start) {
        end = strchr(start, '\n');
        size_t len = end ? (size_t)(end - start) : strlen(start);

        if (len > 0 || end) {
            if (state.console_line_count < CONSOLE_MAX_LINES) {
                strncpy(state.console_lines[state.console_line_count], start,
                        len < CONSOLE_LINE_LENGTH - 1 ? len : CONSOLE_LINE_LENGTH - 1);
                state.console_lines[state.console_line_count][len < CONSOLE_LINE_LENGTH - 1 ? len : CONSOLE_LINE_LENGTH - 1] = '\0';
                state.console_line_count++;
            } else {
                /* Shift lines up */
                memmove(state.console_lines[0], state.console_lines[1],
                        (CONSOLE_MAX_LINES - 1) * CONSOLE_LINE_LENGTH);
                strncpy(state.console_lines[CONSOLE_MAX_LINES - 1], start,
                        len < CONSOLE_LINE_LENGTH - 1 ? len : CONSOLE_LINE_LENGTH - 1);
                state.console_lines[CONSOLE_MAX_LINES - 1][len < CONSOLE_LINE_LENGTH - 1 ? len : CONSOLE_LINE_LENGTH - 1] = '\0';
            }
        }

        if (end) {
            start = end + 1;
        } else {
            break;
        }
    }

    pthread_mutex_unlock(&state.state_mutex);
}

static void console_clear(void) {
    pthread_mutex_lock(&state.state_mutex);
    state.console_line_count = 0;
    pthread_mutex_unlock(&state.state_mutex);
}

/*
 * Callbacks for core library
 */
static void gui_progress_cb(uint32_t current, uint32_t total, void *user_data) {
    (void)user_data;
    pthread_mutex_lock(&state.state_mutex);
    state.progress_value = (float)current / (float)total;
    snprintf(state.progress_text, sizeof(state.progress_text),
             "%u / %u KB", current / 1024, total / 1024);
    pthread_mutex_unlock(&state.state_mutex);
}

static void gui_message_cb(const char *msg, int is_error, void *user_data) {
    (void)is_error;
    (void)user_data;
    console_add_line(msg);
}

/*
 * Worker thread function
 */
static void *worker_thread_func(void *arg) {
    (void)arg;

    flashmd_config_t config;
    flashmd_config_init(&config);
    config.verbose = state.op_verbose;
    config.no_trim = state.op_no_trim;
    config.progress = gui_progress_cb;
    config.message = gui_message_cb;
    config.user_data = NULL;

    flashmd_result_t result = FLASHMD_OK;

    /* Open USB connection */
    result = flashmd_open();
    if (result != FLASHMD_OK) {
        console_add_line("Error: Could not open USB device");
        console_add_line(flashmd_error_string(result));
        goto done;
    }

    switch (state.current_operation) {
        case OP_CONNECT:
            result = flashmd_connect(&config);
            break;

        case OP_CHECK_ID:
            result = flashmd_check_id(&config);
            break;

        case OP_ERASE:
            {
                uint32_t erase_size = state.op_size_kb;
                if (state.op_full_erase) {
                    erase_size = 0; // Full erase
                } else if (erase_size == 0) { // "Auto" selected, but not full erase
                    erase_size = 4096; // Default to 4MB fast erase
                }
                result = flashmd_erase(erase_size, &config);
            }
            break;

        case OP_READ_ROM:
            result = flashmd_read_rom(state.op_filepath, state.op_size_kb, &config);
            break;

        case OP_WRITE_ROM:
            result = flashmd_write_rom(state.op_filepath, state.op_size_kb, &config);
            break;

        case OP_READ_SRAM:
            result = flashmd_read_sram(state.op_filepath, &config);
            break;

        case OP_WRITE_SRAM:
            result = flashmd_write_sram(state.op_filepath, &config);
            break;

        default:
            break;
    }

    flashmd_close();

done:
    pthread_mutex_lock(&state.state_mutex);
    state.operation_running = 0;
    state.operation_result = result;
    if (result == FLASHMD_OK) {
        state.device_connected = 1;
    }
    pthread_mutex_unlock(&state.state_mutex);

    return NULL;
}

/*
 * IPC callbacks for USB handler process
 */
static int ipc_write_fd = -1;  /* Set by USB handler for callbacks */

static void ipc_progress_cb(uint32_t current, uint32_t total, void *user_data) {
    (void)user_data;
    if (ipc_write_fd < 0) return;

    ipc_progress_t msg = {
        .type = IPC_MSG_PROGRESS,
        .current = current,
        .total = total
    };
    write(ipc_write_fd, &msg, sizeof(msg));
}

static void ipc_message_cb(const char *text, int is_error, void *user_data) {
    (void)user_data;
    if (ipc_write_fd < 0) return;

    ipc_log_t msg = {
        .type = IPC_MSG_LOG,
        .is_error = is_error
    };
    strncpy(msg.message, text, sizeof(msg.message) - 1);
    msg.message[sizeof(msg.message) - 1] = '\0';
    write(ipc_write_fd, &msg, sizeof(msg));
}

/*
 * USB handler process main loop (runs as root)
 * Receives commands from GUI, executes USB operations, sends results back
 */
static void usb_handler_loop(int read_fd, int write_fd) {
    ipc_write_fd = write_fd;  /* For callbacks */

    flashmd_config_t config;
    ipc_command_t cmd;

    while (1) {
        /* Wait for command from GUI */
        ssize_t n = read(read_fd, &cmd, sizeof(cmd));
        if (n <= 0) {
            /* Pipe closed or error - GUI exited */
            break;
        }

        if (cmd.type == IPC_MSG_QUIT) {
            break;
        }

        if (cmd.type != IPC_MSG_COMMAND) {
            continue;
        }

        /* Setup config with IPC callbacks */
        flashmd_config_init(&config);
        config.verbose = cmd.verbose;
        config.no_trim = cmd.no_trim;
        config.progress = ipc_progress_cb;
        config.message = ipc_message_cb;
        config.user_data = NULL;

        flashmd_result_t result = FLASHMD_OK;

        /* Open USB connection */
        result = flashmd_open();
        if (result != FLASHMD_OK) {
            ipc_log_t log_msg = {.type = IPC_MSG_LOG, .is_error = 1};
            snprintf(log_msg.message, sizeof(log_msg.message),
                     "Error: Could not open USB device: %s", flashmd_error_string(result));
            write(write_fd, &log_msg, sizeof(log_msg));
            goto send_result;
        }

        /* Execute operation */
        switch (cmd.operation) {
            case OP_CONNECT:
                result = flashmd_connect(&config);
                break;
            case OP_CHECK_ID:
                result = flashmd_check_id(&config);
                break;
            case OP_ERASE:
                {
                    uint32_t erase_size = cmd.size_kb;
                    if (cmd.full_erase) {
                        erase_size = 0;
                    } else if (erase_size == 0) {
                        erase_size = 4096;
                    }
                    result = flashmd_erase(erase_size, &config);
                }
                break;
            case OP_READ_ROM:
                result = flashmd_read_rom(cmd.filepath, cmd.size_kb, &config);
                break;
            case OP_WRITE_ROM:
                result = flashmd_write_rom(cmd.filepath, cmd.size_kb, &config);
                break;
            case OP_READ_SRAM:
                result = flashmd_read_sram(cmd.filepath, &config);
                break;
            case OP_WRITE_SRAM:
                result = flashmd_write_sram(cmd.filepath, &config);
                break;
            default:
                break;
        }

        flashmd_close();

send_result:
        /* Send result back to GUI */
        {
            ipc_result_t res_msg = {
                .type = IPC_MSG_RESULT,
                .result = result
            };
            write(write_fd, &res_msg, sizeof(res_msg));
        }
    }

    ipc_write_fd = -1;
}

/*
 * Send command to USB handler process via IPC
 */
static void send_ipc_command(operation_t op) {
    ipc_command_t cmd = {
        .type = IPC_MSG_COMMAND,
        .operation = op,
        .size_kb = size_values[state.rom_size_index],
        .no_trim = state.no_trim,
        .verbose = state.verbose_mode,
        .full_erase = state.full_erase
    };

    const char *filepath = (op == OP_READ_SRAM || op == OP_WRITE_SRAM)
                           ? state.sram_filepath : state.rom_filepath;
    strncpy(cmd.filepath, filepath, sizeof(cmd.filepath) - 1);
    cmd.filepath[sizeof(cmd.filepath) - 1] = '\0';

    write(pipe_to_usb[1], &cmd, sizeof(cmd));
}

/*
 * Process incoming IPC messages from USB handler (non-blocking)
 */
static void process_ipc_messages(void) {
    /* Set non-blocking read */
    int flags = fcntl(pipe_to_gui[0], F_GETFL, 0);
    fcntl(pipe_to_gui[0], F_SETFL, flags | O_NONBLOCK);

    while (1) {
        /* Peek at message type */
        ipc_msg_type_t msg_type;
        ssize_t n = read(pipe_to_gui[0], &msg_type, sizeof(msg_type));

        if (n <= 0) {
            /* No more messages */
            break;
        }

        switch (msg_type) {
            case IPC_MSG_PROGRESS:
                {
                    ipc_progress_t msg;
                    msg.type = msg_type;
                    /* Read rest of message */
                    read(pipe_to_gui[0], ((char*)&msg) + sizeof(msg_type),
                         sizeof(msg) - sizeof(msg_type));

                    pthread_mutex_lock(&state.state_mutex);
                    state.progress_value = (float)msg.current / (float)msg.total;
                    snprintf(state.progress_text, sizeof(state.progress_text),
                             "%u / %u KB", msg.current / 1024, msg.total / 1024);
                    pthread_mutex_unlock(&state.state_mutex);
                }
                break;

            case IPC_MSG_LOG:
                {
                    ipc_log_t msg;
                    msg.type = msg_type;
                    read(pipe_to_gui[0], ((char*)&msg) + sizeof(msg_type),
                         sizeof(msg) - sizeof(msg_type));
                    console_add_line(msg.message);
                }
                break;

            case IPC_MSG_RESULT:
                {
                    ipc_result_t msg;
                    msg.type = msg_type;
                    read(pipe_to_gui[0], ((char*)&msg) + sizeof(msg_type),
                         sizeof(msg) - sizeof(msg_type));

                    pthread_mutex_lock(&state.state_mutex);
                    state.operation_running = 0;
                    state.operation_result = msg.result;
                    if (msg.result == FLASHMD_OK) {
                        state.device_connected = 1;
                    }
                    pthread_mutex_unlock(&state.state_mutex);
                }
                break;

            default:
                break;
        }
    }

    /* Restore blocking mode */
    fcntl(pipe_to_gui[0], F_SETFL, flags);
}

/*
 * Start an operation in the worker thread or via IPC
 */
static void start_operation(operation_t op) {
    if (state.operation_running) return;

    state.current_operation = op;
    state.operation_running = 1;
    state.progress_value = 0.0f;
    strcpy(state.progress_text, "Starting...");

    if (state.using_ipc) {
        /* Send command to USB handler process */
        send_ipc_command(op);
    } else {
        /* Copy parameters for worker thread */
        strcpy(state.op_filepath, (op == OP_READ_SRAM || op == OP_WRITE_SRAM)
               ? state.sram_filepath : state.rom_filepath);
        state.op_size_kb = size_values[state.rom_size_index];
        state.op_no_trim = state.no_trim;
        state.op_verbose = state.verbose_mode;
        state.op_full_erase = state.full_erase;

        pthread_create(&state.worker_thread, NULL, worker_thread_func, NULL);
        pthread_detach(state.worker_thread);
    }
}

/*
 * File dialog helpers
 */

#ifdef HAVE_LIBPORTAL
/*
 * Portal-based native file dialogs for Linux
 */
typedef struct {
    char *result_path;
    gboolean done;
    GMainLoop *loop;
} PortalDialogData;

static void portal_open_cb(GObject *source, GAsyncResult *result, gpointer user_data) {
    PortalDialogData *data = user_data;
    XdpPortal *portal = XDP_PORTAL(source);
    GError *error = NULL;

    GVariant *res = xdp_portal_open_file_finish(portal, result, &error);
    if (error) {
        g_error_free(error);
        data->result_path = NULL;
    } else if (res) {
        GVariant *uris_v = g_variant_lookup_value(res, "uris", G_VARIANT_TYPE_STRING_ARRAY);
        if (uris_v) {
            gsize n_uris;
            const gchar **uris = g_variant_get_strv(uris_v, &n_uris);
            if (n_uris > 0 && uris[0]) {
                /* Convert file:// URI to path */
                GFile *file = g_file_new_for_uri(uris[0]);
                data->result_path = g_file_get_path(file);
                g_object_unref(file);
            }
            g_free(uris);
            g_variant_unref(uris_v);
        }
        g_variant_unref(res);
    }

    data->done = TRUE;
    g_main_loop_quit(data->loop);
}

static void portal_save_cb(GObject *source, GAsyncResult *result, gpointer user_data) {
    PortalDialogData *data = user_data;
    XdpPortal *portal = XDP_PORTAL(source);
    GError *error = NULL;

    GVariant *res = xdp_portal_save_file_finish(portal, result, &error);
    if (error) {
        g_error_free(error);
        data->result_path = NULL;
    } else if (res) {
        GVariant *uris_v = g_variant_lookup_value(res, "uris", G_VARIANT_TYPE_STRING_ARRAY);
        if (uris_v) {
            gsize n_uris;
            const gchar **uris = g_variant_get_strv(uris_v, &n_uris);
            if (n_uris > 0 && uris[0]) {
                GFile *file = g_file_new_for_uri(uris[0]);
                data->result_path = g_file_get_path(file);
                g_object_unref(file);
            }
            g_free(uris);
            g_variant_unref(uris_v);
        }
        g_variant_unref(res);
    }

    data->done = TRUE;
    g_main_loop_quit(data->loop);
}

static char *portal_file_dialog(const char *title, const char *default_name,
                                 const char *filter_name, const char **patterns,
                                 int pattern_count, int for_save) {
    /* Ensure XDG_RUNTIME_DIR is set (needed for D-Bus on Linux) */
    if (!getenv("XDG_RUNTIME_DIR")) {
        char runtime_path[128];
        snprintf(runtime_path, sizeof(runtime_path), "/run/user/%d", getuid());
        setenv("XDG_RUNTIME_DIR", runtime_path, 0);
    }

    /* Check if D-Bus session is available */
    if (!getenv("DBUS_SESSION_BUS_ADDRESS")) {
        /* Try to get it from user's runtime dir */
        const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
        if (runtime_dir) {
            char bus_path[512];
            snprintf(bus_path, sizeof(bus_path), "unix:path=%s/bus", runtime_dir);
            setenv("DBUS_SESSION_BUS_ADDRESS", bus_path, 0);
        }
    }

    XdpPortal *portal = xdp_portal_new();
    if (!portal) return NULL;

    PortalDialogData data = {0};
    data.loop = g_main_loop_new(NULL, FALSE);

    /* Build filter */
    GVariantBuilder filter_builder;
    g_variant_builder_init(&filter_builder, G_VARIANT_TYPE("a(sa(us))"));

    GVariantBuilder patterns_builder;
    g_variant_builder_init(&patterns_builder, G_VARIANT_TYPE("a(us)"));
    for (int i = 0; i < pattern_count; i++) {
        g_variant_builder_add(&patterns_builder, "(us)", 0, patterns[i]);
    }
    g_variant_builder_add(&filter_builder, "(s@a(us))", filter_name,
                          g_variant_builder_end(&patterns_builder));

    GVariant *filters = g_variant_builder_end(&filter_builder);

    if (for_save) {
        xdp_portal_save_file(portal, NULL, title, default_name ? default_name : "",
                             NULL, NULL, filters, NULL, NULL,
                             XDP_SAVE_FILE_FLAG_NONE, NULL, portal_save_cb, &data);
    } else {
        xdp_portal_open_file(portal, NULL, title, filters, NULL, NULL,
                             XDP_OPEN_FILE_FLAG_NONE, NULL, portal_open_cb, &data);
    }

    /* Run loop until dialog closes */
    g_main_loop_run(data.loop);
    g_main_loop_unref(data.loop);
    g_object_unref(portal);

    return data.result_path;
}
#endif /* HAVE_LIBPORTAL */

static void open_rom_file_dialog(int for_save) {
    const char *filters[] = {"*.bin", "*.md", "*.gen", "*.smd"};
    char *allocated_result = NULL;
    const char *result = NULL;

#ifdef HAVE_LIBPORTAL
    /* Try portal first */
    allocated_result = portal_file_dialog(
        for_save ? "Save ROM" : "Open ROM",
        for_save ? "dump.bin" : NULL,
        "ROM Files", filters, 4, for_save);
    result = allocated_result;
#endif

    /* Fall back to tinyfiledialogs if portal failed or unavailable */
    if (!result) {
        if (for_save) {
            result = tinyfd_saveFileDialog("Save ROM", "dump.bin", 4, filters, "ROM Files");
        } else {
            result = tinyfd_openFileDialog("Open ROM", "", 4, filters, "ROM Files", 0);
        }
    }

    if (result) {
        strncpy(state.rom_filepath, result, sizeof(state.rom_filepath) - 1);
        state.rom_filepath[sizeof(state.rom_filepath) - 1] = '\0';
    } else {
        state.rom_filepath[0] = '\0';
    }

#ifdef HAVE_LIBPORTAL
    if (allocated_result) g_free(allocated_result);
#endif
}

static void open_sram_file_dialog(int for_save) {
    const char *filters[] = {"*.srm", "*.sav", "*.bin"};
    char *allocated_result = NULL;
    const char *result = NULL;

#ifdef HAVE_LIBPORTAL
    /* Try portal first */
    allocated_result = portal_file_dialog(
        for_save ? "Save SRAM" : "Open SRAM",
        for_save ? "save.srm" : NULL,
        "SRAM Files", filters, 3, for_save);
    result = allocated_result;
#endif

    /* Fall back to tinyfiledialogs if portal failed or unavailable */
    if (!result) {
        if (for_save) {
            result = tinyfd_saveFileDialog("Save SRAM", "save.srm", 3, filters, "SRAM Files");
        } else {
            result = tinyfd_openFileDialog("Open SRAM", "", 3, filters, "SRAM Files", 0);
        }
    }

    if (result) {
        strncpy(state.sram_filepath, result, sizeof(state.sram_filepath) - 1);
        state.sram_filepath[sizeof(state.sram_filepath) - 1] = '\0';
    } else {
        state.sram_filepath[0] = '\0';
    }

#ifdef HAVE_LIBPORTAL
    if (allocated_result) g_free(allocated_result);
#endif
}

/*
 * Draw console box
 */
static void draw_console(Rectangle bounds) {
    DrawRectangleRounded(bounds, 0.01f, 4, current_theme->console_bg);
    DrawRectangleRoundedLinesEx(bounds, 0.01f, 4, 1, current_theme->panel_border);

    pthread_mutex_lock(&state.state_mutex);

    int line_height = 20;
    int visible_lines = (int)(bounds.height - 16) / line_height;
    int start_line = state.console_line_count - visible_lines;
    if (start_line < 0) start_line = 0;

    for (int i = 0; i < visible_lines && (start_line + i) < state.console_line_count; i++) {
        if (app_font.texture.id > 0) {
            DrawTextEx(app_font, state.console_lines[start_line + i],
                      (Vector2){bounds.x + 10, bounds.y + 8 + i * line_height},
                      FONT_SIZE_SMALL, 0, current_theme->console_text);
        } else {
            DrawText(state.console_lines[start_line + i],
                     (int)bounds.x + 10,
                     (int)bounds.y + 8 + i * line_height,
                     FONT_SIZE_SMALL, current_theme->console_text);
        }
    }

    pthread_mutex_unlock(&state.state_mutex);
}

/*
 * Helper: Draw text with custom font
 */
static void draw_text(const char *text, int x, int y, int size, Color color) {
    if (app_font.texture.id > 0) {
        DrawTextEx(app_font, text, (Vector2){(float)x, (float)y}, (float)size, 0, color);
    } else {
        DrawText(text, x, y, size, color);
    }
}

/*
 * Helper: Draw themed button
 */
static bool draw_button(Rectangle bounds, const char *text, bool disabled) {
    bool hover = !disabled && CheckCollisionPointRec(GetMousePosition(), bounds);
    bool pressed = hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    Color bg = disabled ? ColorBrightness(current_theme->button_bg, -0.3f) :
               hover ? current_theme->button_hover : current_theme->button_bg;
    DrawRectangleRounded(bounds, 0.2f, 4, bg);
    DrawRectangleRoundedLinesEx(bounds, 0.2f, 4, 1, current_theme->panel_border);

    /* Center text */
    int text_width = app_font.texture.id > 0
        ? (int)MeasureTextEx(app_font, text, FONT_SIZE, 0).x
        : MeasureText(text, FONT_SIZE);
    int text_x = (int)(bounds.x + (bounds.width - text_width) / 2);
    int text_y = (int)(bounds.y + (bounds.height - FONT_SIZE) / 2);
    Color text_color = disabled ? ColorBrightness(current_theme->button_text, -0.4f) : current_theme->button_text;
    draw_text(text, text_x, text_y, FONT_SIZE, text_color);

    return pressed;
}

/*
 * Helper: Draw themed panel with bold header
 */
static void draw_panel(Rectangle bounds, const char *title) {
    DrawRectangleRounded(bounds, 0.02f, 4, current_theme->panel);
    DrawRectangleRoundedLinesEx(bounds, 0.02f, 4, 1, current_theme->panel_border);
    if (title) {
        /* Draw header text larger and with slight emphasis */
        draw_text(title, (int)bounds.x + 12, (int)bounds.y + 10, FONT_SIZE_HEADER, current_theme->text_primary);
    }
}

/*
 * Helper: Draw themed input field
 */
static void draw_input_field(Rectangle bounds, const char *text, const char *placeholder) {
    DrawRectangleRounded(bounds, 0.15f, 4, current_theme->input_bg);
    DrawRectangleRoundedLinesEx(bounds, 0.15f, 4, 1, current_theme->panel_border);

    const char *display = (text && text[0]) ? text : placeholder;
    Color text_color = (text && text[0]) ? current_theme->text_primary : current_theme->text_muted;

    /* Clip text to bounds */
    draw_text(display, (int)bounds.x + 8, (int)bounds.y + (int)(bounds.height - FONT_SIZE) / 2,
              FONT_SIZE - 2, text_color);
}

/*
 * Helper: Draw checkbox
 */
static bool draw_checkbox(Rectangle bounds, const char *label, bool *checked, bool disabled) {
    bool hover = !disabled && CheckCollisionPointRec(GetMousePosition(), bounds);
    bool clicked = hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    if (clicked) *checked = !(*checked);

    /* Draw box */
    Color box_bg = disabled ? ColorBrightness(current_theme->input_bg, -0.2f) : current_theme->input_bg;
    DrawRectangleRounded(bounds, 0.2f, 4, box_bg);
    DrawRectangleRoundedLinesEx(bounds, 0.2f, 4, 1, current_theme->panel_border);

    /* Draw check mark */
    if (*checked) {
        Color check_color = disabled ? ColorBrightness(current_theme->accent, -0.4f) : current_theme->accent;
        DrawRectangleRounded(
            (Rectangle){bounds.x + 4, bounds.y + 4, bounds.width - 8, bounds.height - 8},
            0.2f, 4, check_color);
    }

    /* Draw label */
    Color label_color = disabled ? current_theme->text_muted : current_theme->text_secondary;
    draw_text(label, (int)(bounds.x + bounds.width + 8), (int)(bounds.y + 2),
              FONT_SIZE, label_color);

    return clicked;
}

/*
 * Helper: Draw dropdown
 */
static bool draw_dropdown(Rectangle bounds, const char **options, int option_count,
                          int *selected, bool *active, bool disabled) {
    bool hover = !disabled && CheckCollisionPointRec(GetMousePosition(), bounds);
    bool clicked = hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
    bool changed = false;

    /* Draw main button */
    Color bg = disabled ? ColorBrightness(current_theme->input_bg, -0.2f) :
               hover ? current_theme->button_hover : current_theme->input_bg;
    DrawRectangleRounded(bounds, 0.15f, 4, bg);
    DrawRectangleRoundedLinesEx(bounds, 0.15f, 4, 1, current_theme->panel_border);

    /* Draw selected text */
    if (*selected >= 0 && *selected < option_count) {
        Color text_color = disabled ? current_theme->text_muted : current_theme->text_primary;
        draw_text(options[*selected], (int)bounds.x + 8,
                  (int)(bounds.y + (bounds.height - FONT_SIZE) / 2),
                  FONT_SIZE, text_color);
    }

    /* Draw arrow */
    int arrow_x = (int)(bounds.x + bounds.width - 20);
    int arrow_y = (int)(bounds.y + bounds.height / 2);
    DrawTriangle(
        (Vector2){(float)arrow_x, (float)(arrow_y - 4)},
        (Vector2){(float)(arrow_x + 10), (float)(arrow_y - 4)},
        (Vector2){(float)(arrow_x + 5), (float)(arrow_y + 4)},
        disabled ? ColorBrightness(current_theme->text_muted, -0.3f) : current_theme->text_muted);

    if (clicked) {
        *active = !(*active);
    }

    /* Draw dropdown list if active */
    if (*active) {
        float list_y = bounds.y + bounds.height + 2;
        float list_height = (float)(option_count * 28);
        DrawRectangleRec((Rectangle){bounds.x, list_y, bounds.width, list_height}, current_theme->panel);

        for (int i = 0; i < option_count; i++) {
            Rectangle item = {bounds.x, list_y + i * 28, bounds.width, 28};
            bool item_hover = CheckCollisionPointRec(GetMousePosition(), item);

            if (item_hover) {
                DrawRectangleRec(item, current_theme->button_hover);
            }

            draw_text(options[i], (int)item.x + 8, (int)(item.y + 6),
                      FONT_SIZE, current_theme->text_primary);

            if (item_hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                *selected = i;
                *active = false;
                changed = true;
            }
        }
        DrawRectangleLinesEx(
            (Rectangle){bounds.x, list_y, bounds.width, list_height},
            1, current_theme->panel_border);
    }

    return changed;
}

/*
 * Main GUI loop
 */
int main(void) {
    /* Initialize mutex */
    pthread_mutex_init(&state.state_mutex, NULL);

    /* Check if we need privilege separation:
     * If running as root (uid 0) and SUDO_UID is set, we fork:
     * - Parent (root): handles USB operations
     * - Child: drops to original user, runs GUI
     */
    const char *sudo_uid_str = getenv("SUDO_UID");
    const char *sudo_gid_str = getenv("SUDO_GID");
    uid_t real_uid = sudo_uid_str ? (uid_t)atoi(sudo_uid_str) : getuid();
    gid_t real_gid = sudo_gid_str ? (gid_t)atoi(sudo_gid_str) : getgid();

#ifdef __linux__
    if (getuid() == 0 && sudo_uid_str && sudo_gid_str) {
        /* Running as root via sudo on Linux - use privilege separation */

        /* Create pipes for IPC */
        if (pipe(pipe_to_usb) < 0 || pipe(pipe_to_gui) < 0) {
            fprintf(stderr, "Failed to create pipes: %s\n", strerror(errno));
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "Failed to fork: %s\n", strerror(errno));
            return 1;
        }

        if (pid > 0) {
            /* Parent process - stays root, handles USB */
            usb_handler_pid = pid;

            /* Close unused pipe ends */
            close(pipe_to_usb[1]);  /* Parent reads from this */
            close(pipe_to_gui[0]);  /* Parent writes to this */

            /* Set up file ownership for created files */
            flashmd_set_real_ids(real_uid, real_gid);

            /* Run USB handler loop */
            usb_handler_loop(pipe_to_usb[0], pipe_to_gui[1]);

            /* Cleanup and wait for child */
            close(pipe_to_usb[0]);
            close(pipe_to_gui[1]);
            waitpid(pid, NULL, 0);

            return 0;
        }

        /* Child process - drops privileges, runs GUI */

        /* Close unused pipe ends */
        close(pipe_to_usb[0]);  /* Child writes to this */
        close(pipe_to_gui[1]);  /* Child reads from this */

        /* Drop privileges back to original user */
        if (setgid(real_gid) < 0 || setuid(real_uid) < 0) {
            fprintf(stderr, "Failed to drop privileges: %s\n", strerror(errno));
            _exit(1);
        }

        /* Mark that we're using IPC mode */
        state.using_ipc = true;
    } else
#endif
    {
        /* Not using privilege separation - normal mode */
        flashmd_set_real_ids(real_uid, real_gid);
        state.using_ipc = false;
    }

    /* Initialize raylib */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "FlashMD - Sega Genesis ROM Flasher");
    SetTargetFPS(60);

    /* Load custom font */
    app_font = LoadFontEx(FONT_PATH, 32, NULL, 0);
    if (app_font.texture.id > 0) {
        SetTextureFilter(app_font.texture, TEXTURE_FILTER_BILINEAR);
    }

    /* Initialize state */
    state.dark_mode = true;
    state.full_erase = false;
    current_theme = &THEME_DARK;

    /* Size options array for dropdown */
    const char *size_opts[] = {"Auto", "128 KB", "256 KB", "512 KB", "1 MB", "2 MB", "4 MB"};
    int size_opt_count = 7;
    Rectangle dropdown_bounds = {0};

    /* Welcome message */
    console_add_line("FlashMD GUI - Ready");
    console_add_line("Connect your FlashMaster MD device and click Connect.");

    /* Main loop */
    while (!WindowShouldClose()) {
        /* Process IPC messages if in privilege separation mode */
        if (state.using_ipc) {
            process_ipc_messages();
        }

        /* Update theme */
        current_theme = state.dark_mode ? &THEME_DARK : &THEME_LIGHT;

        /* Get current state (thread-safe) */
        pthread_mutex_lock(&state.state_mutex);
        int running = state.operation_running;
        float progress = state.progress_value;
        char progress_text[64];
        strcpy(progress_text, state.progress_text);
        pthread_mutex_unlock(&state.state_mutex);

        /* Close dropdown if operation starts */
        if (running) {
            state.size_dropdown_active = false;
        }

        BeginDrawing();
        ClearBackground(current_theme->background);

        int margin = 14;
        int width = GetScreenWidth() - 2 * margin;
        int row_height = 38;

        int y = 14;

        /* ===== Title bar ===== */
        draw_text("FlashMD", margin, y, FONT_SIZE_TITLE, current_theme->text_primary);
        draw_text("Sega Genesis ROM Flasher", margin + 115, y + 6, FONT_SIZE, current_theme->text_muted);

        /* Theme toggle button */
        const char *theme_label = state.dark_mode ? "Light" : "Dark";
        if (draw_button((Rectangle){(float)(width - 60), (float)y, 70, 30}, theme_label, false)) {
            state.dark_mode = !state.dark_mode;
        }
        y += 42;

        /* ===== Device section ===== */
        draw_panel((Rectangle){(float)margin, (float)y, (float)width, 56}, NULL);
        draw_text("Device:", margin + 14, y + 18, FONT_SIZE, current_theme->text_primary);

        const char *status = state.device_connected ? "Connected" : "Not Connected";
        Color status_color = state.device_connected ? current_theme->success : current_theme->warning;
        draw_text(status, margin + 90, y + 18, FONT_SIZE, status_color);

        if (draw_button((Rectangle){(float)(width - 195), (float)(y + 12), 90, 32}, "Connect", state.operation_running)) {
            if (!running) {
                console_add_line("");
                start_operation(OP_CONNECT);
            }
        }
        if (draw_button((Rectangle){(float)(width - 95), (float)(y + 12), 85, 32}, "Check ID", state.operation_running)) {
            if (!running) {
                console_add_line("");
                start_operation(OP_CHECK_ID);
            }
        }
        y += 66;

                /* ===== ROM Operations section ===== */

                int rom_section_y = y;

                int rom_section_height = 170;

                draw_panel((Rectangle){(float)margin, (float)y, (float)width, (float)rom_section_height}, "ROM Operations");

                y += 42;

        

                /* Size and options row */

                int size_row_y = y;

                draw_text("Size:", margin + 14, size_row_y + 8, FONT_SIZE, current_theme->text_primary);

                dropdown_bounds = (Rectangle){(float)(margin + 60), (float)(size_row_y + 4), 145, 30};

                y += row_height + 12;

        

                                        /* ROM action buttons row 1 */

        

                                        if (draw_button((Rectangle){(float)(margin + 14), (float)y, 110, 34}, "Write ROM", running || state.size_dropdown_active)) {

        

                                            if (!running) {

        

                                                open_rom_file_dialog(0);

        

                                                if (state.rom_filepath[0]) {

        

                                                    int confirm = tinyfd_messageBox("Confirm Write", "Are you sure you want to write this ROM?", "yesno", "question", 0);

        

                                                    if (confirm) {

        

                                                        console_add_line("");

        

                                                        start_operation(OP_WRITE_ROM);

        

                                                    }

        

                                                }

        

                                            }

        

                                        }

        

                                        if (draw_button((Rectangle){(float)(margin + 132), (float)y, 110, 34}, "Read ROM", running || state.size_dropdown_active)) {

        

                                            if (!running) {

        

                                                open_rom_file_dialog(1);

        

                                                if (state.rom_filepath[0]) {

        

                                                    int confirm = tinyfd_messageBox("Confirm Read", "Are you sure you want to read the ROM to this file?", "yesno", "question", 0);

        

                                                    if (confirm) {

        

                                                        console_add_line("");

        

                                                        start_operation(OP_READ_ROM);

        

                                                    }

        

                                                }

        

                                            }

        

                                        }

        

                                        draw_checkbox((Rectangle){(float)(margin + 250), (float)(y + 7), 22, 22}, "No trim", &state.no_trim, running || state.size_dropdown_active);

        

                                        y += row_height + 12;

        

                                

        

                                        /* ROM action buttons row 2 */

        

                                        if (draw_button((Rectangle){(float)(margin + 14), (float)y, 110, 34}, "Erase", running || state.size_dropdown_active)) {

        

                                            if (!running) {

        

                                                int confirm = tinyfd_messageBox("Confirm Erase", "Are you sure you want to erase the flash memory?", "yesno", "question", 0);

        

                                                if (confirm) {

        

                                                    console_add_line("");

        

                                                    start_operation(OP_ERASE);

        

                                                }

        

                                            }

        

                                        }

        

                                        draw_checkbox((Rectangle){(float)(margin + 132), (float)(y + 7), 22, 22}, "Full Erase", &state.full_erase, running || state.size_dropdown_active);

        

                y = rom_section_y + rom_section_height + 12;


        


                /* ===== SRAM Operations section ===== */


                int sram_section_height = 115;


                draw_panel((Rectangle){(float)margin, (float)y, (float)width, (float)sram_section_height}, "SRAM Operations");


                y += 36;


        


                        /* SRAM file path row */


        


                        draw_text("File:", margin + 14, y + 8, FONT_SIZE, current_theme->text_primary);


        


                        draw_input_field((Rectangle){(float)(margin + 60), (float)(y + 2), (float)(width - 180), 32},


        


                                         state.sram_filepath, "(no file selected)");


        


                        if (draw_button((Rectangle){(float)(width - 100), (float)(y + 2), 90, 32}, "Browse", running || state.size_dropdown_active)) {


        


                            if (!running) open_sram_file_dialog(0);


        


                        }


        


                        y += row_height + 2;


        


                


        


                        /* SRAM action buttons row */


        


                        if (draw_button((Rectangle){(float)(margin + 14), (float)y, 105, 34}, "Read SRAM", running || state.size_dropdown_active)) {


        


                            if (!running) {


        


                                open_sram_file_dialog(1);


        


                                if (state.sram_filepath[0]) {


        


                                    int confirm = tinyfd_messageBox("Confirm Read", "Are you sure you want to read SRAM?", "yesno", "question", 0);


        


                                    if (confirm) {


        


                                        console_add_line("");


        


                                        start_operation(OP_READ_SRAM);


        


                                    }


        


                                }


        


                            }


        


                        }


        


                        if (draw_button((Rectangle){(float)(margin + 127), (float)y, 105, 34}, "Write SRAM", running || state.size_dropdown_active)) {


        


                            if (!running && state.sram_filepath[0]) {


        


                                int confirm = tinyfd_messageBox("Confirm Write", "Are you sure you want to write SRAM?", "yesno", "question", 0);


        


                                if (confirm) {


        


                                    console_add_line("");


        


                                    start_operation(OP_WRITE_SRAM);


        


                                }


        


                            } else if (!state.sram_filepath[0]) {


        


                                console_add_line("Please select an SRAM file first");


        


                            }


        


                        }


                y += row_height + 20;


        


                /* ===== Progress bar ===== */


                draw_text("Progress:", margin, y + 8, FONT_SIZE, current_theme->text_primary);


                Rectangle prog_bounds = {(float)(margin + 95), (float)(y + 2), (float)(width - 200), 30};


                DrawRectangleRounded(prog_bounds, 0.3f, 4, current_theme->progress_bg);


                if (progress > 0) {


                    Rectangle prog_fill = {prog_bounds.x + 2, prog_bounds.y + 2,


                                           (prog_bounds.width - 4) * progress, prog_bounds.height - 4};


                    DrawRectangleRounded(prog_fill, 0.3f, 4, current_theme->progress_fill);


                }


                draw_text(progress_text, width - 90, y + 8, FONT_SIZE, current_theme->text_secondary);


                y += 42;


        


                /* ===== Console output ===== */


                draw_text("Console Output:", margin, y, FONT_SIZE_HEADER, current_theme->text_primary);


                y += 26;


        


                int console_height = GetScreenHeight() - y - 55;


                if (console_height < 100) console_height = 100;


                draw_console((Rectangle){(float)margin, (float)y, (float)width, (float)console_height});


                y += console_height + 12;


        


                                        /* Bottom buttons */


        


                                        if (draw_button((Rectangle){(float)margin, (float)y, 80, 32}, "Clear", state.size_dropdown_active)) {


        


                                            console_clear();


        


                                        }


        


                                


        


                                                draw_checkbox((Rectangle){(float)(width - 90), (float)(y + 6), 22, 22}, "Verbose", &state.verbose_mode, running || state.size_dropdown_active);


        


                                


        


                                        


        


                                


        


                                                if (running) {


        


                                


        


                                                    draw_text("Working...", width / 2 - 45, y + 8, FONT_SIZE, current_theme->warning);


        


                                


        


                                                }


        


                                


        


                                        


        


                                


        


                                                /* ===== Draw dropdown LAST so it appears on top ===== */


        


                                


        


                                                draw_dropdown(dropdown_bounds, size_opts, size_opt_count, &state.rom_size_index, &state.size_dropdown_active, running);


        


                                


        


                                        


        


                                


        


                                                EndDrawing();


            }

    /* Cleanup */
    if (app_font.texture.id > 0) {
        UnloadFont(app_font);
    }
    CloseWindow();
    pthread_mutex_destroy(&state.state_mutex);

    /* If using IPC, send quit message to USB handler and close pipes */
    if (state.using_ipc) {
        ipc_command_t quit_cmd = {.type = IPC_MSG_QUIT};
        write(pipe_to_usb[1], &quit_cmd, sizeof(quit_cmd));
        close(pipe_to_usb[1]);
        close(pipe_to_gui[0]);
    }

    return 0;
}
