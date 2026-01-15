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

    # Check for libportal (native file dialogs on Linux)
    HAVE_PORTAL := $(shell pkg-config --exists libportal 2>/dev/null && echo 1 || echo 0)
    ifeq ($(HAVE_PORTAL),1)
        CFLAGS_PORTAL = $(shell pkg-config --cflags libportal) -DHAVE_LIBPORTAL
        LDFLAGS_PORTAL = $(shell pkg-config --libs libportal)
    else
        CFLAGS_PORTAL =
        LDFLAGS_PORTAL =
    endif
endif

# Source files
CORE_SRC = src/flashmd_core.c
CLI_SRC = src/flashmd_cli.c
GUI_SRC = src/flashmd_gui.c include/tinyfiledialogs.c
QT_SRC = src/flashmd_qt.cpp
LEGACY_SRC = main.c

# Include paths
INCLUDES = -Isrc -Iinclude

# Qt configuration
ifeq ($(UNAME_S),Darwin)
    # macOS with Homebrew - try Qt6 first, then Qt5
    QT_MOC = $(shell find /opt/homebrew/Cellar/qtbase -name "moc" -type f 2>/dev/null | head -1)
    QT_CFLAGS = $(shell pkg-config --cflags Qt6Widgets 2>/dev/null || pkg-config --cflags Qt5Widgets 2>/dev/null)
    QT_LDFLAGS = $(shell pkg-config --libs Qt6Widgets 2>/dev/null || pkg-config --libs Qt5Widgets 2>/dev/null)
else
    # Linux - try Qt5 first (more common), then Qt6
    QT_CFLAGS = $(shell pkg-config --cflags Qt5Widgets 2>/dev/null || pkg-config --cflags Qt6Widgets 2>/dev/null)
    QT_LDFLAGS = $(shell pkg-config --libs Qt5Widgets 2>/dev/null || pkg-config --libs Qt6Widgets 2>/dev/null)
    QT_MOC = $(shell which moc-qt5 2>/dev/null || which moc 2>/dev/null || echo "moc")
endif

# Targets
CLI_TARGET = flashmd
GUI_TARGET = flashmd-gui
QT_TARGET = flashmd-qt
LEGACY_TARGET = flashmd-legacy

.PHONY: all cli gui gui-qt legacy clean help raylib

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
	$(CC) $(CFLAGS) $(CFLAGS_USB) $(CFLAGS_RAYLIB) $(CFLAGS_PORTAL) $(INCLUDES) -o $@ $(CORE_SRC) $(GUI_SRC) $(LDFLAGS_USB) $(LDFLAGS_RAYLIB) $(LDFLAGS_PORTAL) -lpthread

# Legacy single-file build (original main.c)
legacy: $(LEGACY_TARGET)

$(LEGACY_TARGET): $(LEGACY_SRC)
	$(CC) $(CFLAGS) $(CFLAGS_USB) -o $@ $< $(LDFLAGS_USB)

# Qt GUI build
gui-qt: $(QT_TARGET)

src/moc_flashmd_qt.cpp: $(QT_SRC)
	$(QT_MOC) $(QT_SRC) -o src/moc_flashmd_qt.cpp

src/flashmd_core_qt.o: $(CORE_SRC)
	$(CC) $(CFLAGS) $(CFLAGS_USB) $(INCLUDES) -c -o $@ $(CORE_SRC)

$(QT_TARGET): src/flashmd_core_qt.o $(QT_SRC) src/moc_flashmd_qt.cpp
	g++ -std=c++17 $(CFLAGS) $(CFLAGS_USB) $(QT_CFLAGS) $(INCLUDES) -fPIC -o $@ src/flashmd_core_qt.o $(QT_SRC) $(LDFLAGS_USB) $(QT_LDFLAGS)

clean:
	rm -f $(CLI_TARGET) $(GUI_TARGET) $(QT_TARGET) $(LEGACY_TARGET) src/moc_flashmd_qt.cpp src/flashmd_core_qt.o
ifeq ($(USE_SUBMODULE_RAYLIB),1)
	$(MAKE) -C $(RAYLIB_SRC) clean || true
endif

help:
	@echo "FlashMD Build Targets:"
	@echo "  make cli     - Build command-line version (default)"
	@echo "  make gui     - Build raylib GUI version"
	@echo "  make gui-qt  - Build Qt GUI version (recommended for Linux)"
	@echo "  make raylib  - Build raylib library from submodule"
	@echo "  make legacy  - Build from original main.c"
	@echo "  make clean   - Remove built binaries"
	@echo ""
	@echo "Dependencies:"
	@echo "  CLI:    libusb-1.0"
	@echo "  gui:    libusb-1.0, raylib"
	@echo "  gui-qt: libusb-1.0, Qt5"
	@echo ""
	@echo "Install dependencies:"
	@echo "  macOS:  brew install libusb raylib qt@5"
	@echo "  Linux:  sudo apt install libusb-1.0-0-dev qtbase5-dev"
	@echo ""
	@echo "Running on Linux:"
	@echo "  sudo ./flashmd-qt   (Qt handles USB permissions gracefully)"
