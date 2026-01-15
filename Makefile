CC = gcc
CFLAGS = -Wall -Wextra -O2
CFLAGS_USB = $(shell pkg-config --cflags libusb-1.0 2>/dev/null || echo "-I/opt/homebrew/include")
LDFLAGS_USB = $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo "-L/opt/homebrew/lib -lusb-1.0")

# raylib detection (for GUI build)
CFLAGS_RAYLIB = $(shell pkg-config --cflags raylib 2>/dev/null || echo "-I/opt/homebrew/include")
LDFLAGS_RAYLIB = $(shell pkg-config --libs raylib 2>/dev/null || echo "-L/opt/homebrew/lib -lraylib")

# Platform-specific raylib frameworks (macOS)
UNAME_S := $(shell uname -s)
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

.PHONY: all cli gui legacy clean help

# Default: build CLI (backward compatible)
all: cli

# CLI build
cli: $(CLI_TARGET)

$(CLI_TARGET): $(CORE_SRC) $(CLI_SRC)
	$(CC) $(CFLAGS) $(CFLAGS_USB) $(INCLUDES) -o $@ $^ $(LDFLAGS_USB)

# GUI build
gui: $(GUI_TARGET)

$(GUI_TARGET): $(CORE_SRC) $(GUI_SRC)
	$(CC) $(CFLAGS) $(CFLAGS_USB) $(CFLAGS_RAYLIB) $(INCLUDES) -o $@ $^ $(LDFLAGS_USB) $(LDFLAGS_RAYLIB) -lpthread

# Legacy single-file build (original main.c)
legacy: $(LEGACY_TARGET)

$(LEGACY_TARGET): $(LEGACY_SRC)
	$(CC) $(CFLAGS) $(CFLAGS_USB) -o $@ $< $(LDFLAGS_USB)

clean:
	rm -f $(CLI_TARGET) $(GUI_TARGET) $(LEGACY_TARGET)

help:
	@echo "FlashMD Build Targets:"
	@echo "  make cli     - Build command-line version (default)"
	@echo "  make gui     - Build GUI version (requires raylib)"
	@echo "  make legacy  - Build from original main.c"
	@echo "  make clean   - Remove built binaries"
	@echo ""
	@echo "Dependencies:"
	@echo "  CLI: libusb-1.0"
	@echo "  GUI: libusb-1.0, raylib"
	@echo ""
	@echo "Install dependencies:"
	@echo "  macOS:  brew install libusb raylib"
	@echo "  Ubuntu: sudo apt install libusb-1.0-0-dev libraylib-dev"
