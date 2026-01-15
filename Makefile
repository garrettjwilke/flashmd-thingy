CC = gcc
CFLAGS = -Wall -Wextra -O2
CFLAGS_USB = $(shell pkg-config --cflags libusb-1.0 2>/dev/null || echo "-I/opt/homebrew/include")
LDFLAGS_USB = $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo "-L/opt/homebrew/lib -lusb-1.0")

# Platform detection
UNAME_S := $(shell uname -s)

# raylib configuration - use submodule if present, otherwise system
RAYLIB_SUBMODULE = include/raylib
RAYLIB_SRC = $(RAYLIB_SUBMODULE)/src
RAYLIB_LIB = $(RAYLIB_SRC)/libraylib.a

# Check if submodule exists
ifneq ($(wildcard $(RAYLIB_SRC)/raylib.h),)
    USE_SUBMODULE_RAYLIB = 1
    CFLAGS_RAYLIB = -I$(RAYLIB_SRC)
    LDFLAGS_RAYLIB = $(RAYLIB_LIB)
else
    USE_SUBMODULE_RAYLIB = 0
    CFLAGS_RAYLIB = $(shell pkg-config --cflags raylib 2>/dev/null || echo "-I/opt/homebrew/include")
    LDFLAGS_RAYLIB = $(shell pkg-config --libs raylib 2>/dev/null || echo "-L/opt/homebrew/lib -lraylib")
endif

# Platform-specific raylib frameworks
ifeq ($(UNAME_S),Darwin)
    LDFLAGS_RAYLIB += -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
endif
ifeq ($(UNAME_S),Linux)
    LDFLAGS_RAYLIB += -lGL -lm -lpthread -ldl -lrt -lX11
endif

# Source files
CORE_SRC = src/flashmd_core.c
CLI_SRC = src/flashmd_cli.c
GUI_SRC = src/flashmd_gui.c include/tinyfiledialogs.c
LEGACY_SRC = main.c

# Include paths
INCLUDES = -Isrc -Iinclude

# Targets
CLI_TARGET = flashmd
GUI_TARGET = flashmd-gui
LEGACY_TARGET = flashmd-legacy

.PHONY: all cli gui legacy clean help raylib

# Default: build CLI (backward compatible)
all: cli

# CLI build
cli: $(CLI_TARGET)

$(CLI_TARGET): $(CORE_SRC) $(CLI_SRC)
	$(CC) $(CFLAGS) $(CFLAGS_USB) $(INCLUDES) -o $@ $^ $(LDFLAGS_USB)

# Build raylib from submodule
raylib: $(RAYLIB_LIB)

$(RAYLIB_LIB):
ifeq ($(USE_SUBMODULE_RAYLIB),1)
	@echo "Building raylib from submodule..."
	$(MAKE) -C $(RAYLIB_SRC) PLATFORM=PLATFORM_DESKTOP
else
	@echo "raylib submodule not found, using system raylib"
endif

# GUI build
gui: $(GUI_TARGET)

ifeq ($(USE_SUBMODULE_RAYLIB),1)
$(GUI_TARGET): $(CORE_SRC) $(GUI_SRC) $(RAYLIB_LIB)
else
$(GUI_TARGET): $(CORE_SRC) $(GUI_SRC)
endif
	$(CC) $(CFLAGS) $(CFLAGS_USB) $(CFLAGS_RAYLIB) $(INCLUDES) -o $@ $(CORE_SRC) $(GUI_SRC) $(LDFLAGS_USB) $(LDFLAGS_RAYLIB) -lpthread

# Legacy single-file build (original main.c)
legacy: $(LEGACY_TARGET)

$(LEGACY_TARGET): $(LEGACY_SRC)
	$(CC) $(CFLAGS) $(CFLAGS_USB) -o $@ $< $(LDFLAGS_USB)

clean:
	rm -f $(CLI_TARGET) $(GUI_TARGET) $(LEGACY_TARGET)
ifeq ($(USE_SUBMODULE_RAYLIB),1)
	$(MAKE) -C $(RAYLIB_SRC) clean || true
endif

help:
	@echo "FlashMD Build Targets:"
	@echo "  make cli     - Build command-line version (default)"
	@echo "  make gui     - Build GUI version (builds raylib from submodule if present)"
	@echo "  make raylib  - Build raylib library from submodule"
	@echo "  make legacy  - Build from original main.c"
	@echo "  make clean   - Remove built binaries (and raylib if using submodule)"
	@echo ""
	@echo "Dependencies:"
	@echo "  CLI: libusb-1.0"
	@echo "  GUI: libusb-1.0, raylib (submodule or system)"
	@echo ""
	@echo "Install dependencies:"
	@echo "  macOS:  brew install libusb raylib"
	@echo "  Ubuntu: sudo apt install libusb-1.0-0-dev"
	@echo "          (raylib built from include/raylib submodule)"
	@echo ""
	@echo "Setup submodule (if not already done):"
	@echo "  git submodule update --init --recursive"
